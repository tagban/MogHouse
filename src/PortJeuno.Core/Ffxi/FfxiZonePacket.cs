using System.Buffers.Binary;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// The outer framing every zone/map (UDP, port 54230) datagram shares -
/// a completely different envelope than the login/lobby TCP protocol's
/// packet_t. Grounded in LandSandBoat/server's src/map/map_networking.cpp
/// (branch `base`, 2026-08-28).
///
/// Layout: a <see cref="HeaderSize"/>-byte header, then the payload, then a
/// 16-byte MD5 of the payload. The server's `recv_parse` validates that MD5
/// before looking at anything else, and `map_decipher_packet` re-validates it
/// after decrypting - it's the protocol's only integrity check and doubles as
/// the "did this decrypt correctly?" test, which is how the server decides
/// whether to retry with the previous Blowfish key during a zone transition.
///
/// The header's own fields are sparse - only three are ever read:
///   offset 0 (uint16): the sender's own packet counter
///   offset 2 (uint16): the highest counter the sender has seen from the peer
///   offset 8 (uint32): a timestamp
/// Everything else stays zero. Both directions use the same shape, with the
/// roles of offsets 0 and 2 swapped by perspective (see `preparePacket`).
/// </summary>
public static class FfxiZonePacket
{
    /// <summary>FFXI_HEADER_SIZE (mmo.h) - 0x1C.</summary>
    public const int HeaderSize = 0x1C;

    /// <summary>The MD5 that trails every datagram's payload.</summary>
    public const int ChecksumSize = 16;

    public const int OffsetOwnCounter = 0;
    public const int OffsetPeerCounter = 2;
    public const int OffsetTimestamp = 8;

    /// <summary>
    /// Wraps a payload in the header + trailing MD5 the server expects.
    /// The MD5 covers only the payload, not the header (`checksum(buff +
    /// FFXI_HEADER_SIZE, size - (FFXI_HEADER_SIZE + 16), buff + size - 16)`).
    /// </summary>
    public static byte[] Frame(ReadOnlySpan<byte> payload, ushort ownCounter, ushort peerCounter, uint timestamp)
    {
        var datagram = new byte[HeaderSize + payload.Length + ChecksumSize];

        BinaryPrimitives.WriteUInt16LittleEndian(datagram.AsSpan(OffsetOwnCounter, 2), ownCounter);
        BinaryPrimitives.WriteUInt16LittleEndian(datagram.AsSpan(OffsetPeerCounter, 2), peerCounter);
        BinaryPrimitives.WriteUInt32LittleEndian(datagram.AsSpan(OffsetTimestamp, 4), timestamp);

        payload.CopyTo(datagram.AsSpan(HeaderSize));

        System.Security.Cryptography.MD5.HashData(
            datagram.AsSpan(HeaderSize, payload.Length),
            datagram.AsSpan(HeaderSize + payload.Length, ChecksumSize));

        return datagram;
    }

    /// <summary>
    /// Packs a sub-packet's `id:9` / `size:7` bitfield into the single uint16
    /// that opens every payload. On the wire the low 9 bits are the packet id
    /// (the server reads `word &amp; 0x1FF`) and the top 7 are the size in
    /// 4-byte units - so a 92-byte packet reports 23, and the server's own
    /// iteration (`(byte1 &amp; 0xFE) * 2`) recovers 92. Verified against the
    /// real compiler's bitfield packing rather than assumed, since bitfield
    /// bit-order is implementation-defined.
    /// </summary>
    public static ushort PackIdAndSize(ushort id, int sizeInBytes)
    {
        if (id > 0x1FF)
        {
            throw new ArgumentOutOfRangeException(nameof(id), id, "Packet id must fit in 9 bits.");
        }
        if (sizeInBytes % 4 != 0)
        {
            throw new ArgumentException($"Packet size must be a multiple of 4, got {sizeInBytes}.", nameof(sizeInBytes));
        }

        int sizeWords = sizeInBytes / 4;
        if (sizeWords > 0x7F)
        {
            throw new ArgumentOutOfRangeException(nameof(sizeInBytes), sizeInBytes, "Packet size must fit in 7 bits of 4-byte words (max 508 bytes).");
        }

        return (ushort)(id | (sizeWords << 9));
    }

    /// <summary>Inverse of <see cref="PackIdAndSize"/>.</summary>
    public static (ushort Id, int SizeInBytes) UnpackIdAndSize(ushort word) =>
        ((ushort)(word & 0x1FF), (word >> 9) * 4);
}
