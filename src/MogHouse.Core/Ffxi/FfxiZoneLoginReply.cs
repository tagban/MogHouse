using System.Buffers.Binary;
using System.Text;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_SERV_COMMAND_LOGIN (S2C id 0x00A, 260 bytes) - the zone state the server
/// sends in answer to the client's login packet. This is where a client learns
/// where it actually is: position, facing, zone, and the world clock.
///
/// Layout read back from the real struct compiled with this environment's
/// MSVC. Its total size came out at 256 bytes of body plus the 4-byte
/// sub-packet header, matching the 260 bytes the live server actually sends -
/// so the layout is corroborated by real traffic, not just by the compiler.
///
/// Only the fields with a clear meaning and a use are surfaced. The packet
/// carries a great deal more (weather timers, a mog-house "Dancer" block,
/// config flags); leaving them unparsed is deliberate - a field decoded on
/// guesswork is worse than one left alone, because it looks trustworthy.
/// </summary>
public sealed record FfxiZoneLoginReply(
    uint UniqueNo,
    ushort ActIndex,
    sbyte Direction,
    float X,
    float Vertical,
    float Depth,
    uint ZoneNo,
    ushort MapNumber,
    ushort ZoneSubNo,
    uint GameTime,
    uint PlayTime,
    string Name,
    ushort EventNo = 0,
    ushort EventNum = 0,
    ushort EventPara = 0,
    ushort EventMode = 0,
    uint LoginState = 0,
    IReadOnlyList<ushort>? Music = null,
    FfxiWeather Weather = FfxiWeather.None)
{
    public const ushort PacketId = 0x00A;
    public const int PacketSize = 260;

    private const int Body = 4; // the id/size/sync sub-packet header

    // GP_SERV_POS_HEAD, offsets within the body.
    private const int OffsetUniqueNo = Body + 0;
    private const int OffsetActIndex = Body + 4;
    private const int OffsetDirection = Body + 7;
    private const int OffsetX = Body + 8;
    private const int OffsetVertical = Body + 12; // struct `z`, engine Y - see FfxiPositionPacket
    private const int OffsetDepth = Body + 16;    // struct `y`, engine Z

    private const int OffsetZoneNo = Body + 44;

    // Event state. A character parked in an unfinished event is treated as
    // being in a cutscene, and cutscene players are not rendered to others -
    // so a non-zero EventNo on a character nobody can see is a strong signal,
    // not a curiosity.
    /// <summary>
    /// The five music slots the zone starts with - day, night, combat solo,
    /// combat party, mount - in xi::MusicSlot order.
    ///
    /// Sitting between GrapIDTbl and the event fields, and skipping them is
    /// what put the event id ten bytes early for months.
    /// </summary>
    private const int OffsetMusic = Body + 82;
    private const int MusicSlots = 5;

    private const int OffsetEventNo = Body + 60;
    //
    // Counted past GrapIDTbl[9] at 64 and MusicNum[5] at 82. Skipping the
    // five music slots put these ten bytes early, so the event id read back
    // as a background music track - a plausible-looking small number that
    // the server rejected as the wrong event. LoginState at 124 and
    // ZoneSubNo at 154 sit past all of it and were always right, which is
    // what pins the rest of the layout down.
    private const int OffsetEventNum = Body + 94;
    private const int OffsetEventPara = Body + 96;
    private const int OffsetEventMode = Body + 98;
    private const int OffsetLoginState = Body + 124;

    /// <summary>
    /// The zone's weather when we walked in, which is the only way to know it:
    /// the server sends 0x057 when it changes, and a client that only listened
    /// for that would stand under whatever the last zone had until the weather
    /// happened to turn.
    ///
    /// Counted forward through the server's own struct and landing between two
    /// offsets already confirmed against live traffic - EventMode ends at 100
    /// and LoginState begins at 124, with WeatherNumber2 and the three weather
    /// timers filling the gap. Only the current one is read; the second and
    /// the timers are what drive a weather that is part way through changing,
    /// and nothing here can use them yet.
    /// </summary>
    private const int OffsetWeather = Body + 100;
    private const int OffsetGameTime = Body + 56;
    private const int OffsetMapNumber = Body + 62;
    private const int OffsetName = Body + 128;
    private const int OffsetZoneSubNo = Body + 154;
    private const int OffsetPlayTime = Body + 156;

    private const int NameLength = 16;

    /// <summary>
    /// Parses one 0x00A sub-packet. Throws rather than returning partial data
    /// if the id or length is wrong - a mis-sliced sub-packet chain should
    /// surface here, not as plausible-looking nonsense downstream.
    /// </summary>
    public static FfxiZoneLoginReply Parse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < PacketSize)
        {
            throw new ArgumentException($"0x00A reply must be {PacketSize} bytes, got {subPacket.Length}.", nameof(subPacket));
        }

        (ushort id, int size) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket[..2]));
        if (id != PacketId)
        {
            throw new ArgumentException($"Expected sub-packet 0x{PacketId:X3}, got 0x{id:X3}.", nameof(subPacket));
        }
        if (size != PacketSize)
        {
            throw new ArgumentException($"0x{PacketId:X3} declared {size} bytes, expected {PacketSize}.", nameof(subPacket));
        }

        return new FfxiZoneLoginReply(
            UniqueNo: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetUniqueNo, 4)),
            ActIndex: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetActIndex, 2)),
            Direction: (sbyte)subPacket[OffsetDirection],
            X: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetX, 4)),
            Vertical: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetVertical, 4)),
            Depth: BinaryPrimitives.ReadSingleLittleEndian(subPacket.Slice(OffsetDepth, 4)),
            ZoneNo: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetZoneNo, 4)),
            MapNumber: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetMapNumber, 2)),
            ZoneSubNo: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetZoneSubNo, 2)),
            GameTime: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetGameTime, 4)),
            PlayTime: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetPlayTime, 4)),
            Name: ReadFixedString(subPacket.Slice(OffsetName, NameLength)),
            EventNo: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetEventNo, 2)),
            EventNum: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetEventNum, 2)),
            EventPara: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetEventPara, 2)),
            EventMode: BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetEventMode, 2)),
            LoginState: BinaryPrimitives.ReadUInt32LittleEndian(subPacket.Slice(OffsetLoginState, 4)),
            Music: ReadMusic(subPacket),
            Weather: (FfxiWeather)BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetWeather, 2)));
    }

    /// <summary>The zone's opening music, one track per slot.</summary>
    private static IReadOnlyList<ushort> ReadMusic(ReadOnlySpan<byte> subPacket)
    {
        var music = new ushort[MusicSlots];
        for (int slot = 0; slot < MusicSlots; slot++)
        {
            music[slot] = BinaryPrimitives.ReadUInt16LittleEndian(subPacket.Slice(OffsetMusic + slot * 2, 2));
        }

        return music;
    }

    private static string ReadFixedString(ReadOnlySpan<byte> field)
    {
        int nul = field.IndexOf((byte)0);
        return Encoding.ASCII.GetString(nul >= 0 ? field[..nul] : field);
    }
}
