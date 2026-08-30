using System.Buffers.Binary;
using System.Text;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_CLI_COMMAND_LOGIN (id 0x00A, 92 bytes) - the first thing a client sends
/// to the zone/map server after the login server hands it off, and the only
/// packet in this protocol sent in the clear: everything afterwards is
/// Blowfish-encrypted and zlib-compressed. That's not a special case in the
/// server so much as the bootstrap - `recv_parse` tries the plaintext MD5
/// first, and a packet that validates that way is required to be an 0x00A.
///
/// Struct layout is LandSandBoat/server's `GP_CLI_LOGIN` (src/common/mmo.h,
/// branch `base`) and the field names carry over from the original PS2
/// client. Field offsets were NOT hand-computed: the struct was compiled with
/// this environment's real MSVC toolchain and `offsetof` was read back for
/// every field, because it mixes a bitfield, odd-length char arrays
/// (`sName[15]`, `sAccunt[15]`) and a 4-byte-aligned `Ver`, which is exactly
/// the shape where a hand-derived offset table quietly goes wrong.
///
/// What the server actually validates (`recv_parse`):
///   - the outer MD5 over the payload
///   - packet id == 0x00A
///   - the datagram is at least 28 + 92 bytes
///   - <see cref="OffsetLoginPacketCheck"/> equals the low byte of the sum of
///     every byte from `unknown01` (offset 8) to the end of the struct
///   - <see cref="OffsetUniqueNo"/> names a charid with a *pending session*,
///     which the login server creates by IPC during character select
///
/// Everything else is unvalidated on a LandSandBoat server: `Ticket`,
/// `sName`, `sAccunt`, `GrapIDTbl` and `sPlatform` are never read at all
/// (`Ticket` appears nowhere outside the struct definitions), and `uCliLang`
/// is read into a variable that's immediately `std::ignore`d. They're
/// populated here anyway, with honest values, so the packet stays truthful
/// against a stricter server rather than only satisfying this one.
/// </summary>
public static class FfxiZoneLoginPacket
{
    public const ushort PacketId = 0x00A;
    public const int PacketSize = 92;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetLoginPacketCheck = 4;
    private const int OffsetUniqueNo = 12;
    private const int OffsetGrapIdTbl = 16;
    private const int OffsetName = 34;
    private const int OffsetAccount = 49;
    private const int OffsetTicket = 64;
    private const int OffsetVersion = 80;
    private const int OffsetPlatform = 84;
    private const int OffsetClientLanguage = 88;

    private const int NameLength = 15;
    private const int TicketLength = 16;

    /// <summary>
    /// The checksum in <see cref="OffsetLoginPacketCheck"/> covers everything
    /// from `unknown01` onward - deliberately starting *after* the checksum
    /// byte itself, so there's no circular dependency.
    /// </summary>
    private const int ChecksumStart = 8;

    /// <summary>
    /// Builds the complete UDP datagram, ready to send: the 0x00A payload
    /// wrapped in <see cref="FfxiZonePacket.Frame"/>'s header and trailing
    /// MD5.
    /// </summary>
    /// <param name="uniqueNo">
    /// The character id. This is the same value <see cref="FfxiZoneHandoff.ContentId"/>
    /// carries - LandSandBoat's login server looks the "content id" a client
    /// selects up directly as `chars.charid`, so the two are one value under
    /// two names in this server, not a pair that needs mapping.
    /// </param>
    /// <param name="sync">
    /// The sub-packet's own sequence number. `parse` skips any sub-packet
    /// whose sync is &lt;= the session's last client counter or &gt; the outer
    /// header's counter, and an 0x00A resets both session counters to 0 - so
    /// this must be at least 1 and no greater than <paramref name="ownCounter"/>.
    /// </param>
    /// <param name="platform">
    /// `sPlatform`, 4 bytes. LandSandBoat never reads this field, and no
    /// reference available here documents what a real client puts in it - so
    /// there is deliberately no default guess baked in. Callers pass what
    /// they want; zero is a truthful "unknown".
    /// </param>
    public static byte[] BuildDatagram(
        uint uniqueNo,
        string characterName,
        string accountName,
        ReadOnlySpan<byte> ticket,
        uint clientVersion,
        ushort clientLanguage,
        uint platform = 0,
        ushort ownCounter = 1,
        ushort sync = 1,
        uint timestamp = 0)
    {
        if (sync == 0 || sync > ownCounter)
        {
            throw new ArgumentOutOfRangeException(nameof(sync), sync, $"sync must be between 1 and ownCounter ({ownCounter}) inclusive, or the server's parse loop silently skips this packet.");
        }
        if (!ticket.IsEmpty && ticket.Length != TicketLength)
        {
            throw new ArgumentException($"Ticket must be empty or exactly {TicketLength} bytes.", nameof(ticket));
        }

        var payload = new byte[PacketSize];

        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(OffsetSync, 2), sync);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(OffsetUniqueNo, 4), uniqueNo);

        WriteFixedString(payload.AsSpan(OffsetName, NameLength), characterName);
        WriteFixedString(payload.AsSpan(OffsetAccount, NameLength), accountName);

        ticket.CopyTo(payload.AsSpan(OffsetTicket, TicketLength));

        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(OffsetVersion, 4), clientVersion);
        BinaryPrimitives.WriteUInt32LittleEndian(payload.AsSpan(OffsetPlatform, 4), platform);
        BinaryPrimitives.WriteUInt16LittleEndian(payload.AsSpan(OffsetClientLanguage, 2), clientLanguage);

        payload[OffsetLoginPacketCheck] = ComputeLoginPacketCheck(payload);

        return FfxiZonePacket.Frame(payload, ownCounter, peerCounter: 0, timestamp);
    }

    /// <summary>
    /// The `LoginPacketCheck` byte: an 8-bit wrapping sum of every byte from
    /// <see cref="ChecksumStart"/> to the end of the packet. Deliberately
    /// separate from <see cref="BuildDatagram"/> so a received packet can be
    /// verified with the same code that produces one.
    /// </summary>
    public static byte ComputeLoginPacketCheck(ReadOnlySpan<byte> payload)
    {
        if (payload.Length != PacketSize)
        {
            throw new ArgumentException($"Payload must be exactly {PacketSize} bytes, got {payload.Length}.", nameof(payload));
        }

        byte sum = 0;
        foreach (byte b in payload[ChecksumStart..])
        {
            sum += b;
        }
        return sum;
    }

    /// <summary>
    /// `sName`/`sAccunt` are 15-byte fields the server reads as
    /// NUL-terminated, so a 15-character value would leave no room for the
    /// terminator - truncate to 14 to keep one.
    /// </summary>
    private static void WriteFixedString(Span<byte> field, string value)
    {
        int length = Math.Min(value.Length, field.Length - 1);
        Encoding.ASCII.GetBytes(value.AsSpan(0, length), field);
    }
}
