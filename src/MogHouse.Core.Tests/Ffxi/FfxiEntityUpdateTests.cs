using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

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

public class FfxiEntityUpdateAppearanceTests
{
    /// <summary>
    /// A real 0x00D captured live: 104 bytes, Questria (charid 1). Offsets for
    /// Flags1 (32) and name (90) come from the compiled GP_SERV_CHAR_PC, since
    /// several bitfield blocks and a misaligned uint8 sit between the head and
    /// the name.
    /// </summary>
    private const string RealQuestriaSpawnHex =
        "0D3401000100000001041F859A098CC3EC5140C10C82B0C2000000003232640000000000" +
        "000000000001800000000000000000000000000000000000000000000000100400000000" +
        "0102001008200830084008501560007000805175657374726961000000000000000000";

    private static byte[] RealQuestriaSpawn()
    {
        var packet = new byte[104];
        byte[] decoded = Convert.FromHexString(RealQuestriaSpawnHex.Replace(" ", ""));
        decoded.AsSpan(0, Math.Min(decoded.Length, packet.Length)).CopyTo(packet);
        return packet;
    }

    [Fact]
    public void TryParse_ReadsNameFromRealPacket()
    {
        FfxiEntityUpdate entity = FfxiEntityUpdate.TryParse(RealQuestriaSpawn())!;

        Assert.Equal(1u, entity.UniqueNo);
        Assert.Equal("Questria", entity.Name);
    }

    /// <summary>
    /// GraphSize is Flags1 bits 9-10, fed from char_look.size. Zero is worth
    /// surfacing: that column defaults to 0 and isn't filled by the
    /// character-insert trigger, and a zero-scale character is present and
    /// targetable while plausibly drawing as nothing.
    /// </summary>
    [Fact]
    public void TryParse_ReadsModelSize()
    {
        FfxiEntityUpdate entity = FfxiEntityUpdate.TryParse(RealQuestriaSpawn())!;

        Assert.Equal((byte)0, entity.ModelSize);
    }

    [Fact]
    public void TryParse_ModelSizeDecodesFromFlags1Bits9And10()
    {
        byte[] packet = RealQuestriaSpawn();
        // GraphSize = 2 -> bit 10 -> byte 33 bit 2.
        packet[33] |= 0b0000_0100;

        Assert.Equal((byte)2, FfxiEntityUpdate.TryParse(packet)!.ModelSize);
    }

    /// <summary>NPC updates use a different layout past the shared head, so neither field is read for them.</summary>
    [Fact]
    public void TryParse_NpcUpdate_HasNoNameOrModelSize()
    {
        var packet = new byte[104];
        System.Buffers.Binary.BinaryPrimitives.WriteUInt16LittleEndian(
            packet, FfxiZonePacket.PackIdAndSize(FfxiEntityUpdate.NpcPacketId, 104));

        FfxiEntityUpdate entity = FfxiEntityUpdate.TryParse(packet)!;

        Assert.Null(entity.Name);
        Assert.Null(entity.ModelSize);
    }
}
