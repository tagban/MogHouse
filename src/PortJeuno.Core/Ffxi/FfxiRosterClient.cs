using System.Buffers.Binary;
using System.Net.Sockets;
using System.Text;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// The post-auth data_session (54230) + view_session (54001) dance that
/// fetches a character roster. Grounded in LandSandBoat/server's
/// src/login/data_session.cpp and view_session.cpp (branch `base`,
/// 2026-08-28):
///
///  0. Both data_session and view_session only register themselves into
///     the shared session_t (session.data_session / session.view_session)
///     the *first* time each socket's read_func runs - it happens
///     unconditionally at the top of read_func, before the opcode switch.
///     Until data_session has registered, view_session's own 0x1F handler
///     finds session.data_session null and silently bails (sends an error
///     packet back on the VIEW socket instead - which nothing here reads -
///     rather than ever pushing to the data socket). So the client must
///     put *something* on the DATA socket first, before touching the view
///     socket at all. Opcode 0xFE ("Reply with nothing to keep xiloader
///     spinning" per data_session.cpp's own comment) is used for this - it
///     does nothing server-side beyond the registration every opcode
///     causes, and its comment strongly suggests it's exactly what real
///     xiloader sends as an early no-op/keepalive.
///  1. Client sends an "acquire player data" request (view opcode 0x1F) on
///     the VIEW socket.
///  2. Server pushes a bare 5-byte `{0x01,0,0,0,0}` on the DATA socket
///     (view_session.cpp's case 0x1F handler writes this to the *data*
///     session, unprompted).
///  3. Client replies on the DATA socket with opcode 0xA1 (account id +
///     the client's own reported IP + the session hash).
///  4. Server answers 0xA1 with two separate things: a fixed 328-byte
///     compact list on the DATA socket (IDs only, byte[0]=0x03), and -
///     separately, on the still-open VIEW socket - the full lpkt_chr_info2
///     packet this class actually parses (data_session.cpp lines ~280-304).
///
/// Step 0 was added 2026-08-28 after a live test against a real server
/// (ffxi.cc) hung forever waiting for step 2's push - login itself
/// succeeded, confirming the auth layer, but the roster dance never got a
/// response. Cross-checked atom0s's XiPackets docs (the real retail
/// protocol LandSandBoat's own packet structs were adapted from) for
/// RequestGetChr (C2S 0x001F): the real client's version of this packet is
/// 44 bytes, with an extra 16-byte `passwd` field this class doesn't send -
/// but view_session.cpp's actual 0x1F handler never reads anything past
/// the opcode and the session-hash identifier, so that field appears to be
/// retail-protocol-only baggage LandSandBoat's own server doesn't check,
/// not the missing piece. The data_session-not-yet-registered theory is a
/// still-unverified best guess for the real hang cause - this fix has not
/// yet been re-tested against a real server.
/// </summary>
public sealed class FfxiRosterClient : IDisposable
{
    private const byte ViewOpcodeAcquirePlayerData = 0x1F;
    private const byte ViewOpcodeSelectCharacter = 0x07;
    private const byte DataOpcodeCharacterListRequest = 0xA1;
    private const byte DataOpcodeConfirmSelection = 0xA2;
    private const byte DataOpcodeKeepAlive = 0xFE;
    private const byte DataOpcodePush = 0x01;
    private const byte DataOpcodeSelectPush = 0x02;

    private const int PacketNameLength = 16; // "15 characters + null terminator" per data_session.cpp's own comment.
    private const int OffsetSelectCharId = 28;
    private const int OffsetSelectCharName = 36;
    private const int ViewSelectRequestSize = OffsetSelectCharName + PacketNameLength; // 52

    // lpkt_next_login (login_packets.h): packet_t header(28) + ffxi_id(4) +
    // ffxi_id_world(4) + character_name(16) + server_id(4) + server_ip(4) +
    // server_port(4) + cache_ip(4) + cache_port(4) = 0x48 (72), matching the
    // server's own characterSelectionResponse.packet_size assignment.
    private const int ZoneHandoffSize = 72;
    private const int OffsetHandoffContentId = 28;
    private const int OffsetHandoffCharIdWorld = 32;
    private const int OffsetHandoffCharacterName = 36;
    private const int OffsetHandoffServerId = 52;
    private const int OffsetHandoffServerIp = 56;
    private const int OffsetHandoffServerPort = 60;
    private const int OffsetHandoffCacheIp = 64;
    private const int OffsetHandoffCachePort = 68;

    // TC_OPERATION_MAKE offsets, relative to the start of one character record.
    private const int RecordSize = 140;
    private const int OffsetContentId = 0;
    private const int OffsetCharIdMain = 4;
    private const int OffsetCharacterName = 12;
    private const int CharacterNameLength = 16;
    private const int OffsetWorldName = 28;
    private const int WorldNameLength = 16;
    private const int OffsetJobBlock = 44; // start of TC_OPERATION_MAKE
    private const int OffsetRace = OffsetJobBlock + 0;
    private const int OffsetMainJob = OffsetJobBlock + 2;
    private const int OffsetSubJob = OffsetJobBlock + 3;
    private const int OffsetZone = OffsetJobBlock + 28;
    private const int OffsetMainJobLevel = OffsetJobBlock + 29;

    private readonly TcpClient _data = new();
    private readonly TcpClient _view = new();

    public async Task<IReadOnlyList<FfxiCharacter>> FetchCharactersAsync(string host, uint accountId, byte[] sessionHash, int dataPort = FfxiConstants.DataPort, int viewPort = FfxiConstants.ViewPort, CancellationToken ct = default)
    {
        if (sessionHash.Length != FfxiConstants.PacketIdentifierLength)
        {
            throw new ArgumentException($"Session hash must be {FfxiConstants.PacketIdentifierLength} bytes.", nameof(sessionHash));
        }

        await _data.ConnectAsync(host, dataPort, ct);
        await _view.ConnectAsync(host, viewPort, ct);

        NetworkStream dataStream = _data.GetStream();
        NetworkStream viewStream = _view.GetStream();

        // Registers session.data_session server-side before we ask the view
        // socket for anything that depends on it existing - see the class
        // remarks (step 0).
        await dataStream.WriteAsync(BuildDataKeepAliveRequest(sessionHash), ct);

        await viewStream.WriteAsync(BuildViewAcquirePlayerDataRequest(sessionHash), ct);

        var pushBuffer = new byte[16];
        int pushRead = await dataStream.ReadAsync(pushBuffer, ct);
        if (pushRead < 1 || pushBuffer[0] != DataOpcodePush)
        {
            throw new InvalidOperationException($"Expected a 0x{DataOpcodePush:X2} push from the data session, got {(pushRead < 1 ? "nothing" : $"0x{pushBuffer[0]:X2}")}.");
        }

        // The client's own address as the server sees it isn't known to us
        // client-side; 0 is what an unresolvable/unused value looks like in
        // the reference source's own examples. Revisit if a real server
        // rejects this.
        await dataStream.WriteAsync(BuildDataCharacterListRequest(accountId, serverIp: 0, sessionHash), ct);

        var ackBuffer = new byte[512];
        _ = await dataStream.ReadAsync(ackBuffer, ct); // the compact 328-byte uList reply - superseded by the richer view-socket packet below, not parsed here.

        var viewBuffer = new byte[FfxiConstants.PacketHeaderSize + 4 + RecordSize * 16];
        int viewRead = await viewStream.ReadAsync(viewBuffer, ct);
        return ParseCharacters(viewBuffer.AsSpan(0, viewRead));
    }

    /// <summary>
    /// Selects a character from the roster FetchCharactersAsync already
    /// returned, reusing the same still-open data/view sockets (the real
    /// protocol runs both steps over one continuous session, not separate
    /// connections). Sequence (view_session.cpp/data_session.cpp, case
    /// 0x07/0xA2): client sends 0x07 on the view socket (char id + name) ->
    /// server pushes a bare 0x02 on the data socket -> client confirms with
    /// 0xA2 on the data socket -> server sends the lpkt_next_login handoff
    /// on the view socket and then closes it ("Client waits for us to close
    /// the socket" per the server's own comment) - this class doesn't try
    /// to read anything from the view socket afterward.
    ///
    /// 0xA2's request carries a 20-byte `key3` field at offset 1 ("some
    /// kind of magic regarding the blowfish keys" per the server's own
    /// comment) that deliberately overlaps bytes 12-20 of the mandatory
    /// session-hash region every data-socket packet needs (getHashFromPacket
    /// always reads offset 12 regardless of opcode) - the login server only
    /// stores this blob for a *later* zone-server handshake this project
    /// hasn't implemented, so its content doesn't matter yet; left zeroed
    /// beyond the session hash it's forced to contain.
    ///
    /// NOT live-tested - the roster fetch above is confirmed against a real
    /// server, this next step isn't yet.
    /// </summary>
    public async Task<FfxiZoneHandoff> SelectCharacterAsync(FfxiCharacter character, byte[] sessionHash, CancellationToken ct = default)
    {
        NetworkStream dataStream = _data.GetStream();
        NetworkStream viewStream = _view.GetStream();

        await viewStream.WriteAsync(BuildViewSelectCharacterRequest(character.ContentId, character.Name, sessionHash), ct);

        var pushBuffer = new byte[16];
        int pushRead = await dataStream.ReadAsync(pushBuffer, ct);
        if (pushRead < 1 || pushBuffer[0] != DataOpcodeSelectPush)
        {
            throw new InvalidOperationException($"Expected a 0x{DataOpcodeSelectPush:X2} push from the data session, got {(pushRead < 1 ? "nothing" : $"0x{pushBuffer[0]:X2}")}.");
        }

        await dataStream.WriteAsync(BuildDataConfirmSelectionRequest(sessionHash), ct);

        var viewBuffer = new byte[ZoneHandoffSize];
        int viewRead = await viewStream.ReadAsync(viewBuffer, ct);
        return ParseZoneHandoff(viewBuffer.AsSpan(0, viewRead), DeriveSessionKey(sessionHash));
    }

    internal static byte[] BuildViewAcquirePlayerDataRequest(ReadOnlySpan<byte> sessionHash)
    {
        var packet = new byte[FfxiConstants.PacketHeaderSize];
        packet[8] = ViewOpcodeAcquirePlayerData;
        sessionHash.CopyTo(packet.AsSpan(FfxiConstants.PacketIdentifierOffset, FfxiConstants.PacketIdentifierLength));
        return packet;
    }

    /// <summary>
    /// Opcode 0xFE - data_session.cpp's own comment calls this "Reply with
    /// nothing to keep xiloader spinning". Used here purely to make the
    /// server register session.data_session before the view socket needs
    /// it to exist; the server's handler for this opcode does nothing else.
    /// </summary>
    internal static byte[] BuildDataKeepAliveRequest(ReadOnlySpan<byte> sessionHash)
    {
        var packet = new byte[FfxiConstants.PacketHeaderSize];
        packet[0] = DataOpcodeKeepAlive;
        sessionHash.CopyTo(packet.AsSpan(FfxiConstants.PacketIdentifierOffset, FfxiConstants.PacketIdentifierLength));
        return packet;
    }

    internal static byte[] BuildDataCharacterListRequest(uint accountId, uint serverIp, ReadOnlySpan<byte> sessionHash)
    {
        var packet = new byte[FfxiConstants.PacketHeaderSize];
        packet[0] = DataOpcodeCharacterListRequest;
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(1, 4), accountId);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(5, 4), serverIp);
        sessionHash.CopyTo(packet.AsSpan(FfxiConstants.PacketIdentifierOffset, FfxiConstants.PacketIdentifierLength));
        return packet;
    }

    /// <summary>view_session.cpp case 0x07: charId at offset 28, name (up to 15 chars + NUL) at offset 36.</summary>
    internal static byte[] BuildViewSelectCharacterRequest(uint contentId, string characterName, ReadOnlySpan<byte> sessionHash)
    {
        var packet = new byte[ViewSelectRequestSize];
        packet[8] = ViewOpcodeSelectCharacter;
        sessionHash.CopyTo(packet.AsSpan(FfxiConstants.PacketIdentifierOffset, FfxiConstants.PacketIdentifierLength));
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(OffsetSelectCharId, 4), contentId);

        Span<byte> nameField = packet.AsSpan(OffsetSelectCharName, PacketNameLength - 1);
        int nameBytes = Encoding.ASCII.GetBytes(characterName.AsSpan(0, Math.Min(characterName.Length, PacketNameLength - 1)), nameField);
        _ = nameBytes; // remaining bytes stay zero-padded, matching a NUL-terminated fixed field.

        return packet;
    }

    /// <summary>
    /// data_session.cpp case 0xA2: a 20-byte `key3` at offset 1, which
    /// overlaps the mandatory session-hash region (offset 12-27, read by
    /// getHashFromPacket regardless of opcode) - see SelectCharacterAsync's
    /// remarks for why the overlap is left as-is rather than avoided.
    /// </summary>
    internal static byte[] BuildDataConfirmSelectionRequest(ReadOnlySpan<byte> sessionHash)
    {
        var packet = new byte[FfxiConstants.PacketHeaderSize];
        packet[0] = DataOpcodeConfirmSelection;
        sessionHash.CopyTo(packet.AsSpan(FfxiConstants.PacketIdentifierOffset, FfxiConstants.PacketIdentifierLength));
        return packet;
    }

    /// <summary>
    /// The 20-byte `key3` blob the 0xA2 request carries becomes the zone
    /// connection's Blowfish session key verbatim - the login server reads it
    /// straight out of the packet (`memcpy(key3, buffer_.data() + 1, 20)`)
    /// and stores it in `accounts_sessions.session_key`, and the map server
    /// later loads that same blob into `MapSession::blowfish.key` and MD5s it
    /// into the actual cipher key. So the client already knows the key from
    /// the moment it sends 0xA2; nothing needs to be read back.
    ///
    /// Because key3 starts at packet offset 1 and the mandatory session-hash
    /// region starts at offset 12, key3 is 11 zero bytes followed by the
    /// first 9 bytes of the session hash - a direct consequence of the
    /// overlap described in SelectCharacterAsync's remarks, not a choice.
    ///
    /// The server can further mutate `key3[16]` before storing it
    /// (`+= 6` for a just-created character, `+= incrementKeyValue`) - but
    /// `incrementKeyValue` is only ever advanced on error paths that return
    /// before the INSERT, so for a clean login the stored key matches this
    /// exactly. A first login on a brand-new character is the known
    /// exception and is not handled here.
    /// </summary>
    internal static uint[] DeriveSessionKey(ReadOnlySpan<byte> sessionHash)
    {
        var packet = BuildDataConfirmSelectionRequest(sessionHash);

        var key = new uint[5];
        for (int i = 0; i < 5; i++)
        {
            key[i] = BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(1 + i * 4, 4));
        }
        return key;
    }

    /// <summary>
    /// Decodes an lpkt_chr_info2 packet: header(28) + characters(u32)@28 +
    /// N * lpkt_chr_info_sub2(140 bytes each)@32. Field offsets within each
    /// record are derived from login_packets.h's TC_OPERATION_MAKE/
    /// lpkt_chr_info_sub2 layout under standard (unpacked) C struct
    /// alignment - see the constants above.
    /// </summary>
    internal static IReadOnlyList<FfxiCharacter> ParseCharacters(ReadOnlySpan<byte> packet)
    {
        const int countOffset = FfxiConstants.PacketHeaderSize;
        const int recordsStart = countOffset + 4;

        if (packet.Length < recordsStart)
        {
            return [];
        }

        uint count = BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(countOffset, 4));
        var characters = new List<FfxiCharacter>((int)count);

        for (int i = 0; i < count; i++)
        {
            int recordStart = recordsStart + i * RecordSize;
            if (recordStart + RecordSize > packet.Length)
            {
                break;
            }

            ReadOnlySpan<byte> record = packet.Slice(recordStart, RecordSize);
            byte flags = record[10];

            characters.Add(new FfxiCharacter(
                ContentId: BinaryPrimitives.ReadUInt32LittleEndian(record.Slice(OffsetContentId, 4)),
                CharIdMain: BinaryPrimitives.ReadUInt16LittleEndian(record.Slice(OffsetCharIdMain, 2)),
                Name: ReadFixedString(record.Slice(OffsetCharacterName, CharacterNameLength)),
                WorldName: ReadFixedString(record.Slice(OffsetWorldName, WorldNameLength)),
                Race: BinaryPrimitives.ReadUInt16LittleEndian(record.Slice(OffsetRace, 2)),
                MainJob: record[OffsetMainJob],
                MainJobLevel: record[OffsetMainJobLevel],
                SubJob: record[OffsetSubJob],
                Zone: record[OffsetZone],
                CanRename: (flags & 0b0000_0001) != 0,
                EligibleForRaceChange: (flags & 0b0000_0010) != 0));
        }

        return characters;
    }

    /// <summary>
    /// Decodes an lpkt_next_login packet (see login_packets.h) - the
    /// zone-server handoff sent right after a successful character select.
    /// NOT live-tested. The IP fields are read big-endian, unlike every
    /// other integer field in this protocol (all little-endian): the
    /// server populates them via str2ip() from a dotted-quad DB string, and
    /// every other in-memory `in_addr`-style representation on real systems
    /// stores octets in wire/dotted order (first byte = first octet) - so a
    /// big-endian read here gives back the correct dotted address, while a
    /// little-endian one would reverse it. server_port/cache_port are
    /// plain little-endian integers (assigned by value from a uint16 DB
    /// column into a uint32 field, not copied from any network struct).
    /// </summary>
    private const uint ExpectedZoneHandoffCommand = 0x0B; // S2C_0x000B_ResponseNextLogin

    internal static FfxiZoneHandoff ParseZoneHandoff(ReadOnlySpan<byte> packet, uint[] sessionKey)
    {
        if (packet.Length < ZoneHandoffSize)
        {
            throw new ArgumentException($"Zone handoff packet too short: got {packet.Length} bytes, need {ZoneHandoffSize}. Raw: {Convert.ToHexString(packet)}", nameof(packet));
        }

        // packet_t's command field (offset 8) should be 0x0B for a real
        // handoff. If the login server instead sent an error packet (e.g.
        // "unable to connect to world server" - see data_session.cpp's
        // several `viewSession->do_write(0x24)` error paths), the command
        // and payload will differ from what this method assumes - fail
        // loudly with the raw bytes rather than silently returning zeros.
        uint command = BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(8, 4));
        if (command != ExpectedZoneHandoffCommand)
        {
            throw new InvalidOperationException($"Expected command 0x{ExpectedZoneHandoffCommand:X}, got 0x{command:X} - likely a server-side error packet, not a real handoff. Raw: {Convert.ToHexString(packet)}");
        }

        return new FfxiZoneHandoff(
            ContentId: BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(OffsetHandoffContentId, 4)),
            CharIdMain: BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(OffsetHandoffCharIdWorld, 4)),
            CharacterName: ReadFixedString(packet.Slice(OffsetHandoffCharacterName, PacketNameLength)),
            ServerId: BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(OffsetHandoffServerId, 4)),
            ZoneServerIp: BinaryPrimitives.ReadUInt32BigEndian(packet.Slice(OffsetHandoffServerIp, 4)),
            ZoneServerPort: BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(OffsetHandoffServerPort, 4)),
            SearchServerIp: BinaryPrimitives.ReadUInt32BigEndian(packet.Slice(OffsetHandoffCacheIp, 4)),
            SearchServerPort: BinaryPrimitives.ReadUInt32LittleEndian(packet.Slice(OffsetHandoffCachePort, 4)),
            SessionKey: sessionKey);
    }

    /// <summary>Formats a ZoneServerIp/SearchServerIp value (big-endian octets - see ParseZoneHandoff) as a dotted-quad string.</summary>
    public static string FormatIpAddress(uint value) =>
        $"{(value >> 24) & 0xFF}.{(value >> 16) & 0xFF}.{(value >> 8) & 0xFF}.{value & 0xFF}";

    private static string ReadFixedString(ReadOnlySpan<byte> field)
    {
        int nul = field.IndexOf((byte)0);
        ReadOnlySpan<byte> trimmed = nul >= 0 ? field[..nul] : field;
        return Encoding.ASCII.GetString(trimmed);
    }

    public void Dispose()
    {
        _data.Dispose();
        _view.Dispose();
    }
}
