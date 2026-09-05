using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// One thing a shop will sell, as the server describes it.
/// </summary>
/// <param name="ItemId">The item, to be looked up in the item DAT for a name and an icon.</param>
/// <param name="Price">What it costs, in gil.</param>
/// <param name="ShopIndex">
/// Where it sits in the shop's own list. This is what a purchase quotes back,
/// not the position in any packet, so it survives a list that arrived in
/// pieces or out of order.
/// </param>
/// <param name="Skill">
/// The craft skill a guild shop wants before it will sell. Zero for an
/// ordinary shopkeeper.
/// </param>
/// <param name="GuildInfo">Guild bookkeeping; zero for an ordinary shopkeeper.</param>
public sealed record FfxiShopItem(ushort ItemId, uint Price, byte ShopIndex, ushort Skill, ushort GuildInfo);

/// <summary>
/// A shop, assembled from the packets that describe one.
///
/// <para>
/// Opening a shop is two packets and not one. <c>0x03E</c> says how many
/// things are for sale, and then <c>0x03C</c> arrives carrying up to nineteen
/// of them at a time, each batch saying where in the list it starts. So a
/// shop with fifty items is one open and three lists, and the window cannot be
/// drawn until they have all landed.
/// </para>
///
/// <para>
/// The layouts are the ones documented at XiPackets, world/server/0x003C and
/// 0x003E and world/client/0x0083.
/// </para>
/// </summary>
public sealed class FfxiShop
{
    public const ushort ListPacketId = 0x03C;
    public const ushort OpenPacketId = 0x03E;
    public const ushort BuyPacketId = 0x083;

    private const int Body = 4;         // the sub-packet header
    private const int EntrySize = 12;   // GP_SHOP

    private readonly Dictionary<byte, FfxiShopItem> _items = [];

    /// <summary>How many things the server said this shop has.</summary>
    public int Expected { get; private set; }

    /// <summary>What has arrived so far, in the shop's own order.</summary>
    public IReadOnlyList<FfxiShopItem> Items =>
        _items.Values.OrderBy(i => i.ShopIndex).ToArray();

    /// <summary>Whether every item the open packet promised has arrived.</summary>
    public bool Complete => Expected > 0 && _items.Count >= Expected;

    /// <summary>
    /// Starts a shop from <c>0x03E</c>, returning false if this is not one.
    ///
    /// A shop that is opened again is emptied first: the server describes each
    /// shop from scratch, and keeping the last shopkeeper's stock would sell
    /// the wrong things at the wrong prices.
    /// </summary>
    public bool Open(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < Body + 2 || IdOf(subPacket) != OpenPacketId)
        {
            return false;
        }

        _items.Clear();
        Expected = BinaryPrimitives.ReadUInt16LittleEndian(subPacket[Body..]);
        return true;
    }

    /// <summary>
    /// Adds a batch from <c>0x03C</c>, returning false if this is not one.
    ///
    /// The batch is however many whole entries the packet actually holds
    /// rather than the nineteen the struct declares - the array is variable
    /// length and a short shop sends a short packet.
    /// </summary>
    public bool AddBatch(ReadOnlySpan<byte> subPacket)
    {
        if (subPacket.Length < Body + 4 || IdOf(subPacket) != ListPacketId)
        {
            return false;
        }

        ReadOnlySpan<byte> entries = subPacket[(Body + 4)..];
        for (int at = 0; at + EntrySize <= entries.Length; at += EntrySize)
        {
            ReadOnlySpan<byte> one = entries[at..];
            uint price = BinaryPrimitives.ReadUInt32LittleEndian(one);
            ushort itemId = BinaryPrimitives.ReadUInt16LittleEndian(one[4..]);
            byte shopIndex = one[6];
            ushort skill = BinaryPrimitives.ReadUInt16LittleEndian(one[8..]);
            ushort guild = BinaryPrimitives.ReadUInt16LittleEndian(one[10..]);

            // A zero item is padding on the last batch, not a thing for sale.
            if (itemId == 0)
            {
                continue;
            }

            _items[shopIndex] = new FfxiShopItem(itemId, price, shopIndex, skill, guild);
        }

        return true;
    }

    private static ushort IdOf(ReadOnlySpan<byte> subPacket) =>
        FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(subPacket)).Id;

    /// <summary>Four bytes of header and twelve of body.</summary>
    public const int BuyPacketSize = 16;

    /// <summary>
    /// <c>0x083</c>, asking to buy.
    /// </summary>
    /// <param name="quantity">How many. The server refuses more than it has.</param>
    /// <param name="shopNo">Which shop, from the open packet's own numbering.</param>
    /// <param name="shopItemIndex">
    /// The item's <see cref="FfxiShopItem.ShopIndex"/> - the shop's numbering,
    /// not the position in whatever packet happened to carry it, so a list
    /// that arrived in pieces still buys the right thing.
    /// </param>
    /// <param name="sync">The client's own counter, as every c2s packet carries.</param>
    public static byte[] BuildBuy(uint quantity, ushort shopNo, ushort shopItemIndex, ushort sync)
    {
        var packet = new byte[BuyPacketSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(0, 2),
                                                 FfxiZonePacket.PackIdAndSize(BuyPacketId, BuyPacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(2, 2), sync);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4), quantity);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(8), shopNo);
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(10), shopItemIndex);
        packet[12] = 0;   // PropertyItemIndex - only a guild shop uses it
        return packet;
    }
}
