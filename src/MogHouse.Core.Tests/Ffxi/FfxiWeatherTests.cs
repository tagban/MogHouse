using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

/// <summary>
/// Reading the weather, which arrives two ways: on the zone login reply when
/// you walk in, and as 0x057 when it turns while you are standing there.
///
/// The offsets are counted through LandSandBoat's own packet structs rather
/// than guessed, and the login reply's weather field lands between two
/// offsets this project had already confirmed against live traffic - EventMode
/// ending at 100 and LoginState beginning at 124. These tests pin it there.
/// </summary>
public class FfxiWeatherChangeTests
{
    /// <summary>A 0x057 as the server lays it out: u32 start, u16 weather, u16 offset.</summary>
    private static byte[] BuildWeatherPacket(ushort weather, uint startTime = 0, ushort offsetTime = 0)
    {
        var packet = new byte[12];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, FfxiZonePacket.PackIdAndSize(FfxiWeatherChange.PacketId, 12));
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4), startTime);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(8), weather);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(10), offsetTime);
        return packet;
    }

    [Theory]
    [InlineData(1, FfxiWeather.Sunshine)]
    [InlineData(3, FfxiWeather.Fog)]
    [InlineData(6, FfxiWeather.Rain)]
    [InlineData(15, FfxiWeather.Thunderstorms)]
    [InlineData(19, FfxiWeather.Darkness)]
    public void ReadsTheWeatherNumber(ushort raw, FfxiWeather expected)
    {
        FfxiWeatherChange? change = FfxiWeatherChange.TryParse(BuildWeatherPacket(raw));

        Assert.NotNull(change);
        Assert.Equal(expected, change!.Weather);
    }

    [Fact]
    public void ReadsTheTimesEitherSideOfIt()
    {
        FfxiWeatherChange? change = FfxiWeatherChange.TryParse(
            BuildWeatherPacket(6, startTime: 0x11223344, offsetTime: 0x5566));

        Assert.NotNull(change);
        Assert.Equal(0x11223344u, change!.StartTime);
        Assert.Equal((ushort)0x5566, change.OffsetTime);
    }

    /// <summary>
    /// Every sub-packet in a zone reply is offered to every parser, so saying
    /// "not mine" correctly matters as much as parsing does.
    /// </summary>
    [Fact]
    public void IgnoresAPacketThatIsNotOne()
    {
        var other = new byte[12];
        BinaryPrimitives.WriteUInt16LittleEndian(other, FfxiZonePacket.PackIdAndSize(FfxiMusicChange.PacketId, 12));

        Assert.Null(FfxiWeatherChange.TryParse(other));
    }

    [Fact]
    public void IgnoresAPacketTooShortToHoldOne()
    {
        Assert.Null(FfxiWeatherChange.TryParse(BuildWeatherPacket(6).AsSpan(0, 8)));
    }
}

public class FfxiZoneLoginReplyWeatherTests
{
    /// <summary>
    /// A 0x00A of the right size with the weather written where the server's
    /// struct puts it: body offset 100, which is 4 (sub-packet header) + 100.
    /// </summary>
    private static byte[] BuildLoginReply(ushort weather)
    {
        var packet = new byte[FfxiZoneLoginReply.PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(
            packet, FfxiZonePacket.PackIdAndSize(FfxiZoneLoginReply.PacketId, FfxiZoneLoginReply.PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(4 + 100), weather);
        return packet;
    }

    [Theory]
    [InlineData(0, FfxiWeather.None)]
    [InlineData(2, FfxiWeather.Clouds)]
    [InlineData(12, FfxiWeather.Snow)]
    public void ZoningInSaysWhatTheSkyIsDoing(ushort raw, FfxiWeather expected)
    {
        Assert.Equal(expected, FfxiZoneLoginReply.Parse(BuildLoginReply(raw)).Weather);
    }

    /// <summary>
    /// The field sits between EventMode and LoginState, both already pinned
    /// against real traffic. Writing the weather must not disturb either, which
    /// is what catches an off-by-a-field slip through the struct.
    /// </summary>
    [Fact]
    public void ReadingItDoesNotDisturbItsNeighbours()
    {
        byte[] packet = BuildLoginReply(6);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(4 + 98), 0xBEEF);   // EventMode
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4 + 124), 0xCAFEu); // LoginState

        FfxiZoneLoginReply reply = FfxiZoneLoginReply.Parse(packet);

        Assert.Equal(FfxiWeather.Rain, reply.Weather);
        Assert.Equal((ushort)0xBEEF, reply.EventMode);
        Assert.Equal(0xCAFEu, reply.LoginState);
    }
}
