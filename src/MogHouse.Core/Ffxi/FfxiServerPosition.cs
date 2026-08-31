using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_SERV_COMMAND_WPOS2 (S2C 0x065) - "you are here now".
///
/// The client walks its own character and reports where it went, which works
/// right up until something other than the player moves it: a GM command, a
/// zone line putting you at the far side, a failed zone check putting you back
/// where you started. All of those arrive as this, and a client that only ever
/// reports its own position ignores them and snaps straight back.
///
/// Positions are the server's own frame; the caller turns them into the
/// renderer's.
/// </summary>
public sealed record FfxiServerPosition(float X, float Vertical, float Depth, sbyte Direction, byte Mode,
                                        uint UniqueNo, ushort ActIndex)
{
    public const ushort PacketId = 0x065;

    private const int OffsetX = 4;
    private const int OffsetVertical = 8;
    private const int OffsetDepth = 12;
    private const int OffsetUniqueNo = 16;
    private const int OffsetActIndex = 20;
    private const int OffsetMode = 22;
    private const int OffsetDirection = 23;

    public static FfxiServerPosition? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < OffsetDirection + 1)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiServerPosition(
            X: BinaryPrimitives.ReadSingleLittleEndian(subPacket[OffsetX..]),
            // The struct calls these y and z; y is the vertical and z the depth,
            // the same way round as everywhere else in this protocol.
            Vertical: BinaryPrimitives.ReadSingleLittleEndian(subPacket[OffsetVertical..]),
            Depth: BinaryPrimitives.ReadSingleLittleEndian(subPacket[OffsetDepth..]),
            Direction: (sbyte)subPacket[OffsetDirection],
            Mode: subPacket[OffsetMode],
            UniqueNo: BinaryPrimitives.ReadUInt32LittleEndian(subPacket[OffsetUniqueNo..]),
            ActIndex: BinaryPrimitives.ReadUInt16LittleEndian(subPacket[OffsetActIndex..]));
    }
}
