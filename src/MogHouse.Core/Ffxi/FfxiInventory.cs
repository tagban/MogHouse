using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// Where an item is kept. The server's CONTAINER_ID, byte for byte.
///
/// Only <see cref="Inventory"/> and the wardrobes can be equipped from
/// without a server setting being changed, which is why a client that offers
/// to equip out of a mog satchel gets a validation failure rather than a
/// swap.
/// </summary>
public enum FfxiContainer : byte
{
    Inventory = 0,
    MogSafe = 1,
    Storage = 2,
    Temporary = 3,
    MogLocker = 4,
    MogSatchel = 5,
    MogSack = 6,
    MogCase = 7,
    Wardrobe = 8,
    MogSafe2 = 9,
    Wardrobe2 = 10,
    Wardrobe3 = 11,
    Wardrobe4 = 12,
    Wardrobe5 = 13,
    Wardrobe6 = 14,
    Wardrobe7 = 15,
    Wardrobe8 = 16,
    RecycleBin = 17,
}

/// <summary>
/// An equipment slot. The server's SLOTTYPE, and the same order the item
/// DAT's slot bitmask uses - bit 0 is main hand, bit 15 is back.
/// </summary>
public enum FfxiEquipSlot : byte
{
    Main = 0,
    Sub = 1,
    Ranged = 2,
    Ammo = 3,
    Head = 4,
    Body = 5,
    Hands = 6,
    Legs = 7,
    Feet = 8,
    Neck = 9,
    Waist = 10,
    Ear1 = 11,
    Ear2 = 12,
    Ring1 = 13,
    Ring2 = 14,
    Back = 15,
}

/// <summary>
/// GP_SERV_COMMAND_ITEM_MAX (S2C 0x01C) - how many slots each container has.
///
/// Two arrays saying the same thing: eighteen bytes, then eighteen shorts.
/// The bytes are the PS2 client's; the shorts were added when containers grew
/// past 255 slots. The shorts are the ones to believe, and the bytes are kept
/// only because a size under 256 appears in both and disagreeing would be a
/// useful sign something is being read wrong.
/// </summary>
public sealed record FfxiContainerSizes(ushort[] Sizes)
{
    public const ushort PacketId = 0x01C;

    /// <summary>MAX_CONTAINER_ID.</summary>
    public const int Containers = 18;

    private const int OffsetBytes = 4;
    private const int OffsetShorts = 4 + Containers + 14;

    public ushort Size(FfxiContainer container) =>
        (byte)container < Sizes.Length ? Sizes[(byte)container] : (ushort)0;

    public static FfxiContainerSizes? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < OffsetShorts + (Containers * 2))
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        var sizes = new ushort[Containers];
        for (int i = 0; i < Containers; i++)
        {
            ushort wide = BinaryPrimitives.ReadUInt16LittleEndian(subPacket[(OffsetShorts + (i * 2))..]);
            sizes[i] = wide != 0 ? wide : subPacket[OffsetBytes + i];
        }

        return new FfxiContainerSizes(sizes);
    }
}

/// <summary>
/// GP_SERV_COMMAND_ITEM_SAME (S2C 0x01D) - "that is all of them", or not yet.
///
/// The server sends the contents of every container as a stream of 0x01F and
/// 0x020 packets with no count in front, so this is how a client knows when
/// to trust what it has.
///
/// It arrives twice over. Once per container as that container finishes, with
/// state 0 and the container's own id in the byte the struct calls padding;
/// then once at the end with state 1 and 18 - MAX_CONTAINER_ID, meaning all
/// of them - in the same byte. So state 0 is not "wait, something is wrong":
/// it is a bag reporting in.
/// </summary>
public sealed record FfxiInventoryReady(bool AllLoaded, FfxiContainer Container, uint Flags)
{
    public const ushort PacketId = 0x01D;

    public static FfxiInventoryReady? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < 12)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiInventoryReady(
            subPacket[4] == 1,
            (FfxiContainer)subPacket[5],
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket[8..]));
    }
}

/// <summary>
/// GP_SERV_COMMAND_ITEM_LIST (S2C 0x01F) - one item sitting in one slot.
///
/// This is the packet the inventory is built from. A quantity of zero means
/// the slot is now empty, which is how the server deletes as well as adds.
/// </summary>
public sealed record FfxiInventoryItem(
    FfxiContainer Container, byte Slot, ushort ItemId, uint Quantity, byte LockFlag)
{
    public const ushort PacketId = 0x01F;

    public static FfxiInventoryItem? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < 17)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiInventoryItem(
            (FfxiContainer)subPacket[10],
            subPacket[11],
            BinaryPrimitives.ReadUInt16LittleEndian(subPacket[8..]),
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket[4..]),
            subPacket[12]);
    }
}

/// <summary>
/// GP_SERV_COMMAND_ITEM_NUM (S2C 0x01E) - a slot's quantity changed.
///
/// Sent instead of a whole 0x01F when only the count moved: a stack spent
/// down, an arrow fired. It carries no item id, so a client that has not
/// already seen the slot has nothing to apply it to.
/// </summary>
public sealed record FfxiInventoryCount(FfxiContainer Container, byte Slot, uint Quantity, byte LockFlag)
{
    public const ushort PacketId = 0x01E;

    public static FfxiInventoryCount? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < 15)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiInventoryCount(
            (FfxiContainer)subPacket[8],
            subPacket[9],
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket[4..]),
            subPacket[10]);
    }
}

/// <summary>
/// GP_SERV_COMMAND_ITEM_ATTR (S2C 0x020) - the rest of what a slot holds.
///
/// Follows a 0x01F for anything with more to say than an id and a count: what
/// a shop would pay for it, and twenty-four bytes of extra data carrying
/// augments, a signature, charges, a linkshell's colour. Those bytes are kept
/// whole here - their shape depends on what the item is, and unpacking them
/// belongs with whatever cares.
/// </summary>
public sealed record FfxiInventoryItemDetail(
    FfxiContainer Container, byte Slot, ushort ItemId, uint Quantity, uint Price, byte LockFlag, byte[] Extra)
{
    public const ushort PacketId = 0x020;

    /// <summary>Length of the extdata block.</summary>
    public const int ExtraLength = 24;

    public static FfxiInventoryItemDetail? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < 17 + ExtraLength)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiInventoryItemDetail(
            (FfxiContainer)subPacket[14],
            subPacket[15],
            BinaryPrimitives.ReadUInt16LittleEndian(subPacket[12..]),
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket[4..]),
            BinaryPrimitives.ReadUInt32LittleEndian(subPacket[8..]),
            subPacket[16],
            subPacket.Slice(17, ExtraLength).ToArray());
    }
}

/// <summary>
/// GP_SERV_COMMAND_EQUIP_LIST (S2C 0x050) - what is worn in one slot.
///
/// It names a place, not a thing: the container and the slot within it that
/// the equipped item is sitting in. To know what that is, look it up in the
/// inventory - which is why the bags have to arrive before the equipment
/// means anything.
///
/// Unequipping sends the same packet with the slot set to 255, the server's
/// ERROR_SLOTID.
/// </summary>
public sealed record FfxiEquipment(FfxiEquipSlot Slot, FfxiContainer Container, byte ItemSlot)
{
    public const ushort PacketId = 0x050;

    /// <summary>ERROR_SLOTID - this equipment slot is empty.</summary>
    public const byte Empty = 255;

    public bool IsEmpty => ItemSlot == Empty;

    public static FfxiEquipment? TryParse(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < 8)
        {
            return null;
        }

        (ushort id, _) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket));
        if (id != PacketId)
        {
            return null;
        }

        return new FfxiEquipment((FfxiEquipSlot)subPacket[5], (FfxiContainer)subPacket[6], subPacket[4]);
    }
}

/// <summary>
/// GP_CLI_COMMAND_CHARREQ (C2S 0x016) - tell me about that entity again.
///
/// Sent by the retail client when it wants data for something it cannot draw
/// yet. Pointed at our own targid it is the protocol's way of asking about
/// ourselves: the server answers with a full entity update and a status
/// packet, and the entity update is the only place the equipment model ids
/// ever come from.
///
/// Which makes it the answer to gear that has changed without the character
/// changing with it. The server does push a look of its own accord, but not
/// dependably in the same breath as the equip - so rather than wait, ask.
/// </summary>
public static class FfxiCharacterRequestPacket
{
    public const ushort PacketId = 0x016;
    public const int PacketSize = 8;

    public static byte[] Build(ushort actIndex, ushort sync)
    {
        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(0, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(2, 2), sync);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(4, 2), actIndex);
        return packet;
    }
}

/// <summary>
/// GP_CLI_COMMAND_ITEM_DUMP (C2S 0x028) - throw this away.
///
/// The quantity is the whole stack: the server checks it against what it
/// thinks is in the slot and refuses the packet if they disagree, so this
/// cannot be used to drop part of one.
///
/// There is no undo and no confirmation on the wire. Retail asks before
/// sending it, and so should anything that builds this.
/// </summary>
public static class FfxiDropPacket
{
    public const ushort PacketId = 0x028;

    /// <summary>Six bytes of body, rounded up to the four the header counts in.</summary>
    public const int PacketSize = 12;

    public static byte[] Build(uint quantity, FfxiContainer container, byte slot, ushort sync)
    {
        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(0, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(2, 2), sync);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4, 4), quantity);
        packet[8] = (byte)container;
        packet[9] = slot;
        return packet;
    }
}

/// <summary>
/// GP_CLI_COMMAND_EQUIP_SET (C2S 0x050) - wear this, or take it off.
///
/// Three bytes: where the item is, and which slot to put it in. There is no
/// item id anywhere in it, so the server can only equip what it agrees is in
/// that slot already; a client with a stale inventory will equip whatever has
/// since taken the place.
///
/// Pass <see cref="FfxiEquipment.Empty"/> as the item slot to unequip.
///
/// The server refuses this while the character is in an event or under a
/// status that blocks it, and refuses containers other than the inventory and
/// the wardrobes unless the server has been configured to allow them.
/// </summary>
public static class FfxiEquipPacket
{
    public const ushort PacketId = 0x050;

    /// <summary>Three bytes of body, rounded up to the four the header counts in.</summary>
    public const int PacketSize = 8;

    public static byte[] Build(byte itemSlot, FfxiEquipSlot slot, FfxiContainer container, ushort sync)
    {
        var packet = new byte[PacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(0, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(2, 2), sync);
        packet[4] = itemSlot;
        packet[5] = (byte)slot;
        packet[6] = (byte)container;
        return packet;
    }
}
