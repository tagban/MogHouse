using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>Which piece of music a track is for - xi::MusicSlot.</summary>
public enum FfxiMusicSlot : ushort
{
    ZoneDay = 0,
    ZoneNight = 1,
    CombatSolo = 2,
    CombatParty = 3,
    Mount = 4,
    Dead = 5,
    MogHouse = 6,
    Fishing = 7,
}

/// <summary>
/// GP_SERV_COMMAND_MUSIC (S2C 0x05F) - "play this track in this slot".
///
/// Music is the server's decision, not the client's: zoning in, nightfall,
/// combat starting and mounting a chocobo each arrive as one of these. The
/// client holds a track per slot and plays whichever the situation calls for,
/// which is why day and night are separate slots rather than one track the
/// client swaps on a clock it would have to keep itself.
///
/// The track number names a file: music%03d.bgw under sound/win/music/data,
/// with the expansions' own numbering continuing into sound2 through sound9.
/// Those are BGMStream containers - see docs, not decoded yet.
/// </summary>
public sealed record FfxiMusicChange(FfxiMusicSlot Slot, ushort Track)
{
    public const ushort PacketId = 0x05F;

    private const int OffsetSlot = 4;
    private const int OffsetTrack = 6;

    /// <summary>Parses a 0x05F sub-packet, or returns null if it is not one.</summary>
    public static FfxiMusicChange? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < OffsetTrack + 2)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiMusicChange((FfxiMusicSlot)BinaryPrimitives.ReadUInt16LittleEndian(subPacket[OffsetSlot..]),
                                   BinaryPrimitives.ReadUInt16LittleEndian(subPacket[OffsetTrack..]));
    }

    /// <summary>The file this track names, relative to the install root.</summary>
    public string FileName => $"music{Track:D3}.bgw";
}
