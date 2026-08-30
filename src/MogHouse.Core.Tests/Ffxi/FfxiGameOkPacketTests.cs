using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiGameOkPacketTests
{
    [Fact]
    public void Build_HasCorrectIdAndSize()
    {
        byte[] packet = FfxiGameOkPacket.Build(sync: 5);

        Assert.Equal(FfxiGameOkPacket.PacketSize, packet.Length);
        (ushort id, int size) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(packet));
        Assert.Equal(FfxiGameOkPacket.PacketId, id);
        Assert.Equal(FfxiGameOkPacket.PacketSize, size);
        Assert.Equal(5, BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(2, 2)));
    }

    /// <summary>
    /// The server validates both payload words as zero (`ClientState not 0`,
    /// `DebugClientFlg not 0`) and drops the packet otherwise.
    /// </summary>
    [Fact]
    public void Build_LeavesBothValidatedWordsZero()
    {
        byte[] packet = FfxiGameOkPacket.Build(sync: 1);

        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(4, 4)));
        Assert.Equal(0u, BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(8, 4)));
    }
}
