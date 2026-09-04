namespace MogHouse.Core.Ffxi;

/// <summary>What is in one slot of one bag.</summary>
public sealed record FfxiInventorySlot(
    byte Slot, ushort ItemId, uint Quantity, uint Price, byte LockFlag, byte[]? Extra)
{
    /// <summary>Whether this item is bound in place - equipped, or otherwise locked.</summary>
    public bool IsLocked => LockFlag != 0;
}

/// <summary>
/// Assembles the bags and the equipment from the packets the server sends.
///
/// <para>
/// The server does not send an inventory. It sends a stream of one-slot
/// updates - a size for each container, then an item per occupied slot, then
/// "that is all" - and the same stream again, one packet at a time, whenever
/// anything changes. This holds the result.
/// </para>
///
/// <para>
/// Equipment arrives as a pointer rather than an item: slot 5 is wearing
/// whatever is in inventory slot 12. So <see cref="Equipped"/> answers with a
/// slot reference and <see cref="EquippedItem"/> resolves it, which can only
/// work once the bags have arrived. Before that it returns null rather than a
/// guess.
/// </para>
///
/// <para>
/// Nothing here needs the retail install. Item ids are enough to track what is
/// where; turning an id into a name and an icon is <see cref="FfxiItemTable"/>'s
/// job, and a client without the DATs still knows its inventory, just not what
/// to call it.
/// </para>
/// </summary>
public sealed class FfxiInventoryTracker
{
    private readonly Dictionary<FfxiContainer, Dictionary<byte, FfxiInventorySlot>> _bags = [];
    private readonly Dictionary<FfxiEquipSlot, (FfxiContainer Container, byte Slot)> _worn = [];
    private ushort[] _sizes = [];

    /// <summary>Raised whenever anything below changed, so a UI can redraw.</summary>
    public event Action? Changed;

    /// <summary>
    /// Whether the server has said it finished sending. Until it has, a bag
    /// that looks empty may simply not have arrived.
    /// </summary>
    public bool IsLoaded { get; private set; }

    /// <summary>How many slots a container has, or 0 if it was never sized.</summary>
    public ushort Size(FfxiContainer container) =>
        (byte)container < _sizes.Length ? _sizes[(byte)container] : (ushort)0;

    /// <summary>Every occupied slot of a container, in slot order.</summary>
    public IReadOnlyList<FfxiInventorySlot> Contents(FfxiContainer container) =>
        _bags.TryGetValue(container, out Dictionary<byte, FfxiInventorySlot>? bag)
            ? bag.Values.OrderBy(s => s.Slot).ToArray()
            : [];

    /// <summary>One slot of one container, or null if it is empty.</summary>
    public FfxiInventorySlot? At(FfxiContainer container, byte slot) =>
        _bags.TryGetValue(container, out Dictionary<byte, FfxiInventorySlot>? bag) &&
        bag.TryGetValue(slot, out FfxiInventorySlot? found) ? found : null;

    /// <summary>Where the item worn in an equipment slot is kept, or null if nothing is.</summary>
    public (FfxiContainer Container, byte Slot)? Equipped(FfxiEquipSlot slot) =>
        _worn.TryGetValue(slot, out (FfxiContainer Container, byte Slot) at) ? at : null;

    /// <summary>What is worn in an equipment slot, or null if nothing is - or if the bags have not arrived.</summary>
    public FfxiInventorySlot? EquippedItem(FfxiEquipSlot slot) =>
        Equipped(slot) is var (container, at) ? At(container, at) : null;

    /// <summary>Everything worn, in slot order.</summary>
    public IEnumerable<(FfxiEquipSlot Slot, FfxiInventorySlot? Item)> Worn()
    {
        for (int i = 0; i <= (int)FfxiEquipSlot.Back; i++)
        {
            var slot = (FfxiEquipSlot)i;
            yield return (slot, EquippedItem(slot));
        }
    }

    public void Apply(FfxiContainerSizes sizes)
    {
        _sizes = sizes.Sizes;
        Changed?.Invoke();
    }

    public void Apply(FfxiInventoryReady ready)
    {
        if (IsLoaded == ready.AllLoaded)
        {
            return;
        }

        IsLoaded = ready.AllLoaded;
        Changed?.Invoke();
    }

    public void Apply(FfxiInventoryItem item)
    {
        // Quantity zero is how the server empties a slot; there is no separate
        // "removed" packet.
        if (item.Quantity == 0 || item.ItemId == 0)
        {
            Remove(item.Container, item.Slot);
            return;
        }

        Bag(item.Container)[item.Slot] =
            new FfxiInventorySlot(item.Slot, item.ItemId, item.Quantity, 0, item.LockFlag, null);
        Changed?.Invoke();
    }

    public void Apply(FfxiInventoryItemDetail detail)
    {
        if (detail.Quantity == 0 || detail.ItemId == 0)
        {
            Remove(detail.Container, detail.Slot);
            return;
        }

        Bag(detail.Container)[detail.Slot] = new FfxiInventorySlot(
            detail.Slot, detail.ItemId, detail.Quantity, detail.Price, detail.LockFlag, detail.Extra);
        Changed?.Invoke();
    }

    public void Apply(FfxiInventoryCount count)
    {
        if (count.Quantity == 0)
        {
            Remove(count.Container, count.Slot);
            return;
        }

        // No item id in this packet, so there is nothing to do for a slot we
        // have never seen. The 0x01F that fills it will carry the count.
        if (At(count.Container, count.Slot) is not { } existing)
        {
            return;
        }

        Bag(count.Container)[count.Slot] =
            existing with { Quantity = count.Quantity, LockFlag = count.LockFlag };
        Changed?.Invoke();
    }

    public void Apply(FfxiEquipment equipment)
    {
        if (equipment.IsEmpty)
        {
            if (_worn.Remove(equipment.Slot))
            {
                Changed?.Invoke();
            }

            return;
        }

        _worn[equipment.Slot] = (equipment.Container, equipment.ItemSlot);
        Changed?.Invoke();
    }

    /// <summary>Forgets everything. A zone change resends the lot.</summary>
    public void Clear()
    {
        _bags.Clear();
        _worn.Clear();
        _sizes = [];
        IsLoaded = false;
        Changed?.Invoke();
    }

    private Dictionary<byte, FfxiInventorySlot> Bag(FfxiContainer container)
    {
        if (!_bags.TryGetValue(container, out Dictionary<byte, FfxiInventorySlot>? bag))
        {
            bag = [];
            _bags[container] = bag;
        }

        return bag;
    }

    private void Remove(FfxiContainer container, byte slot)
    {
        if (_bags.TryGetValue(container, out Dictionary<byte, FfxiInventorySlot>? bag) && bag.Remove(slot))
        {
            Changed?.Invoke();
        }
    }
}
