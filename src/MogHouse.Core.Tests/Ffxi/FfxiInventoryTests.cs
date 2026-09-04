using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiInventoryTests
{
    /// <summary>A sub-packet: the packed id and size, a sync word, then a body.</summary>
    private static byte[] SubPacket(ushort id, byte[] body)
    {
        int size = 4 + body.Length;
        size += (4 - (size % 4)) % 4;
        var packet = new byte[size];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(0, 2), FfxiZonePacket.PackIdAndSize(id, size));
        body.CopyTo(packet, 4);
        return packet;
    }

    [Fact]
    public void ItemListCarriesTheSlotAndTheCount()
    {
        var body = new byte[16];
        BinaryPrimitives.WriteUInt32LittleEndian(body.AsSpan(0, 4), 12);      // quantity
        BinaryPrimitives.WriteUInt16LittleEndian(body.AsSpan(4, 2), 4096);    // item id
        body[6] = (byte)FfxiContainer.MogSatchel;
        body[7] = 33;                                                          // slot
        body[8] = 1;                                                           // locked

        FfxiInventoryItem? item = FfxiInventoryItem.TryParse(SubPacket(FfxiInventoryItem.PacketId, body));

        Assert.NotNull(item);
        Assert.Equal(FfxiContainer.MogSatchel, item.Container);
        Assert.Equal(33, item.Slot);
        Assert.Equal(4096, item.ItemId);
        Assert.Equal(12u, item.Quantity);
        Assert.Equal(1, item.LockFlag);
    }

    [Fact]
    public void ADifferentPacketIsNotAnItem()
    {
        Assert.Null(FfxiInventoryItem.TryParse(SubPacket(0x0B5, new byte[16])));
    }

    [Fact]
    public void ContainerSizesPreferTheWideArray()
    {
        // Eighteen bytes, fourteen of padding, then eighteen shorts.
        var body = new byte[18 + 14 + (18 * 2) + 28];
        body[0] = 30;                                                          // inventory, narrow
        BinaryPrimitives.WriteUInt16LittleEndian(body.AsSpan(18 + 14, 2), 80); // inventory, wide

        FfxiContainerSizes? sizes = FfxiContainerSizes.TryParse(
            SubPacket(FfxiContainerSizes.PacketId, body));

        Assert.NotNull(sizes);
        Assert.Equal(80, sizes.Size(FfxiContainer.Inventory));
    }

    [Fact]
    public void ContainerSizesFallBackToTheNarrowArray()
    {
        var body = new byte[18 + 14 + (18 * 2) + 28];
        body[(byte)FfxiContainer.Wardrobe] = 8;

        FfxiContainerSizes? sizes = FfxiContainerSizes.TryParse(
            SubPacket(FfxiContainerSizes.PacketId, body));

        Assert.NotNull(sizes);
        Assert.Equal(8, sizes.Size(FfxiContainer.Wardrobe));
    }

    [Fact]
    public void ReadyNamesTheContainerThatFinished()
    {
        var body = new byte[8];
        body[0] = 0;                                   // still loading
        body[1] = (byte)FfxiContainer.MogSack;

        FfxiInventoryReady? ready = FfxiInventoryReady.TryParse(
            SubPacket(FfxiInventoryReady.PacketId, body));

        Assert.NotNull(ready);
        Assert.False(ready.AllLoaded);
        Assert.Equal(FfxiContainer.MogSack, ready.Container);
    }

    [Fact]
    public void EquipmentPointsAtABagSlot()
    {
        byte[] body = [12, (byte)FfxiEquipSlot.Body, (byte)FfxiContainer.Wardrobe2, 0];

        FfxiEquipment? worn = FfxiEquipment.TryParse(SubPacket(FfxiEquipment.PacketId, body));

        Assert.NotNull(worn);
        Assert.Equal(FfxiEquipSlot.Body, worn.Slot);
        Assert.Equal(FfxiContainer.Wardrobe2, worn.Container);
        Assert.Equal(12, worn.ItemSlot);
        Assert.False(worn.IsEmpty);
    }

    [Fact]
    public void SlotTwoFiveFiveMeansNothingIsWorn()
    {
        byte[] body = [255, (byte)FfxiEquipSlot.Head, (byte)FfxiContainer.Inventory, 0];

        FfxiEquipment? worn = FfxiEquipment.TryParse(SubPacket(FfxiEquipment.PacketId, body));

        Assert.NotNull(worn);
        Assert.True(worn.IsEmpty);
    }

    [Fact]
    public void EquipPacketCarriesTheThreeBytesTheServerReads()
    {
        byte[] packet = FfxiEquipPacket.Build(7, FfxiEquipSlot.Ring2, FfxiContainer.Wardrobe, 0x1234);

        (ushort id, int size) = FfxiZonePacket.UnpackIdAndSize(
            BinaryPrimitives.ReadUInt16LittleEndian(packet));

        Assert.Equal(FfxiEquipPacket.PacketId, id);
        Assert.Equal(FfxiEquipPacket.PacketSize, size);
        Assert.Equal(0x1234, BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(2, 2)));
        Assert.Equal(7, packet[4]);
        Assert.Equal((byte)FfxiEquipSlot.Ring2, packet[5]);
        Assert.Equal((byte)FfxiContainer.Wardrobe, packet[6]);
    }

    [Fact]
    public void TrackerResolvesWhatIsWornThroughTheBag()
    {
        var tracker = new FfxiInventoryTracker();
        tracker.Apply(new FfxiInventoryItem(FfxiContainer.Inventory, 3, 12579, 1, 5));
        tracker.Apply(new FfxiEquipment(FfxiEquipSlot.Body, FfxiContainer.Inventory, 3));

        FfxiInventorySlot? worn = tracker.EquippedItem(FfxiEquipSlot.Body);

        Assert.NotNull(worn);
        Assert.Equal(12579, worn.ItemId);
        Assert.True(worn.IsLocked);
        Assert.Null(tracker.EquippedItem(FfxiEquipSlot.Head));
    }

    [Fact]
    public void AQuantityOfZeroEmptiesTheSlot()
    {
        var tracker = new FfxiInventoryTracker();
        tracker.Apply(new FfxiInventoryItem(FfxiContainer.Inventory, 3, 4096, 12, 0));
        tracker.Apply(new FfxiInventoryItem(FfxiContainer.Inventory, 3, 4096, 0, 0));

        Assert.Null(tracker.At(FfxiContainer.Inventory, 3));
        Assert.Empty(tracker.Contents(FfxiContainer.Inventory));
    }

    [Fact]
    public void ACountUpdateKeepsTheItemItHasNoIdFor()
    {
        var tracker = new FfxiInventoryTracker();
        tracker.Apply(new FfxiInventoryItem(FfxiContainer.Inventory, 3, 4096, 12, 0));
        tracker.Apply(new FfxiInventoryCount(FfxiContainer.Inventory, 3, 5, 0));

        FfxiInventorySlot? slot = tracker.At(FfxiContainer.Inventory, 3);

        Assert.NotNull(slot);
        Assert.Equal(4096, slot.ItemId);
        Assert.Equal(5u, slot.Quantity);
    }

    [Fact]
    public void ACountUpdateForAnUnseenSlotIsIgnored()
    {
        var tracker = new FfxiInventoryTracker();
        tracker.Apply(new FfxiInventoryCount(FfxiContainer.Inventory, 9, 5, 0));

        Assert.Null(tracker.At(FfxiContainer.Inventory, 9));
    }

    [Fact]
    public void DetailReplacesTheListingAndKeepsTheExtraData()
    {
        var tracker = new FfxiInventoryTracker();
        byte[] extra = new byte[FfxiInventoryItemDetail.ExtraLength];
        extra[0] = 0xAB;

        tracker.Apply(new FfxiInventoryItem(FfxiContainer.Inventory, 1, 17440, 1, 0));
        tracker.Apply(new FfxiInventoryItemDetail(FfxiContainer.Inventory, 1, 17440, 1, 4000, 0, extra));

        FfxiInventorySlot? slot = tracker.At(FfxiContainer.Inventory, 1);

        Assert.NotNull(slot);
        Assert.Equal(4000u, slot.Price);
        Assert.Equal(0xAB, slot.Extra![0]);
    }
}
