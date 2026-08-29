using System.Buffers.Binary;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// The identifying head of an entity update - GP_SERV_COMMAND_CHAR_PC (0x00D,
/// another player) and GP_SERV_COMMAND_CHAR_NPC (0x00E, an NPC or mob) both
/// open with the same GP_SERV_POS_HEAD block that GP_SERV_COMMAND_LOGIN uses,
/// so the fields that say *who* an update is about are read the same way for
/// all three.
///
/// This exists to make "does client A know about client B?" a thing code can
/// assert rather than something a person has to eyeball. Reading these by hand
/// out of hex is exactly how I got it wrong twice: an 0x00D in a login reply
/// looks like proof another player is visible, but its UniqueNo is usually the
/// receiving character's own id - the server describes you to yourself first.
/// Compare UniqueNo against your own before concluding anything.
///
/// Only the head is parsed. The rest of an 0x00D (appearance, equipment, name)
/// is a large structure with no compiled reference read yet, and guessing at
/// it would produce fields that look authoritative while being wrong.
/// </summary>
public sealed record FfxiEntityUpdate(
    ushort PacketId,
    uint UniqueNo,
    ushort ActIndex,
    sbyte Direction,
    float X,
    float Vertical,
    float Depth)
{
    public const ushort PlayerPacketId = 0x00D;
    public const ushort NpcPacketId = 0x00E;

    private const int Body = 4; // id/size/sync sub-packet header
    private const int OffsetUniqueNo = Body + 0;
    private const int OffsetActIndex = Body + 4;
    private const int OffsetDirection = Body + 7;
    private const int OffsetX = Body + 8;
    private const int OffsetVertical = Body + 12; // engine Y - see FfxiPositionPacket
    private const int OffsetDepth = Body + 16;

    /// <summary>Minimum bytes needed for the head this parses.</summary>
    public const int MinimumSize = Body + 20;

    /// <summary>True for the two ids that carry a GP_SERV_POS_HEAD.</summary>
    public static bool IsEntityUpdate(ushort id) => id is PlayerPacketId or NpcPacketId;

    /// <summary>
    /// Parses the head of one entity-update sub-packet, or returns null if
    /// it's not one or is too short to hold the block.
    /// </summary>
    public static FfxiEntityUpdate? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < MinimumSize)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket[..2]));
        if (!IsEntityUpdate(id))
        {
            return null;
        }

        return new FfxiEntityUpdate(
            PacketId: id,
            UniqueNo: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetUniqueNo, 4)),
            ActIndex: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetActIndex, 2)),
            Direction: (sbyte)subPacket[OffsetDirection],
            X: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetX, 4)),
            Vertical: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetVertical, 4)),
            Depth: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetDepth, 4)));
    }
}
