using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

/// <summary>
/// Reading what an NPC said, which arrives as a line id rather than as words.
///
/// The id's top bit is a flag and not part of the number. Taken as part of it,
/// every line the server marked that way looked up 32,768 past the end of the
/// zone's table and came out as "(line 48555)" - which is how this was found.
/// </summary>
public class FfxiNpcMessageTests
{
    /// <summary>TALKNUM: UniqueNo, ActIndex, MesNum, Type - see the server's 0x036.</summary>
    private static byte[] BuildTalkNum(uint uniqueNo, ushort actIndex, ushort mesNum, byte type = 0)
    {
        var packet = new byte[16];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, FfxiZonePacket.PackIdAndSize(FfxiNpcMessage.TalkNumPacketId, 16));
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4), uniqueNo);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(8), actIndex);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(10), mesNum);
        packet[12] = type;
        return packet;
    }

    /// <summary>TALKNUMWORK: UniqueNo, num[4], ActIndex, MesNum, Type.</summary>
    private static byte[] BuildTalkNumWork(ushort mesNum)
    {
        var packet = new byte[64];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, FfxiZonePacket.PackIdAndSize(FfxiNpcMessage.TalkNumWorkPacketId, 64));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(4 + 0x16), mesNum);
        return packet;
    }

    [Fact]
    public void ReadsAnOrdinaryLine()
    {
        FfxiNpcMessage? spoken = FfxiNpcMessage.TryParse(BuildTalkNum(0x0102_0304, 7, 6439));

        Assert.NotNull(spoken);
        Assert.Equal(0x0102_0304u, spoken!.UniqueNo);
        Assert.Equal((ushort)7, spoken.ActIndex);
        Assert.Equal((ushort)6439, spoken.MessageId);
        Assert.False(spoken.Nameless);
    }

    // The two seen in a real session, and what they are once the flag is off:
    // 6439 is an ordinary line in West Ronfaure, 15787 one in Southern San
    // d'Oria. Both zones' tables hold their id; neither holds the raw number.
    [Theory]
    [InlineData(39207, 6439)]
    [InlineData(48555, 15787)]
    [InlineData(0x8000, 0)]
    public void StripsTheFlagFromTheTopOfTheId(int raw, int expected)
    {
        FfxiNpcMessage? spoken = FfxiNpcMessage.TryParse(BuildTalkNum(1, 1, (ushort)raw));

        Assert.NotNull(spoken);
        Assert.Equal((ushort)expected, spoken!.MessageId);
        Assert.True(spoken.Nameless);
    }

    [Fact]
    public void StripsItOnTheOtherDialoguePacketToo()
    {
        FfxiNpcMessage? spoken = FfxiNpcMessage.TryParse(BuildTalkNumWork(39207));

        Assert.NotNull(spoken);
        Assert.Equal((ushort)6439, spoken!.MessageId);
        Assert.True(spoken.Nameless);
    }

    [Fact]
    public void IgnoresAPacketThatIsNeither()
    {
        var other = new byte[16];
        BinaryPrimitives.WriteUInt16LittleEndian(other, FfxiZonePacket.PackIdAndSize(FfxiMusicChange.PacketId, 16));

        Assert.Null(FfxiNpcMessage.TryParse(other));
    }
}
