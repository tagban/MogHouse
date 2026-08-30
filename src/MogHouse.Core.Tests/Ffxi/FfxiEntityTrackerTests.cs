using System.Buffers.Binary;
using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiEntityTrackerTests
{
    private static readonly DateTimeOffset Now = new(2026, 8, 30, 12, 0, 0, TimeSpan.Zero);

    /// <summary>An 0x00E long enough to carry the HP block, so allegiance is present.</summary>
    private static FfxiEntityUpdate Npc(uint uniqueNo, byte allegiance, float x = 0, float z = 0)
    {
        byte[] packet = new byte[0x48];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, (ushort)(0x00E | (packet.Length / 4) << 9));
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4), uniqueNo);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(12), x);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(20), z);
        packet[0x1E] = 100;
        packet[0x29] = allegiance;
        return FfxiEntityUpdate.TryParse(packet)!;
    }

    /// <summary>A movement-only update: the position block and nothing past it.</summary>
    private static FfxiEntityUpdate ShortNpc(uint uniqueNo, float x = 0, float z = 0)
    {
        byte[] packet = new byte[FfxiEntityUpdate.MinimumSize];
        BinaryPrimitives.WriteUInt16LittleEndian(packet, (ushort)(0x00E | (packet.Length / 4) << 9));
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(4), uniqueNo);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(12), x);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(20), z);
        return FfxiEntityUpdate.TryParse(packet)!;
    }

    [Fact]
    public void AnObservedEntityBecomesVisible()
    {
        FfxiEntityTracker tracker = new();
        tracker.Observe(Npc(uniqueNo: 100, allegiance: 0, x: 12, z: -4), Now);

        FfxiTrackedEntity entity = Assert.Single(tracker.Visible(Now));
        Assert.Equal(100u, entity.UniqueNo);
        Assert.Equal(FfxiEntityKind.Enemy, entity.Kind);
        Assert.Equal(12, entity.X);
        Assert.Equal(-4, entity.Depth);
    }

    [Fact]
    public void ObservingTheSameEntityMovesItRatherThanDuplicatingIt()
    {
        FfxiEntityTracker tracker = new();
        tracker.Observe(Npc(uniqueNo: 100, allegiance: 0, x: 0, z: 0), Now);
        tracker.Observe(Npc(uniqueNo: 100, allegiance: 0, x: 30, z: 9), Now.AddSeconds(2));

        FfxiTrackedEntity entity = Assert.Single(tracker.Visible(Now.AddSeconds(2)));
        Assert.Equal(30, entity.X);
    }

    /// <summary>
    /// The one that would have shipped as a bug: a movement-only update has no
    /// allegiance, so taking its Kind turns a red dot green the moment the mob
    /// takes a step.
    /// </summary>
    [Fact]
    public void AMovementUpdateDoesNotTurnAnEnemyIntoAnNpc()
    {
        FfxiEntityTracker tracker = new();
        tracker.Observe(Npc(uniqueNo: 100, allegiance: 0), Now);
        Assert.Equal(FfxiEntityKind.Enemy, Assert.Single(tracker.Visible(Now)).Kind);

        tracker.Observe(ShortNpc(uniqueNo: 100, x: 5, z: 5), Now.AddSeconds(1));

        FfxiTrackedEntity entity = Assert.Single(tracker.Visible(Now.AddSeconds(1)));
        Assert.Equal(FfxiEntityKind.Enemy, entity.Kind);
        Assert.Equal(5, entity.X);
    }

    [Fact]
    public void OurOwnCharacterIsNotAnotherEntity()
    {
        FfxiEntityTracker tracker = new() { SelfUniqueNo = 4242 };
        tracker.Observe(Npc(uniqueNo: 4242, allegiance: 1), Now);

        Assert.Empty(tracker.Visible(Now));
    }

    [Fact]
    public void SilenceEventuallyForgetsAnEntity()
    {
        FfxiEntityTracker tracker = new(TimeSpan.FromSeconds(30));
        tracker.Observe(Npc(uniqueNo: 100, allegiance: 0), Now);

        Assert.Single(tracker.Visible(Now.AddSeconds(29)));
        Assert.Empty(tracker.Visible(Now.AddSeconds(31)));
    }

    /// <summary>
    /// A stationary NPC may never be mentioned twice, so the timeout has to be
    /// long enough not to delete the whole town while it stands still.
    /// </summary>
    [Fact]
    public void TheDefaultTimeoutOutlivesAStationaryNpc()
    {
        FfxiEntityTracker tracker = new();
        tracker.Observe(Npc(uniqueNo: 100, allegiance: 3), Now);

        Assert.Single(tracker.Visible(Now.AddMinutes(2)));
    }

    [Fact]
    public void ZoningForgetsEverything()
    {
        FfxiEntityTracker tracker = new();
        tracker.Observe(Npc(uniqueNo: 100, allegiance: 0), Now);
        tracker.Observe(Npc(uniqueNo: 101, allegiance: 3), Now);
        Assert.Equal(2, tracker.Count);

        tracker.Clear();

        Assert.Empty(tracker.Visible(Now));
    }

    [Fact]
    public void AnEntityWithNoIdIsIgnored()
    {
        FfxiEntityTracker tracker = new();
        tracker.Observe(Npc(uniqueNo: 0, allegiance: 0), Now);

        Assert.Empty(tracker.Visible(Now));
    }

    [Fact]
    public void ANameSurvivesAnUpdateThatDoesNotCarryOne()
    {
        FfxiEntityTracker tracker = new();

        // The name sits at offset 90 and runs 16 bytes, so the packet has to be
        // long enough to hold it - a shorter one parses fine and simply has no
        // name, which is not what this is testing.
        byte[] player = new byte[0x70];
        BinaryPrimitives.WriteUInt16LittleEndian(player, (ushort)(0x00D | (player.Length / 4) << 9));
        BinaryPrimitives.WriteUInt32LittleEndian(player.AsSpan(4), 200u);
        "Questria"u8.CopyTo(player.AsSpan(90));
        tracker.Observe(FfxiEntityUpdate.TryParse(player)!, Now);

        Assert.Equal("Questria", Assert.Single(tracker.Visible(Now)).Name);

        tracker.Observe(ShortNpc(uniqueNo: 200, x: 1, z: 1), Now.AddSeconds(1));

        Assert.Equal("Questria", Assert.Single(tracker.Visible(Now.AddSeconds(1))).Name);
    }
}
