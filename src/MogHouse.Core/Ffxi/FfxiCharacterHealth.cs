using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_SERV_COMMAND_GROUP_ATTR (S2C 0x0DF) - how the client learns its own
/// hit points.
///
/// Nothing else tells it. The zone login reply carries a maximum, the entity
/// updates carry other people's health as a percentage, and the character's
/// own current HP arrives here - pushed to the character themselves, and to a
/// party, which is why it is named for the group rather than for the person.
///
/// Without reading it a client cannot tell that it has died, which is not a
/// cosmetic gap: a dead character in FFXI is owed a choice between returning
/// to their home point and waiting for a raise, and until one is made they
/// are lying on the floor. A client that misses the death walks the corpse
/// around instead.
/// </summary>
public sealed record FfxiCharacterHealth(uint UniqueNo, uint Hp, uint Mp, uint Tp,
                                         ushort ActIndex, byte HealthPercent, byte ManaPercent)
{
    public const ushort PacketId = 0x0DF;

    private const int Body = 4; // id/size/sync sub-packet header
    private const int OffsetUniqueNo = Body + 0;
    private const int OffsetHp = Body + 4;
    private const int OffsetMp = Body + 8;
    private const int OffsetTp = Body + 12;
    private const int OffsetActIndex = Body + 16;
    private const int OffsetHpp = Body + 18;
    private const int OffsetMpp = Body + 19;

    /// <summary>Dead. The server will not move them again until it is told what to do.</summary>
    public bool IsDead => Hp == 0;

    public static FfxiCharacterHealth? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < OffsetMpp + 1)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiCharacterHealth(
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetUniqueNo, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetHp, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetMp, 4)),
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetTp, 4)),
            BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetActIndex, 2)),
            subPacket[OffsetHpp],
            subPacket[OffsetMpp]);
    }
}
