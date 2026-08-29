using System.Buffers.Binary;
using PortJeuno.Core.Ffxi;

namespace PortJeuno.Core.Tests.Ffxi;

public class FfxiEntityUpdateTests
{
    /// <summary>
    /// The first 24 bytes of a real 0x00D captured from the live server -
    /// header, then the GP_SERV_POS_HEAD block. Padded out to the declared
    /// 104-byte length; only the head is parsed.
    /// </summary>
    private const string RealPlayerUpdateHeadHex = "0D34010001000000000418D289C3EC5140C1EE3CB7C2";

    private static byte[] RealPlayerUpdate()
    {
        var packet = new byte[104];
        Convert.FromHexString(RealPlayerUpdateHeadHex).CopyTo(packet, 0);
        return packet;
    }

    [Fact]
    public void TryParse_RealPlayerUpdate_ReadsIdentityFromPosHead()
    {
        FfxiEntityUpdate? entity = FfxiEntityUpdate.TryParse(RealPlayerUpdate());

        Assert.NotNull(entity);
        Assert.Equal(FfxiEntityUpdate.PlayerPacketId, entity!.PacketId);
        Assert.Equal(1u, entity.UniqueNo);
        Assert.Equal(1024, entity.ActIndex);
    }

    /// <summary>
    /// The trap this type exists to prevent: an 0x00D whose UniqueNo is the
    /// receiving character's own id is the server describing you to yourself,
    /// not evidence that another player is visible. Reading these by hand is
    /// how that got misdiagnosed twice.
    /// </summary>
    [Fact]
    public void TryParse_SelfUpdate_IsDistinguishableFromAnotherPlayer()
    {
        FfxiEntityUpdate self = FfxiEntityUpdate.TryParse(RealPlayerUpdate())!;

        byte[] otherRaw = RealPlayerUpdate();
        BinaryPrimitives.WriteUInt32LittleEndian(otherRaw.AsSpan(4, 4), 2u);
        FfxiEntityUpdate other = FfxiEntityUpdate.TryParse(otherRaw)!;

        const uint ourCharId = 1u;
        Assert.Equal(ourCharId, self.UniqueNo);
        Assert.NotEqual(ourCharId, other.UniqueNo);
    }

    [Fact]
    public void TryParse_NpcUpdate_Recognised()
    {
        var packet = new byte[72];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, FfxiZonePacket.PackIdAndSize(FfxiEntityUpdate.NpcPacketId, 72));
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4, 4), 17801226u);

        FfxiEntityUpdate? entity = FfxiEntityUpdate.TryParse(packet);

        Assert.NotNull(entity);
        Assert.Equal(FfxiEntityUpdate.NpcPacketId, entity!.PacketId);
        Assert.Equal(17801226u, entity.UniqueNo);
    }

    [Fact]
    public void TryParse_UnrelatedPacketId_ReturnsNull()
    {
        var packet = new byte[72];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, FfxiZonePacket.PackIdAndSize(0x017, 72));

        Assert.Null(FfxiEntityUpdate.TryParse(packet));
    }

    [Fact]
    public void TryParse_TooShort_ReturnsNull()
    {
        Assert.Null(FfxiEntityUpdate.TryParse(new byte[8]));
    }

    [Theory]
    [InlineData(0x00D, true)]
    [InlineData(0x00E, true)]
    [InlineData(0x00A, false)]
    [InlineData(0x017, false)]
    public void IsEntityUpdate_OnlyMatchesThePosHeadCarryingIds(ushort id, bool expected)
    {
        Assert.Equal(expected, FfxiEntityUpdate.IsEntityUpdate(id));
    }
}
