using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

/// <summary>
/// Offsets and the mode-flag bit values were read back from the real
/// GP_CLI_COMMAND_POS struct compiled with this environment's MSVC, not
/// derived by hand - it carries a bitfield whose bit order is
/// implementation-defined.
/// </summary>
public class FfxiPositionPacketTests
{
    [Fact]
    public void Build_HasCorrectSizeAndPackedHeader()
    {
        byte[] packet = FfxiPositionPacket.Build(sync: 7, x: 1f, vertical: 2f, depth: 3f);

        Assert.Equal(32, packet.Length);

        ushort word = BinaryPrimitives.ReadUInt16LittleEndian(packet);
        Assert.Equal((0x015, 32), FfxiZonePacket.UnpackIdAndSize(word));
        Assert.Equal(7, BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(2, 2)));
    }

    /// <summary>
    /// The server's own handler maps these with a "Not a typo" comment: the
    /// packet's second float is the engine's vertical axis. Pinning the wire
    /// order here, because getting it wrong puts a character underground
    /// rather than failing visibly.
    /// </summary>
    [Fact]
    public void Build_WritesFloatsInWireOrderXVerticalDepth()
    {
        byte[] packet = FfxiPositionPacket.Build(sync: 1, x: -274.49f, vertical: -12.02f, depth: -90f);

        Assert.Equal(-274.49f, BinaryPrimitives.ReadSingleLittleEndian(packet.AsSpan(4, 4)));
        Assert.Equal(-12.02f, BinaryPrimitives.ReadSingleLittleEndian(packet.AsSpan(8, 4)));
        Assert.Equal(-90f, BinaryPrimitives.ReadSingleLittleEndian(packet.AsSpan(12, 4)));
    }

    [Theory]
    [InlineData(FfxiPositionPacket.ModeFlags.Target, 0x01)]
    [InlineData(FfxiPositionPacket.ModeFlags.Run, 0x02)]
    [InlineData(FfxiPositionPacket.ModeFlags.Ground, 0x04)]
    public void Build_ModeFlagsMatchRealBitfieldPacking(FfxiPositionPacket.ModeFlags mode, byte expected)
    {
        byte[] packet = FfxiPositionPacket.Build(sync: 1, x: 0, vertical: 0, depth: 0, modes: mode);

        Assert.Equal(expected, packet[21]);
    }

    [Fact]
    public void Build_WritesDirectionAndFaceTarget()
    {
        byte[] packet = FfxiPositionPacket.Build(sync: 1, x: 0, vertical: 0, depth: 0, direction: -123, faceTarget: 1024);

        Assert.Equal(unchecked((byte)-123), packet[20]);
        Assert.Equal(1024, BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(22, 2)));
    }
}

public class FfxiZoneLoginReplyTests
{
    private static byte[] BuildSyntheticReply()
    {
        var packet = new byte[FfxiZoneLoginReply.PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, FfxiZonePacket.PackIdAndSize(0x00A, FfxiZoneLoginReply.PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(2, 2), 1);

        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4, 4), 1u);       // UniqueNo
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(8, 2), 1024);     // ActIndex
        packet[11] = unchecked((byte)-123);                                      // dir
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(12, 4), -274.49f);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(16, 4), -12.02f);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(20, 4), -90f);

        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4 + 44, 4), 235u); // ZoneNo
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(4 + 62, 2), 235);  // MapNumber
        System.Text.Encoding.ASCII.GetBytes("Questria").CopyTo(packet, 4 + 128);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4 + 156, 4), 808u); // PlayTime

        return packet;
    }

    [Fact]
    public void Parse_ReadsPositionZoneAndName()
    {
        FfxiZoneLoginReply reply = FfxiZoneLoginReply.Parse(BuildSyntheticReply());

        Assert.Equal(1u, reply.UniqueNo);
        Assert.Equal(1024, reply.ActIndex);
        Assert.Equal(-123, reply.Direction);
        Assert.Equal(-274.49f, reply.X);
        Assert.Equal(-12.02f, reply.Vertical);
        Assert.Equal(-90f, reply.Depth);
        Assert.Equal(235u, reply.ZoneNo);
        Assert.Equal(235, reply.MapNumber);
        Assert.Equal("Questria", reply.Name);
        Assert.Equal(808u, reply.PlayTime);
    }

    [Fact]
    public void Parse_WrongPacketId_Throws()
    {
        byte[] packet = BuildSyntheticReply();
        BinaryPrimitives.WriteUInt16LittleEndian(packet, FfxiZonePacket.PackIdAndSize(0x00B, FfxiZoneLoginReply.PacketSize));

        Assert.Throws<ArgumentException>(() => FfxiZoneLoginReply.Parse(packet));
    }

    [Fact]
    public void Parse_TooShort_Throws()
    {
        Assert.Throws<ArgumentException>(() => FfxiZoneLoginReply.Parse(new byte[100]));
    }
}
