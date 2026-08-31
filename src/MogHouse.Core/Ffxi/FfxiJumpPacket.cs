using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_CLI_COMMAND_JUMP (C2S 0x11D, 12 bytes) - the /jump command.
///
/// A jump is not an emote. Emotes go through GP_CLI_COMMAND_MOTION (0x05D)
/// and are validated against the Emote list, which has no jump in it; this has
/// a packet all of its own, and the server rebroadcasts it as
/// GP_SERV_COMMAND_JUMP (S2C 0x11E) to everyone in range including the sender.
/// That rebroadcast is the only reason anyone else sees the animation - a
/// client that plays it locally and sends nothing is jumping in private.
///
/// The server checks both ids against the character it thinks is sending, so
/// neither can be left zero, and refuses the packet outright while the
/// character is in an event. It also rate-limits it, so a held key will start
/// being dropped with a warning in the map log.
/// </summary>
public static class FfxiJumpPacket
{
    public const ushort PacketId = 0x11D;
    public const int PacketSize = 12;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetUniqueNo = 4;
    private const int OffsetActIndex = 8;

    /// <summary>
    /// Builds a jump. Both ids are our own: the server rejects the packet
    /// unless UniqueNo is the character's id and ActIndex is its targid.
    /// </summary>
    public static byte[] Build(uint uniqueNo, ushort actIndex, ushort sync)
    {
        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(OffsetUniqueNo, 4), uniqueNo);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetActIndex, 2), actIndex);
        return packet;
    }
}
