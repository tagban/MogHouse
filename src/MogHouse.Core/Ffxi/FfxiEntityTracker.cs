using System;

namespace MogHouse.Core.Ffxi;

/// <summary>One entity the client currently believes is nearby.</summary>
/// <param name="LastSeen">
/// When an update last mentioned it. This is what decides whether it is still
/// there, so it matters that it comes from the caller rather than the clock -
/// tests need to move time without waiting for it.
/// </param>
public sealed record FfxiTrackedEntity(
    uint UniqueNo,
    ushort ActIndex,
    FfxiEntityKind Kind,
    string? Name,
    float X,
    float Vertical,
    float Depth,
    sbyte Direction,
    bool Hidden,

    /// <summary>
    /// The server wants this one's name kept off screen until it is targeted -
    /// doors, zone lines, scenery. They are named, and drawing the name
    /// unprompted labels half a city with things nobody asked about.
    /// </summary>
    bool NameHidden,

    /// <summary>GM level, 0 for an ordinary player. Drives the name's colour.</summary>
    int GmLevel,

    /// <summary>
    /// What the server says this one looks like, kept so the renderer can
    /// build it. Sticky: position-only updates carry no look.
    /// </summary>
    FfxiEntityLook? Look,
    byte? HealthPercent,
    bool Triggerable,
    DateTimeOffset LastSeen);

/// <summary>
/// What is visible right now, assembled from the entity updates the server
/// sends.
///
/// "Visible" is not a decision this makes - it is a decision the server has
/// already made. A zone holds hundreds of NPCs and the server only sends
/// updates for the ones in range, so the set of entities we have heard about
/// *is* the set that can be seen. Nothing here filters by distance.
/// </summary>
public sealed class FfxiEntityTracker
{
    private readonly Dictionary<uint, FfxiTrackedEntity> _entities = [];
    private readonly TimeSpan _forgetAfter;

    /// <summary>What MOGHOUSE_TRACE_ENTITY asked to watch, if anything.</summary>
    private static readonly string? Trace = Environment.GetEnvironmentVariable("MOGHOUSE_TRACE_ENTITY");

    /// <summary>
    /// How long an entity survives without being mentioned again.
    ///
    /// Deliberately generous. The obvious value is a few seconds - the server
    /// sends position updates about once a second - but that is only true of
    /// things that move. A shopkeeper who has not shifted since zone-in may
    /// never be mentioned a second time, and a short timeout would delete
    /// every stationary NPC in the zone while they were still standing there.
    ///
    /// Five minutes was still too short. A player standing still is mentioned
    /// exactly as often as a shopkeeper is - which is to say once - and their
    /// name vanished off them after five minutes while they were plainly
    /// there. Despawn is read from the packet now, so this is only a backstop
    /// against a despawn lost in transit, and it can afford to be long.
    ///
    /// The despawn packet is the exact signal and is parsed now, so this is
    /// only a backstop - for an entity that goes away without one, or a
    /// despawn lost to the transport.
    /// </summary>
    public static readonly TimeSpan DefaultForgetAfter = TimeSpan.FromHours(1);

    public FfxiEntityTracker(TimeSpan? forgetAfter = null)
    {
        _forgetAfter = forgetAfter ?? DefaultForgetAfter;
    }

    /// <summary>
    /// Our own character, so it can be left off the list of other people. The
    /// login reply describes us to ourselves before it describes anyone else,
    /// and a client that does not know this shows itself as another player.
    /// </summary>
    public uint SelfUniqueNo { get; set; }

    /// <summary>
    /// Our own targid, learned from the server describing us to ourselves.
    ///
    /// The handoff gives the character id but not this, and packets the server
    /// validates against the sender - a jump, an emote - are refused without
    /// it. It cannot be derived from the id, so it is taken from the update we
    /// would otherwise only be skipping.
    /// </summary>
    public ushort SelfActIndex { get; private set; }

    public int Count => _entities.Count;

    /// <summary>Folds one update into what we know.</summary>
    public void Observe(FfxiEntityUpdate update, DateTimeOffset now)
    {
        if (update.UniqueNo == 0 || update.UniqueNo == SelfUniqueNo)
        {
            if (update.UniqueNo != 0 && update.ActIndex != 0)
            {
                SelfActIndex = update.ActIndex;
            }

            return;
        }

        // Set MOGHOUSE_TRACE_ENTITY to a unique id, or to "players", to watch
        // what actually arrives for it. Reasoning about which flag hides
        // someone has been wrong twice; this prints the bytes instead.
        if (Trace is not null &&
            (Trace == "players" ? update.PacketId == FfxiEntityUpdate.PlayerPacketId
                                : update.UniqueNo.ToString() == Trace))
        {
            Console.WriteLine(
                $"  TRACE {update.UniqueNo:X8} id=0x{update.PacketId:X3} send=0x{update.SendFlags:X2} " +
                $"flags1={(update.RawFlags1 is uint f1 ? $"0x{f1:X8}" : "-")} " +
                $"despawn={update.IsDespawn} hidden={update.IsHidden} look={update.Look?.Kind.ToString() ?? "-"} " +
                $"name={update.Name ?? "-"} at {update.X:F1},{update.Depth:F1}");
        }

        // A despawn is the real answer to "is it still there", and the only
        // exact one. The timeout below is a backstop for entities that leave
        // without one.
        if (update.IsDespawn)
        {
            _entities.Remove(update.UniqueNo);
            return;
        }

        _entities.TryGetValue(update.UniqueNo, out FfxiTrackedEntity? known);

        // Enemies are sticky, for two reasons that look the same from here.
        //
        // A movement-only update has nothing past the position block, so it
        // cannot say what kind of thing moved. And the flag that marks a mob
        // literally means "a mob that is alive", so killing one clears it.
        // Either way, taking the new Kind would turn a red dot green.
        FfxiEntityKind kind = known?.Kind == FfxiEntityKind.Enemy ? FfxiEntityKind.Enemy
            : update.BattleFlags is null && known is not null ? known.Kind
            : update.Kind;

        _entities[update.UniqueNo] = new FfxiTrackedEntity(
            UniqueNo: update.UniqueNo,
            ActIndex: update.ActIndex,
            Kind: kind,
            // Only present on an update carrying health, so remembered like
            // everything else in that block. A shopkeeper does not stop being
            // clickable because they turned round.
            Triggerable: update.RenderFlags is null ? known?.Triggerable ?? false : update.IsTriggerable,
            // Empty counts as absent. A partial update carries a name field
            // of zeros rather than no name field, and "" is not null - so the
            // sticky rule every other field here gets was skipped for this one,
            // and a character who changed anything at all lost their name.
            Name: string.IsNullOrEmpty(update.Name) ? known?.Name : update.Name,
            X: update.X,
            Vertical: update.Vertical,
            Depth: update.Depth,
            Direction: update.Direction,
            // Sticky the way the name is: a later update that carries no flags
            // must not turn an invisible thing visible.
            Hidden: update.RawFlags1 is null ? known?.Hidden ?? false : update.IsHidden,
            // Sticky for the same reason: a position-only update carries no
            // namevis byte, and must not reveal a door's name.
            // Two ways to earn this. The namevis bit is what the protocol
            // has for it, but a server need not set it - this one leaves it
            // zero on every entity, doors included. What it does say reliably
            // is that a door is a door, and scenery is never labelled.
            NameHidden: update.NameVis is null && update.Look is null
                ? known?.NameHidden ?? false
                : update.IsNameHidden || (update.Look?.IsScenery ?? false) || (known?.NameHidden ?? false),
            Look: update.Look ?? known?.Look,
            // Sticky like the rest: an update with no flags word must not
            // demote a GM back to an ordinary player.
            GmLevel: update.RawFlags1 is null ? known?.GmLevel ?? 0 : update.GmLevel,
            HealthPercent: update.HealthPercent ?? known?.HealthPercent,
            LastSeen: now);
    }

    /// <summary>
    /// Everything still believed to be nearby, forgetting anything that has
    /// gone quiet for too long.
    /// </summary>
    public IReadOnlyList<FfxiTrackedEntity> Visible(DateTimeOffset now)
    {
        if (_forgetAfter > TimeSpan.Zero)
        {
            List<uint> stale = [];
            foreach ((uint id, FfxiTrackedEntity entity) in _entities)
            {
                if (now - entity.LastSeen > _forgetAfter)
                {
                    stale.Add(id);
                }
            }
            foreach (uint id in stale)
            {
                _entities.Remove(id);
            }
        }

        return [.. _entities.Values];
    }

    /// <summary>
    /// Forgets everything. Zoning invalidates the whole list at once - target
    /// indices are reused per zone, so carrying one across would attach an old
    /// entity's name and kind to a new one at the same index.
    /// </summary>
    public void Clear() => _entities.Clear();
}
