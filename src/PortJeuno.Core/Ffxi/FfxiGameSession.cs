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

    public FfxiGameSession(FfxiHuffman? codec = null) => _codec = codec;

    /// <summary>Raised for every chat message received.</summary>
    public event Action<FfxiChatLine>? ChatReceived;

    /// <summary>Raised after each batch of entity updates, with the current snapshot.</summary>
    public event Action<IReadOnlyList<FfxiEntityUpdate>>? EntitiesChanged;

    /// <summary>Raised with a human-readable note whenever the session's state changes.</summary>
    public event Action<string>? Status;

    public FfxiZoneLoginReply? ZoneState { get; private set; }
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
        Status?.Invoke($"In {(ZoneState is null ? "zone" : $"zone {ZoneState.ZoneNo}")} as {Handoff.CharacterName}.");
    }

    /// <summary>
    /// Starts the position heartbeat that keeps the session alive. The
    /// character stands still: this client has no terrain height, and the
    /// server persists position, so moving without it risks writing a bad
    /// position to the character record.
    /// </summary>
    public Task StartHeartbeatAsync()
    {
        if (_zone is null || _zoneEndpoint is null || ZoneState is null)
        {
            throw new InvalidOperationException("Call ConnectToZoneAsync first.");
        }

        _holdCts = new CancellationTokenSource();
        _holdTask = _zone.HoldWithPositionAsync(
            _zoneEndpoint,
            x: ZoneState.X,
            vertical: ZoneState.Vertical,
            depth: ZoneState.Depth,
            direction: ZoneState.Direction,
            duration: TimeSpan.FromHours(12),
            interval: TimeSpan.FromMilliseconds(400),
            onReply: OnReply,
            ct: _holdCts.Token);

        return Task.CompletedTask;
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

            if (FfxiEntityUpdate.IsEntityUpdate(id))
            {
                sawEntity = true;
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

        await _zone.SendEncryptedAsync(_zoneEndpoint, FfxiChatPacket.Build(kind, message, 1));
    }

    public async Task TellAsync(string recipient, string message)
    {
        if (_zone is null || _zoneEndpoint is null)
        {
            return;
        }

        await _zone.SendEncryptedAsync(_zoneEndpoint, FfxiTellPacket.Build(recipient, message, 1));
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
