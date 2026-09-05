using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiEventTableTests
{
    /// <summary>
    /// Builds one entity's block the way the file holds it.
    ///
    /// The header is the entity id, the capacity and one word - ten bytes, not
    /// twelve. Then the index: <c>capacity - 1</c> offsets, the terminator,
    /// and <c>capacity</c> ids.
    /// </summary>
    private static byte[] Block(uint entityId, ushort[] ids, int[] starts, byte[] code)
    {
        int capacity = ids.Length;
        Assert.Equal(capacity - 1, starts.Length);

        var block = new byte[0x0A + (capacity * 4) + code.Length];
        BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(0), entityId);
        BinaryPrimitives.WriteUInt32LittleEndian(block.AsSpan(4), (uint)capacity);

        void Word(int i, ushort v) => BinaryPrimitives.WriteUInt16LittleEndian(block.AsSpan(0x0A + (i * 2)), v);

        for (int i = 0; i < starts.Length; i++)
        {
            Word(i, (ushort)starts[i]);
        }

        Word(capacity - 1, 0xFFFF);

        for (int i = 0; i < capacity; i++)
        {
            Word(capacity + i, ids[i]);
        }

        code.CopyTo(block, 0x0A + (capacity * 4));
        return block;
    }

    [Fact]
    public void EventZeroStartsAtZeroAndTheOffsetsAreTheRest()
    {
        byte[] code = [1, 2, 3, 4, 5, 6, 7, 8, 9];
        var scripts = new FfxiEventScripts(0x010E6006, Block(0x010E6006, [583, 19], [4], code));

        IReadOnlyList<FfxiEventScripts.Event> events = scripts.Events;

        Assert.Equal(2, events.Count);
        Assert.Equal(583, events[0].Id);
        Assert.Equal<byte[]>([1, 2, 3, 4], events[0].Code);
        Assert.Equal(19, events[1].Id);
        Assert.Equal<byte[]>([5, 6, 7, 8, 9], events[1].Code);
    }

    [Fact]
    public void ASingleEventTakesTheWholeCode()
    {
        byte[] code = [0xAA, 0xBB, 0xCC];
        var scripts = new FfxiEventScripts(1, Block(1, [42], [], code));

        Assert.Single(scripts.Events);
        Assert.Equal(42, scripts.Events[0].Id);
        Assert.Equal(code, scripts.Events[0].Code);
    }

    [Fact]
    public void ASlotTheServerCannotNameStillHasCode()
    {
        // 0xFFFF in the id run is a slot with no server-callable name. It is
        // not padding: it has its own code, and leaving it out of the walk
        // would shift every event after it onto the wrong bytes.
        byte[] code = [1, 2, 3, 4, 5, 6];
        var scripts = new FfxiEventScripts(1, Block(1, [100, 0xFFFF, 200], [2, 4], code));

        Assert.Equal(3, scripts.Events.Count);
        Assert.Equal<byte[]>([3, 4], scripts.Events[1].Code);
        Assert.Equal<byte[]>([5, 6], scripts.Events[2].Code);

        // The named list skips it, because the server can never ask for it.
        Assert.Equal<ushort[]>([100, 200], [.. scripts.EventIds]);
        Assert.Equal<byte[]>([5, 6], scripts.CodeFor(200)!);
    }

    [Fact]
    public void ATerminatorInTheWrongPlaceIsRefusedRatherThanGuessedAt()
    {
        // Reading the index from byte twelve instead of ten puts the runs a
        // word out and lands the terminator at capacity - 2. Rather than
        // produce events off the wrong bytes, the walk gives nothing.
        byte[] block = Block(1, [100, 200], [2], [1, 2, 3, 4]);
        BinaryPrimitives.WriteUInt16LittleEndian(block.AsSpan(0x0A + 2), 0x1234);

        Assert.Empty(new FfxiEventScripts(1, block).Events);
    }

    [Fact]
    public void AnOffsetPastTheCodeIsRefused()
    {
        byte[] block = Block(1, [100, 200], [99], [1, 2, 3, 4]);

        Assert.Empty(new FfxiEventScripts(1, block).Events);
    }

    [Fact]
    public void OffsetsThatGoBackwardsAreRefused()
    {
        byte[] block = Block(1, [1, 2, 3], [3, 1], [1, 2, 3, 4, 5, 6]);

        Assert.Empty(new FfxiEventScripts(1, block).Events);
    }

    [Fact]
    public void TheZoneItselfIsRecognised()
    {
        var scripts = new FfxiEventScripts(FfxiEventScripts.ZoneItself, Block(FfxiEventScripts.ZoneItself, [1], [], [0]));

        Assert.True(scripts.IsZoneItself);
    }
}
