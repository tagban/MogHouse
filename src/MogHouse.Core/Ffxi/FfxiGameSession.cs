using System.Net;

namespace MogHouse.Core.Ffxi;

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
    private readonly string? _zoneDataDirectory;

    /// <summary>Zone lines for the current zone, if the data was available.</summary>
    public IReadOnlyList<FfxiZoneLine> ZoneLines { get; private set; } = [];

    /// <param name="navMeshDirectory">
    /// Where the server's `.nav` files live. Optional: without it the character
    /// can still move, but at a fixed height and with nothing stopping it
    /// walking off a ledge.
    /// </param>
    /// <param name="zoneDataDirectory">
    /// The server's `data/zones` directory, which holds each zone's zone lines.
    /// Optional: without it the character can walk over a zone line and nothing
    /// happens, because only the client ever initiates a zone change.
    /// </param>
    public FfxiGameSession(FfxiHuffman? codec = null, string? navMeshDirectory = null, string? zoneDataDirectory = null)
    {
        _codec = codec;
        _navMeshDirectory = navMeshDirectory;
        _zoneDataDirectory = zoneDataDirectory;
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

    /// <summary>The route currently being walked, if any. World-space waypoints.</summary>
    public IReadOnlyList<(float X, float Vertical, float Depth)> CurrentPath { get; private set; } = [];

    private CancellationTokenSource? _walkCts;

    /// <summary>
    /// Walks to a point using the server's own navmesh for routing, so the path
    /// goes around walls rather than into them.
    ///
    /// Cancels any walk already in progress: a second destination means the
    /// first is no longer wanted.
    /// </summary>
    public async Task WalkToAsync(float targetX, float targetDepth)
    {
        if (NavMesh is null)
        {
            Status?.Invoke("No navmesh for this zone - cannot route.");
            return;
        }

        _walkCts?.Cancel();
        _walkCts = new CancellationTokenSource();
        CancellationToken ct = _walkCts.Token;

        if (!NavMesh.TryGetGroundHeight(targetX, PosVertical, targetDepth, out float targetVertical))
        {
            Status?.Invoke("Nothing walkable there.");
            return;
        }

        IReadOnlyList<(float X, float Vertical, float Depth)> route =
            NavMesh.FindPath(PosX, PosVertical, PosDepth, targetX, targetVertical, targetDepth);

        if (route.Count == 0)
        {
            Status?.Invoke("No route to there.");
            return;
        }

        CurrentPath = route;
        Status?.Invoke($"Walking {route.Count} waypoints to ({targetX:F1}, {targetDepth:F1}).");

        try
        {
            // Skip the first waypoint - it's where we already are.
            for (int i = 1; i < route.Count && !ct.IsCancellationRequested; i++)
            {
                (float wx, _, float wz) = route[i];

                // Step toward each corner at a walking pace rather than
                // teleporting: the server tracks movement and a jump would
                // look like a speed hack.
                while (!ct.IsCancellationRequested)
                {
                    float dx = wx - PosX;
                    float dz = wz - PosDepth;
                    float distance = MathF.Sqrt((dx * dx) + (dz * dz));

                    if (distance < 0.5f)
                    {
                        break;
                    }

                    float step = MathF.Min(StepDistance, distance);
                    Move(dx / distance * step, dz / distance * step);

                    await Task.Delay(StepInterval, ct);
                }
            }
        }
        catch (OperationCanceledException)
        {
            // A newer destination replaced this walk; nothing to report.
        }

        CurrentPath = [];
    }

    /// <summary>Stops any walk in progress.</summary>
    public void StopWalking()
    {
        _walkCts?.Cancel();
        CurrentPath = [];
    }

    /// <summary>Roughly a normal running pace once combined with the interval below.</summary>
    private const float StepDistance = 0.6f;
    private static readonly TimeSpan StepInterval = TimeSpan.FromMilliseconds(120);

    /// <summary>Steps the character horizontally and turns it to face the direction of travel.</summary>
    /// <summary>
    /// Reports where the character actually is, rather than asking to be moved.
    ///
    /// <see cref="Move"/> walks against the navmesh, which is a coarse thing
    /// built for pathfinding. A renderer has the zone's own collision mesh and
    /// has already decided what the character could and could not walk into, so
    /// it says where it ended up and this follows. The zone-line check still
    /// runs - walking across one is the whole point of knowing where you are.
    /// </summary>
    public void PlaceAt(float x, float vertical, float depth, sbyte facing)
    {
        bool shifted = x != PosX || vertical != PosVertical || depth != PosDepth;

        PosX = x;
        PosVertical = vertical;
        PosDepth = depth;
        Facing = facing;

        if (!shifted)
        {
            return;
        }

        Moved?.Invoke();
        _ = CheckZoneLinesAsync();
    }

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
            // Raycast along the path rather than testing the destination:
            // a destination test finds the floor on the far side of a wall and
            // waves the move through.
            if (NavMesh.TryMove(PosX, PosVertical, PosDepth, targetX, targetZ,
                                out float newX, out float newVertical, out float newZ, out bool blocked))
            {
                LastMoveBlocked = blocked;
                PosX = newX;
                PosVertical = newVertical;
                PosDepth = newZ;
                Facing = (sbyte)(Math.Atan2(-dx, -dz) * (128.0 / Math.PI));
                Moved?.Invoke();
                _ = CheckZoneLinesAsync();
                return;
            }

            // Not on the mesh at all - fall through and move anyway rather
            // than freezing the character somewhere it can never leave.
        }

        PosX = targetX;
        PosDepth = targetZ;

        // FFXI packs a full turn into one signed byte, so a heading is
        // atan2 scaled by 256/2pi rather than by 360.
        Facing = (sbyte)(Math.Atan2(-dx, -dz) * (128.0 / Math.PI));
        Moved?.Invoke();
        _ = CheckZoneLinesAsync();
    }
    public FfxiZoneHandoff? Handoff { get; private set; }
    public uint OwnCharId => Handoff?.ContentId ?? 0;

    /// <summary>Everything we have been told about, for drawing or listing.</summary>
    public IReadOnlyList<FfxiEntityUpdate> KnownEntities() => _zone?.KnownEntities() ?? [];

    /// <summary>How many entities we currently know about. Diagnostic.</summary>
    public int KnownEntityCount => KnownEntities().Count;
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

        // Adopt the reported position before loading zone lines: the arrival
        // guard compares against where we are, and at this point the heartbeat
        // has not started to set it.
        if (ZoneState is not null)
        {
            PosX = ZoneState.X;
            PosVertical = ZoneState.Vertical;
            PosDepth = ZoneState.Depth;
            Facing = ZoneState.Direction;
        }

        // Without this the server never sends the initialization batch, so the
        // character arrives knowing nothing about itself.
        await _zone.SendGameOkAsync(_zoneEndpoint, ct);

        TryLoadNavMesh();
        TryLoadZoneLines();
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
            TryLoadZoneLines();
            ZoneChanged?.Invoke(ZoneState.ZoneNo);
            Status?.Invoke($"Now in zone {ZoneState.ZoneNo}.");
        }
    }

    /// <summary>Which zone line we have already asked about, so one step does not fire a dozen requests.</summary>
    private uint? _zoneLineRequested;

    private void TryLoadZoneLines()
    {
        ZoneLines = [];
        _zoneLineRequested = null;

        if (_zoneDataDirectory is null || ZoneState is null)
        {
            return;
        }

        string? zoneName = FfxiZoneNames.Get(ZoneState.ZoneNo);
        if (zoneName is null)
        {
            return;
        }

        // Zone data directories are the zone name lowercased.
        string path = Path.Combine(_zoneDataDirectory, zoneName.ToLowerInvariant(), "zone.yaml");

        try
        {
            ZoneLines = FfxiZoneLineReader.Read(path);
            if (ZoneLines.Count > 0)
            {
                Status?.Invoke($"{ZoneLines.Count} zone lines loaded for {zoneName}.");
            }

            // Zoning drops us onto the arrival zone line, which is the return
            // trip. Treat whichever line we land inside as already requested,
            // so we do not bounce straight back where we came from.
            foreach (FfxiZoneLine line in ZoneLines)
            {
                if (line.DistanceSquaredTo(PosX, PosDepth) <= line.Radius * line.Radius)
                {
                    _zoneLineRequested = line.Id;
                    break;
                }
            }
        }
        catch (Exception ex)
        {
            Status?.Invoke($"Zone lines for {zoneName} could not be read: {ex.Message}");
        }
    }

    /// <summary>
    /// Asks the server to zone if we are standing on a zone line. Only the
    /// client ever starts a zone change - the server waits to be asked - so
    /// without this a character can walk over a zone line all day and stay put.
    /// </summary>
    private async Task CheckZoneLinesAsync()
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null || ZoneLines.Count == 0 || _pendingZone is not null)
        {
            return;
        }

        FfxiZoneLine? touching = null;
        foreach (FfxiZoneLine candidate in ZoneLines)
        {
            if (candidate.DistanceSquaredTo(PosX, PosDepth) <= candidate.Radius * candidate.Radius)
            {
                touching = candidate;
                break;
            }
        }

        // Clear the guard once clear of every line, so the same one can be
        // used again on a later crossing.
        if (touching is null)
        {
            _zoneLineRequested = null;
            return;
        }

        foreach (FfxiZoneLine line in new[] { touching })
        {
            if (_zoneLineRequested == line.Id)
            {
                return;
            }

            Status?.Invoke($"Touched zone line {line.Token} to {line.Destination} - requesting zone change.");

            // One request per touch. The heartbeat calls this several times a
            // second, and the server takes a moment to answer, so without a
            // guard a single step onto a line fires a dozen requests.
            _zoneLineRequested = line.Id;
            await _zone.SendZoneLineAsync(_zoneEndpoint, line.Id, PosX, PosVertical, PosDepth, ZoneState.ActIndex);
            return;
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
