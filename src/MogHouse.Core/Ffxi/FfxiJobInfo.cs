using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_SERV_COMMAND_JOB_INFO (S2C 0x01B, 132 bytes) - the character's own job,
/// levels and base stats.
///
/// Only reachable once the client sends GP_CLI_COMMAND_GAMEOK: this is part of
/// the initialization batch the server withholds until the zone-in handshake
/// completes, so a client that skips 0x00C never learns what job or level it
/// is.
///
/// Layout read back from the real compiled struct. The named fields sit inside
/// a nested `GP_MYROOM_DANCER` block whose oddly-ordered members
/// (`mjob_no`, `hair_no`, `size`, `sjob_no`) put the sub-job *after* two
/// unrelated appearance bytes, which is not where reading the field list
/// top-to-bottom would suggest.
/// </summary>
public sealed record FfxiJobInfo(
    byte MainJob,
    byte SubJob,
    byte MainJobLevel,
    IReadOnlyList<byte> JobLevels,
    IReadOnlyList<ushort> BaseStats,
    IReadOnlyList<short> StatModifiers,
    int MaxHp,
    int MaxMp)
{
    public const ushort PacketId = 0x01B;
    public const int PacketSize = 132;

    private const int OffsetMainJob = 8;
    private const int OffsetSubJob = 11;
    private const int OffsetJobLevels = 16;
    private const int OffsetBaseStats = 32;
    private const int OffsetStatModifiers = 46;
    private const int OffsetMaxHp = 60;
    private const int OffsetMaxMp = 64;

    /// <summary>One entry per job id, so index 1 is Warrior. Index 0 is unused.</summary>
    public const int JobCount = 16;

    /// <summary>STR, DEX, VIT, AGI, INT, MND, CHR.</summary>
    public const int StatCount = 7;

    public static FfxiJobInfo? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < PacketSize)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket[..2]));
        if (id != PacketId)
        {
            return null;
        }

        byte mainJob = subPacket[OffsetMainJob];

        var jobLevels = new byte[JobCount];
        subPacket.Slice(OffsetJobLevels, JobCount).CopyTo(jobLevels);

        var baseStats = new ushort[StatCount];
        var statModifiers = new short[StatCount];
        for (int i = 0; i < StatCount; i++)
        {
            baseStats[i] = BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetBaseStats + i * 2, 2));
            statModifiers[i] = BinaryPrimitives.ReadInt16LittleEndian(subPacket.Slice(OffsetStatModifiers + i * 2, 2));
        }

        return new FfxiJobInfo(
            MainJob: mainJob,
            SubJob: subPacket[OffsetSubJob],
            MainJobLevel: mainJob < JobCount ? jobLevels[mainJob] : (byte)0,
            JobLevels: jobLevels,
            BaseStats: baseStats,
            StatModifiers: statModifiers,
            MaxHp: BinaryPrimitives.ReadInt32LittleEndian(subPacket.Slice(OffsetMaxHp, 4)),
            MaxMp: BinaryPrimitives.ReadInt32LittleEndian(subPacket.Slice(OffsetMaxMp, 4)));
    }
}
