using System.Net;

namespace PortJeuno.Core.Ffxi;

/// <summary>A chat line as the UI wants it: who, what, and which channel.</summary>
public sealed record FfxiChatLine(DateTimeOffset At, FfxiChatMessageType Kind, string Sender, string Text);

/// <summary>
/// Drives a whole session - auth, roster, character select, zone connection,
/// and the position heartbeat - and surfaces the parts a user interface cares
/// about: chat, nearby entities, and where we are.
///
/// This exists so the UI never has to know the protocol. Everything here is
/// already covered by the lower-level clients; the value added is sequencing,
/// keeping the connection alive, and raising events on arrival rather than
/// making a caller poll.
/// </summary>
public sealed class FfxiGameSession : IDisposable
{
    private readonly FfxiHuffman? _codec;
    private FfxiRosterClient? _roster;
    private FfxiZoneClient? _zone;
    private IPEndPoint? _zoneEndpoint;
    private CancellationTokenSource? _holdCts;
    private Task? _holdTask;

    private readonly string? _navMeshDirectory;

    /// <param name="navMeshDirectory">
    /// Where the server's `.nav` files live. Optional: without it the character
    /// can still move, but at a fixed height and with nothing stopping it
    /// walking off a ledge.
    /// </param>
    public FfxiGameSession(FfxiHuffman? codec = null, string? navMeshDirectory = null)
    {
        _codec = codec;
        _navMeshDirectory = navMeshDirectory;
    }

    /// <summary>Raised for every chat message received.</summary>
    public event Action<FfxiChatLine>? ChatReceived;

    /// <summary>Raised after each batch of entity updates, with the current snapshot.</summary>
    public event Action<IReadOnlyList<FfxiEntityUpdate>>? EntitiesChanged;

    /// <summary>Raised with a human-readable note whenever the session's state changes.</summary>
    public event Action<string>? Status;

    public FfxiZoneLoginReply? ZoneState { get; private set; }

    /// <summary>
    /// Where we currently tell the server we are. Movement is horizontal only:
    /// the vertical stays at whatever the server reported on zone-in, because
    /// this client has no terrain height. Other players still see the
    /// character correctly, since the game client draws them on the ground -
    /// but the server persists position, so walking far from the starting
    /// ground level can leave a height in the character record that puts a
    /// real client underground next time it logs in there.
    /// </summary>
    public float PosX { get; private set; }
    public float PosVertical { get; private set; }
    public float PosDepth { get; private set; }
    public sbyte Facing { get; private set; }

    /// <summary>Raised after the position changes, so a UI can redraw.</summary>
    public event Action? Moved;

    /// <summary>
    /// The zone's navmesh, when one was found. With it, movement follows the
    /// ground and refuses to leave walkable surfaces - which is collision, for
    /// free, since walls and ledges are simply absent from the mesh. Without
    /// it, movement is horizontal-only at a fixed height.
    /// </summary>
    public FfxiNavMesh? NavMesh { get; private set; }

    /// <summary>True when the last move was refused because the destination was off the navmesh.</summary>
    public bool LastMoveBlocked { get; private set; }

    /// <summary>Steps the character horizontally and turns it to face the direction of travel.</summary>
    public void Move(float dx, float dz)
    {
        if (dx == 0 && dz == 0)
        {
            return;
        }

        float targetX = PosX + dx;
        float targetZ = PosDepth + dz;
        LastMoveBlocked = false;

        if (NavMesh is not null)
        {
            if (!NavMesh.TryGetGroundHeight(targetX, PosVertical, targetZ, out float ground))
            {
                // Off the walkable surface - a wall, a ledge, or thin air.
                // Refusing beats walking into it and being written to the
                // character record somewhere invalid.
                LastMoveBlocked = true;
                return;
            }

            PosVertical = ground;
        }

        PosX = targetX;
        PosDepth = targetZ;

        // FFXI packs a full turn into one signed byte, so a heading is
        // atan2 scaled by 256/2pi rather than by 360.
        Facing = (sbyte)(Math.Atan2(-dx, -dz) * (128.0 / Math.PI));
        Moved?.Invoke();
    }
    public FfxiZoneHandoff? Handoff { get; private set; }
    public uint OwnCharId => Handoff?.ContentId ?? 0;
    public bool IsConnected => _holdTask is { IsCompleted: false };

    /// <summary>Authenticates and returns the character roster.</summary>
    public async Task<(FfxiLoginResponse Login, IReadOnlyList<FfxiCharacter> Characters)> LoginAsync(
        FfxiServerProfile profile, string? otp = null, CancellationToken ct = default)
    {
        Status?.Invoke($"Connecting to {profile.Host}:{profile.AuthPort}...");

        var client = new FfxiLoginClient();
        (FfxiLoginResponse login, IReadOnlyList<FfxiCharacter> characters, FfxiRosterClient? roster) =
            await client.LoginAsync(profile, otp, ct: ct);

        _roster = roster;
        Status?.Invoke(login.Result == FfxiLoginResult.Success
            ? $"Logged in - {characters.Count(c => !string.IsNullOrWhiteSpace(c.Name))} character(s)."
            : $"Login failed: {login.Result}");

        return (login, characters);
    }

    /// <summary>
    /// Selects a character and connects to its zone server, completing the
    /// zone-in handshake. Movement is deliberately not started - see
    /// <see cref="StartHeartbeatAsync"/>.
    /// </summary>
    public async Task ConnectToZoneAsync(FfxiCharacter character, byte[] sessionHash, string host, CancellationToken ct = default)
    {
        if (_roster is null)
        {
            throw new InvalidOperationException("Call LoginAsync first.");
        }

        Status?.Invoke($"Selecting {character.Name}...");
        Handoff = await _roster.SelectCharacterAsync(character, sessionHash, ct);

        string zoneHost = FfxiRosterClient.FormatIpAddress(Handoff.ZoneServerIp);
        if (zoneHost == "0.0.0.0")
        {
            zoneHost = host;
        }

        _zoneEndpoint = new IPEndPoint(IPAddress.Parse(zoneHost), (int)Handoff.ZoneServerPort);
        _zone = new FfxiZoneClient(Handoff, _codec);

        Status?.Invoke($"Connecting to zone server {_zoneEndpoint}...");
        FfxiZoneReply? reply = await _zone.LoginAsync(
            _zoneEndpoint, Handoff.ContentId, Handoff.CharacterName, "", clientVersion: 99, clientLanguage: 2, ct: ct);

        if (reply?.Plaintext is null)
        {
            throw new InvalidOperationException("Zone server did not answer, or its reply could not be decoded.");
        }

        foreach ((ushort id, int offset, int size) in FfxiZonePacket.EnumerateSubPackets(reply.Plaintext))
        {
            if (id == FfxiZoneLoginReply.PacketId && size == FfxiZoneLoginReply.PacketSize)
            {
                ZoneState = FfxiZoneLoginReply.Parse(reply.Plaintext.AsSpan(offset, size));
            }
        }

        // Without this the server never sends the initialization batch, so the
        // character arrives knowing nothing about itself.
        await _zone.SendGameOkAsync(_zoneEndpoint, ct);

        TryLoadNavMesh();
        Status?.Invoke($"In {(ZoneState is null ? "zone" : $"zone {ZoneState.ZoneNo}")} as {Handoff.CharacterName}.");
    }

    /// <summary>
    /// Starts the position heartbeat that keeps the session alive, reporting
    /// whatever <see cref="PosX"/>/<see cref="PosDepth"/> currently say - so
    /// <see cref="Move"/> steers a live character.
    /// </summary>
    public Task StartHeartbeatAsync()
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null)
        {
            throw new InvalidOperationException("Call ConnectToZoneAsync first.");
        }

        PosX = ZoneState.X;
        PosVertical = ZoneState.Vertical;
        PosDepth = ZoneState.Depth;
        Facing = ZoneState.Direction;

        _holdCts = new CancellationTokenSource();
        _holdTask = RunSessionAsync(_holdCts.Token);
        return Task.CompletedTask;
    }

    /// <summary>
    /// Keeps the heartbeat running, and carries the session across zone
    /// changes. A zone change ends one heartbeat and starts another against a
    /// different server, so the loop is the natural shape: hold until
    /// something interrupts, and if what interrupted was a handoff, follow it.
    /// </summary>
    private async Task RunSessionAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested && _zone is not null && _zoneEndpoint is not null)
        {
            _zoneCts = CancellationTokenSource.CreateLinkedTokenSource(ct);

            await _zone.HoldWithPositionAsync(
                _zoneEndpoint,
                x: PosX,
                vertical: PosVertical,
                depth: PosDepth,
                direction: Facing,
                duration: TimeSpan.FromHours(12),
                interval: TimeSpan.FromMilliseconds(400),
                onReply: OnReply,
                positionProvider: () => (PosX, PosVertical, PosDepth, Facing),
                ct: _zoneCts.Token);

            IPEndPoint? destination = _pendingZone;
            _pendingZone = null;

            if (destination is null || ct.IsCancellationRequested)
            {
                break;
            }

            try
            {
                await CompleteZoneChangeAsync(destination);
            }
            catch (Exception ex)
            {
                Status?.Invoke($"Zone change failed: {ex.Message}");
                break;
            }
        }
    }

    private CancellationTokenSource? _zoneCts;

    /// <summary>
    /// How far the server's idea of our position may differ from ours before we
    /// treat it as a teleport rather than ordinary lag. Comfortably larger than
    /// a movement step, comfortably smaller than any real warp.
    /// </summary>
    private const float TeleportThreshold = 5.0f;

    private void AdoptServerPosition(FfxiEntityUpdate self)
    {
        float dx = self.X - PosX;
        float dz = self.Depth - PosDepth;

        if ((dx * dx) + (dz * dz) < TeleportThreshold * TeleportThreshold)
        {
            return;
        }

        PosX = self.X;
        PosVertical = self.Vertical;
        PosDepth = self.Depth;
        Facing = self.Direction;

        Status?.Invoke($"Moved by the server to x {PosX:F1}  y {PosVertical:F1}  z {PosDepth:F1}.");
        Moved?.Invoke();
    }

    /// <summary>Raised when the server moves us to a different zone.</summary>
    public event Action<uint>? ZoneChanged;

    /// <summary>
    /// Handles GP_SERV_COMMAND_LOGOUT. Despite the name, state 2 is an
    /// ordinary zone change - walking a zone line, or a GM teleporting us
    /// somewhere else. The server rotates its Blowfish key straight after
    /// sending this, so ignoring it means talking to the wrong address with
    /// the wrong key, which from the outside looks like being logged off for
    /// no reason.
    /// </summary>
    private void HandleZoneTransition(FfxiZoneTransition transition)
    {
        if (_zone is null)
        {
            return;
        }

        if (!transition.IsZoneChange)
        {
            Status?.Invoke($"Server ended the session: {transition.State}.");
            _holdCts?.Cancel();
            return;
        }

        string ip = FfxiRosterClient.FormatIpAddress(transition.ZoneServerIp);
        if (ip == "0.0.0.0" && _zoneEndpoint is not null)
        {
            ip = _zoneEndpoint.Address.ToString();
        }

        _pendingZone = new IPEndPoint(IPAddress.Parse(ip), (int)transition.ZoneServerPort);
        Status?.Invoke($"Zoning to {_pendingZone}...");

        // Ends the current heartbeat so RunSessionAsync can pick the handoff up.
        _zoneCts?.Cancel();
    }

    private IPEndPoint? _pendingZone;

    /// <summary>
    /// Completes a zone change: rotate the key the way the server just did,
    /// restart the counters, and introduce ourselves to the new zone server
    /// with a fresh 0x00A and GAMEOK.
    /// </summary>
    private async Task CompleteZoneChangeAsync(IPEndPoint destination)
    {
        if (_zone is null || Handoff is null)
        {
            return;
        }

        _zone.AdvanceKey();
        _zone.ResetCountersForNewZone();
        _zoneEndpoint = destination;

        FfxiZoneReply? reply = await _zone.LoginAsync(
            destination, Handoff.ContentId, Handoff.CharacterName, "", clientVersion: 99, clientLanguage: 2);

        if (reply?.Plaintext is null)
        {
            Status?.Invoke("New zone server did not answer.");
            return;
        }

        foreach ((ushort id, int offset, int size) in FfxiZonePacket.EnumerateSubPackets(reply.Plaintext))
        {
            if (id == FfxiZoneLoginReply.PacketId && size == FfxiZoneLoginReply.PacketSize)
            {
                ZoneState = FfxiZoneLoginReply.Parse(reply.Plaintext.AsSpan(offset, size));
            }
        }

        await _zone.SendGameOkAsync(destination);

        if (ZoneState is not null)
        {
            PosX = ZoneState.X;
            PosVertical = ZoneState.Vertical;
            PosDepth = ZoneState.Depth;
            Facing = ZoneState.Direction;
            NavMesh = null;
            TryLoadNavMesh();
            ZoneChanged?.Invoke(ZoneState.ZoneNo);
            Status?.Invoke($"Now in zone {ZoneState.ZoneNo}.");
        }
    }

    private void TryLoadNavMesh()
    {
        if (_navMeshDirectory is null || ZoneState is null)
        {
            return;
        }

        string? zoneName = FfxiZoneNames.Get(ZoneState.ZoneNo);
        if (zoneName is null)
        {
            Status?.Invoke($"No name known for zone {ZoneState.ZoneNo} - movement will not follow terrain.");
            return;
        }

        try
        {
            NavMesh = FfxiNavMesh.TryLoadZone(_navMeshDirectory, zoneName);
            Status?.Invoke(NavMesh is null
                ? $"No navmesh for {zoneName} - movement will not follow terrain."
                : $"Navmesh loaded for {zoneName} - movement follows the ground.");
        }
        catch (Exception ex)
        {
            Status?.Invoke($"Navmesh for {zoneName} could not be read: {ex.Message}");
        }
    }

    private void OnReply(FfxiZoneReply reply)
    {
        if (reply.Plaintext is null)
        {
            return;
        }

        bool sawEntity = false;

        foreach ((ushort id, int offset, int size) in FfxiZonePacket.EnumerateSubPackets(reply.Plaintext))
        {
            FfxiChatMessage? chat = FfxiChatMessage.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (chat is not null && chat.Text.Length > 0)
            {
                ChatReceived?.Invoke(new FfxiChatLine(DateTimeOffset.Now, chat.Kind, chat.Sender, chat.Text));
            }

            FfxiZoneTransition? transition = FfxiZoneTransition.TryParse(reply.Plaintext.AsSpan(offset, size));
            if (transition is not null)
            {
                HandleZoneTransition(transition);
            }

            if (FfxiEntityUpdate.IsEntityUpdate(id))
            {
                sawEntity = true;

                // The server is authoritative for anything that moves us
                // without us asking - a GM !bring, a warp, a knockback. If it
                // places us somewhere far from where we have been claiming to
                // be, adopt its answer; otherwise our next heartbeat would
                // simply assert the old position and undo the teleport.
                FfxiEntityUpdate? self = FfxiEntityUpdate.TryParse(reply.Plaintext.AsSpan(offset, size));
                if (self is not null && self.UniqueNo == OwnCharId)
                {
                    AdoptServerPosition(self);
                }
            }
        }

        if (sawEntity && _zone is not null)
        {
            EntitiesChanged?.Invoke(_zone.KnownEntities());
        }
    }

    public async Task SayAsync(string message, FfxiChatKind kind = FfxiChatKind.Say)
    {
        if (_zone is null || _zoneEndpoint is null)
        {
            return;
        }

        await _zone.SendChatAsync(_zoneEndpoint, kind, message);
    }

    public async Task TellAsync(string recipient, string message)
    {
        if (_zone is null || _zoneEndpoint is null)
        {
            return;
        }

        await _zone.SendTellAsync(_zoneEndpoint, recipient, message);
    }

    /// <summary>Asks the server to log out, then stops the heartbeat.</summary>
    public async Task LogoutAsync()
    {
        if (_zone is not null && _zoneEndpoint is not null)
        {
            try
            {
                await _zone.SendLogoutAsync(_zoneEndpoint);
                Status?.Invoke("Logout requested.");
            }
            catch (Exception ex)
            {
                Status?.Invoke($"Logout request failed: {ex.Message}");
            }
        }

        await _holdCts?.CancelAsync()!;
    }

    public void Dispose()
    {
        _holdCts?.Cancel();
        _holdCts?.Dispose();
        _zone?.Dispose();
        _roster?.Dispose();
    }
}
