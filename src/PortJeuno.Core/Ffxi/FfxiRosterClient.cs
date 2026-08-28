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
/// Steps 1 and 3's exact request byte-layout beyond the fields the server
/// actually reads (view opcode at offset 8, data opcode at offset 0,
/// session hash at FfxiConstants.PacketIdentifierOffset on both) is
/// inferred, not confirmed against real xiloader source - this class was
/// not live-tested against a real server. lpkt_chr_info2 parsing (ParseCharacters)
/// is the well-grounded, testable-without-a-server part; see the tests
/// project for byte-offset verification against a hand-built packet.
/// </summary>
public sealed class FfxiRosterClient : IDisposable
{
    private const byte ViewOpcodeAcquirePlayerData = 0x1F;
    private const byte DataOpcodeCharacterListRequest = 0xA1;
    private const byte DataOpcodePush = 0x01;

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

    public async Task<IReadOnlyList<FfxiCharacter>> FetchCharactersAsync(string host, uint accountId, byte[] sessionHash, CancellationToken ct = default)
    {
        if (sessionHash.Length != FfxiConstants.PacketIdentifierLength)
        {
            throw new ArgumentException($"Session hash must be {FfxiConstants.PacketIdentifierLength} bytes.", nameof(sessionHash));
        }

        await _data.ConnectAsync(host, FfxiConstants.DataPort, ct);
        await _view.ConnectAsync(host, FfxiConstants.ViewPort, ct);

        NetworkStream dataStream = _data.GetStream();
        NetworkStream viewStream = _view.GetStream();

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

    internal static byte[] BuildViewAcquirePlayerDataRequest(ReadOnlySpan<byte> sessionHash)
    {
        var packet = new byte[FfxiConstants.PacketHeaderSize];
        packet[8] = ViewOpcodeAcquirePlayerData;
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
