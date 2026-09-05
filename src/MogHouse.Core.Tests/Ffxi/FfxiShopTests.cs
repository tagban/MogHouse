using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiShopTests
{
    /// <summary>The sub-packet header: the id and the size in four-byte words.</summary>
    private static void WriteHeader(Span<byte> into, ushort id, int bytes) =>
        BinaryPrimitives.WriteUInt16LittleEndian(into, (ushort)(id | ((bytes / 4) << 9)));

    private static byte[] Open(ushort count)
    {
        var p = new byte[8];
        WriteHeader(p, FfxiShop.OpenPacketId, p.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(4), count);
        return p;
    }

    private static byte[] Batch(ushort offset, params (ushort Item, uint Price, byte Index)[] items)
    {
        var p = new byte[8 + (items.Length * 12)];
        WriteHeader(p, FfxiShop.ListPacketId, p.Length);
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(4), offset);

        for (int i = 0; i < items.Length; i++)
        {
            Span<byte> one = p.AsSpan(8 + (i * 12));
            BinaryPrimitives.WriteUInt32LittleEndian(one, items[i].Price);
            BinaryPrimitives.WriteUInt16LittleEndian(one[4..], items[i].Item);
            one[6] = items[i].Index;
        }

        return p;
    }

    [Fact]
    public void AShopIsNotReadyUntilEveryItemHasArrived()
    {
        var shop = new FfxiShop();

        Assert.True(shop.Open(Open(3)));
        Assert.Equal(3, shop.Expected);
        Assert.False(shop.Complete);

        shop.AddBatch(Batch(0, (17, 12u, 0), (18, 34u, 1)));
        Assert.False(shop.Complete);

        shop.AddBatch(Batch(2, (19, 56u, 2)));
        Assert.True(shop.Complete);
        Assert.Equal(3, shop.Items.Count);
    }

    [Fact]
    public void ItemsComeBackInTheShopsOwnOrderRatherThanArrivalOrder()
    {
        var shop = new FfxiShop();
        shop.Open(Open(3));

        // The later batch first, as a reordered pair of packets would give it.
        shop.AddBatch(Batch(2, (19, 56u, 2)));
        shop.AddBatch(Batch(0, (17, 12u, 0), (18, 34u, 1)));

        Assert.Equal<ushort[]>([17, 18, 19], [.. shop.Items.Select(i => i.ItemId)]);
    }

    [Fact]
    public void PriceAndSkillAreRead()
    {
        var shop = new FfxiShop();
        shop.Open(Open(1));

        byte[] p = Batch(0, (4096, 999999u, 0));
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(8 + 8), 40);   // Skill
        BinaryPrimitives.WriteUInt16LittleEndian(p.AsSpan(8 + 10), 7);   // GuildInfo
        shop.AddBatch(p);

        FfxiShopItem only = shop.Items[0];
        Assert.Equal(4096, only.ItemId);
        Assert.Equal(999999u, only.Price);
        Assert.Equal(40, only.Skill);
        Assert.Equal(7, only.GuildInfo);
    }

    [Fact]
    public void PaddingEntriesAreNotThingsForSale()
    {
        var shop = new FfxiShop();
        shop.Open(Open(1));

        // A short shop still sends whole entries; the spare ones are zeroed.
        shop.AddBatch(Batch(0, (17, 12u, 0), (0, 0u, 1), (0, 0u, 2)));

        Assert.Single(shop.Items);
        Assert.Equal(17, shop.Items[0].ItemId);
    }

    [Fact]
    public void OpeningASecondShopForgetsTheFirst()
    {
        var shop = new FfxiShop();
        shop.Open(Open(1));
        shop.AddBatch(Batch(0, (17, 12u, 0)));

        shop.Open(Open(1));

        // Keeping the last shopkeeper's stock would offer the wrong things at
        // the wrong prices under a new name.
        Assert.Empty(shop.Items);
        Assert.False(shop.Complete);
    }

    [Fact]
    public void APacketOfAnotherKindIsRefused()
    {
        var shop = new FfxiShop();
        var other = new byte[8];
        WriteHeader(other, 0x017, other.Length);

        Assert.False(shop.Open(other));
        Assert.False(shop.AddBatch(other));
    }

    [Fact]
    public void ABuyRequestQuotesTheShopsOwnIndex()
    {
        byte[] p = FfxiShop.BuildBuy(quantity: 12, shopNo: 3, shopItemIndex: 5, sync: 99);

        Assert.Equal(FfxiShop.BuyPacketSize, p.Length);
        (ushort id, int size) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(p));
        Assert.Equal(FfxiShop.BuyPacketId, id);
        Assert.Equal(FfxiShop.BuyPacketSize, size);
        Assert.Equal(99, BinaryPrimitives.ReadUInt16LittleEndian(p.AsSpan(2)));
        Assert.Equal(12u, BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(4)));
        Assert.Equal(3, BinaryPrimitives.ReadUInt16LittleEndian(p.AsSpan(8)));
        Assert.Equal(5, BinaryPrimitives.ReadUInt16LittleEndian(p.AsSpan(10)));
    }
}
