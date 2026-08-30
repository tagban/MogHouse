using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

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
/// <param name="Name">
/// Present only for player updates long enough to carry it - NPC updates use a
/// different layout past the shared head, so this is never read for them.
/// </param>
/// <param name="ModelSize">
/// `Flags1.GraphSize`, the model scale the server sends from `char_look.size`.
/// Surfaced because a zero here is worth noticing: that column defaults to 0
/// and isn't populated by the character-insert trigger, and a zero-scale
/// character is present and targetable while plausibly drawing as nothing.
/// </param>
public sealed record FfxiEntityUpdate(
    ushort PacketId,
    uint UniqueNo,
    ushort ActIndex,
    sbyte Direction,
    float X,
    float Vertical,
    float Depth,
    string? Name = null,
    byte? ModelSize = null,
    uint? RawFlags1 = null,
    uint? RawFlags0 = null)
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

    // Offsets confirmed against the compiled GP_SERV_CHAR_PC, not counted by
    // hand: the struct threads several bitfield blocks and a misaligned
    // uint8 between the head and the name.
    private const int OffsetFlags1 = 32;
    private const int OffsetName = 90;
    private const int NameLength = 16;

    /// <summary>Flags1 bits 9-10 - see <see cref="ModelSize"/>.</summary>
    private const int GraphSizeBitOffset = 9;
    private const uint GraphSizeMask = 0b11;

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

        // Only player updates share the layout past the head, so name and
        // model size are read for those alone - and only when the packet is
        // actually long enough, since these are truncated by update type.
        string? name = null;
        byte? modelSize = null;
        uint? rawFlags1 = null;
        uint? rawFlags0 = null;

        if (id == PlayerPacketId && subPacket.Length >= OffsetFlags1 + 4)
        {
            uint flags1 = BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetFlags1, 4));
            rawFlags1 = flags1;
            rawFlags0 = BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(24, 4));
            modelSize = (byte)((flags1 >> GraphSizeBitOffset) & GraphSizeMask);

            if (subPacket.Length >= OffsetName + 1)
            {
                int available = Math.Min(NameLength, subPacket.Length - OffsetName);
                name = ReadFixedString(subPacket.Slice(OffsetName, available));
            }
        }

        return new FfxiEntityUpdate(
            PacketId: id,
            UniqueNo: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetUniqueNo, 4)),
            ActIndex: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetActIndex, 2)),
            Direction: (sbyte)subPacket[OffsetDirection],
            X: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetX, 4)),
            Vertical: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetVertical, 4)),
            Depth: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetDepth, 4)),
            Name: name,
            ModelSize: modelSize,
            RawFlags1: rawFlags1,
            RawFlags0: rawFlags0);
    }

    private static string ReadFixedString(ReadOnlySpan<byte> field)
    {
        int nul = field.IndexOf((byte)0);
        return System.Text.Encoding.ASCII.GetString(nul >= 0 ? field[..nul] : field);
    }
}
