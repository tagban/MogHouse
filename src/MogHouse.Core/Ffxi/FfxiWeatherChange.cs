using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// GP_SERV_COMMAND_WEATHER (S2C 0x057) - "the weather is now this".
///
/// <para>
/// Weather is the server's, the way music is: a zone runs its own schedule and
/// tells whoever is standing in it. A client that only listened for this would
/// be under the wrong sky until the weather next turned, which is why
/// <see cref="FfxiZoneLoginReply"/> carries the current one as well.
/// </para>
///
/// <para>
/// The body is the server's own struct, three fields and no padding: a start
/// time, the weather, and an offset. <c>StartTime</c> and
/// <c>WeatherOffsetTime</c> are read but unused - together they say when the
/// change began and how far into it the client is joining, which is what a
/// client that faded between skies would need. This one changes sky at once,
/// so it only wants the number.
/// </para>
/// </summary>
public sealed record FfxiWeatherChange(FfxiWeather Weather, uint StartTime, ushort OffsetTime)
{
    public const ushort PacketId = 0x057;

    private const int OffsetStartTime = 4;
    private const int OffsetWeather = 8;
    private const int OffsetOffsetTime = 10;

    /// <summary>Parses a 0x057 sub-packet, or returns null if it is not one.</summary>
    public static FfxiWeatherChange? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < OffsetOffsetTime + 2)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiWeatherChange(
            (FfxiWeather)BinaryPrimitives.ReadUInt16LittleEndian(subPacket[OffsetWeather..]),
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket[OffsetStartTime..]),
            BinaryPrimitives.ReadUInt16LittleEndian(subPacket[OffsetOffsetTime..]));
    }
}
