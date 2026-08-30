using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_CLI_COMMAND_POS (id 0x015, 32 bytes) - the position/status update a real
/// client sends continuously. It is the closest thing this protocol has to a
/// heartbeat, and it is what makes a character behave like a live player
/// rather than an inert one: the server's handler sets UPDATE_POS, marks the
/// character dirty for persistence, calls `onEntityMoved`, and sets
/// `requestedInfoSync`, which is what asks the zone to push this character's
/// state to everyone else. A client that never sends it logs in successfully
/// and then renders to other players as "timed out" - accurately, since
/// nothing is arriving.
///
/// Layout read back from the real struct compiled with this environment's
/// MSVC, not hand-derived.
///
/// **The three floats are not (x, y, z).** The server's own handler maps them
/// with a comment saying "Not a typo": the packet's second float is the engine's
/// *vertical* axis and the third is the horizontal depth axis. Getting this
/// wrong doesn't fail loudly - it silently puts the character underground or in
/// a wall - so the parameters here are named for what they mean rather than for
/// their position in the struct.
/// </summary>
public static class FfxiPositionPacket
{
    public const ushort PacketId = 0x015;
    public const int PacketSize = 32;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetX = 4;
    private const int OffsetVertical = 8;  // struct field `z`, engine Y
    private const int OffsetDepth = 12;    // struct field `y`, engine Z
    private const int OffsetMoveTime = 16;
    private const int OffsetMoveFrame = 18;
    private const int OffsetDirection = 20;
    private const int OffsetModeFlags = 21;
    private const int OffsetFaceTarget = 22;
    private const int OffsetTimeNow = 24;

    /// <summary>Mode bits in the flags byte at offset 21, verified against the real bitfield packing.</summary>
    [Flags]
    public enum ModeFlags : byte
    {
        None = 0,
        Target = 0x01,
        Run = 0x02,
        Ground = 0x04,
    }

    /// <summary>
    /// Builds the 32-byte sub-packet. Unlike the login packet this is not a
    /// complete datagram - position updates travel compressed and encrypted
    /// alongside other sub-packets, so this returns just the body for
    /// <c>FfxiZoneClient</c> to frame.
    /// </summary>
    /// <param name="x">Engine X.</param>
    /// <param name="vertical">Engine Y - height. Second float on the wire.</param>
    /// <param name="depth">Engine Z. Third float on the wire.</param>
    /// <param name="direction">Facing, as a single signed byte covering the full circle.</param>
    /// <param name="faceTarget">Entity index being faced, or 0.</param>
    public static byte[] Build(
        ushort sync,
        float x,
        float vertical,
        float depth,
        sbyte direction = 0,
        ushort faceTarget = 0,
        ushort moveTime = 0,
        ushort moveFrame = 0,
        ModeFlags modes = ModeFlags.None,
        uint timeNow = 0)
    {
        var packet = new byte[PacketSize];

        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);

        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(OffsetX, 4), x);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(OffsetVertical, 4), vertical);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(OffsetDepth, 4), depth);

        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetMoveTime, 2), moveTime);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetMoveFrame, 2), moveFrame);

        packet[OffsetDirection] = (byte)direction;
        packet[OffsetModeFlags] = (byte)modes;

        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetFaceTarget, 2), faceTarget);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(OffsetTimeNow, 4), timeNow);

        return packet;
    }
}
