using System.Buffers.Binary;
using System.Text;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// One fragment of the server's login message - GP_SERV_COMMAND_FRAGMENTS::SERVMES
/// (S2C 0x04D), the answer to <see cref="FfxiFragmentsPacket"/>.
///
/// The message arrives 236 bytes at a time, so a fragment carries where it sits
/// in the whole and how long the whole is. Anything past the first needs asking
/// for again at the new offset.
/// </summary>
public sealed record FfxiServerMessageFragment(int SizeTotal, int Offset, string Text)
{
    public const ushort PacketId = 0x04D;

    /// <summary>Whether this fragment finishes the message.</summary>
    public bool IsComplete => Offset + Text.Length + 1 >= SizeTotal;

    /// <summary>Where the next request should start, or null when there is no more.</summary>
    public int? NextOffset => IsComplete ? null : Offset + Text.Length;

    private const int OffsetValue1 = 2;
    private const int OffsetSizeTotal = 8;
    private const int OffsetOffset = 12;
    private const int OffsetDataSize = 16;
    private const int OffsetData = 20;

    /// <summary>
    /// Parses a 0x04D sub-packet body, or returns null if it is not a server
    /// message fragment - the same packet id also carries fishing rankings.
    /// The span starts at the sub-packet header, as the other TryParse methods
    /// here do.
    /// </summary>
    public static FfxiServerMessageFragment? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < 4)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        // Past the 4-byte header the body matches the request's shape.
        ReadOnlySpan<byte> body = subPacket[4..];
        if (body.Length < OffsetData)
        {
            return null;
        }

        if (body[OffsetValue1] != FfxiFragmentsPacket.KindServerMessage)
        {
            return null;   // a fishing ranking, not the login message
        }

        int sizeTotal = BinaryPrimitives.ReadInt32LittleEndian(body[OffsetSizeTotal..]);
        int offset = BinaryPrimitives.ReadInt32LittleEndian(body[OffsetOffset..]);
        int dataSize = BinaryPrimitives.ReadInt32LittleEndian(body[OffsetDataSize..]);

        if (sizeTotal <= 0 || offset < 0 || dataSize <= 0 || OffsetData + dataSize > body.Length)
        {
            return null;
        }

        ReadOnlySpan<byte> data = body.Slice(OffsetData, dataSize);

        // The server sends the trailing NUL as part of the length, and the
        // message is plain text otherwise.
        int end = data.IndexOf((byte)0);
        if (end >= 0)
        {
            data = data[..end];
        }

        return new FfxiServerMessageFragment(sizeTotal, offset, Encoding.UTF8.GetString(data));
    }
}
