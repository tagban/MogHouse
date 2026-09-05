using System.Buffers.Binary;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// One entity's event scripts: everything it can be made to play.
/// </summary>
/// <param name="EntityId">
/// The server's own id, <c>0x1000000 | zone &lt;&lt; 12 | index</c> - the same
/// number the entity update carries, so a script can be found from what the
/// server said without a table in between.
/// </param>
/// <param name="Script">The block as it sits in the file, undecoded.</param>
public sealed record FfxiEventScripts(uint EntityId, byte[] Script)
{
    /// <summary>
    /// Which events this entity has, as the server names them.
    ///
    /// <para>
    /// The index is <c>capacity * 4</c> bytes of sixteen-bit words, and inside
    /// it are two runs: where each event begins, then <c>0xFFFF</c>, then the
    /// ids. The terminator is what separates them, not their lengths - a block
    /// can have no offsets at all and still have events, which is why reading
    /// the ids at a fixed distance from the start works for some NPCs and not
    /// others.
    /// </para>
    ///
    /// <para>
    /// The retail files are the source of truth here; LandSandBoat is only how
    /// the reading was checked, because it is the one place that says what
    /// number a server would actually send. Its DefaultActions.lua names an
    /// event for 38 of Southern San d'Oria's people and 32 of those are
    /// exactly where this puts them - Coderiant's 583, Glenne's 520 and 513,
    /// Ailevia's 655 and 615. Three are not found and three could not be
    /// matched to a block by name at all; where the two disagree it is not
    /// settled that the file is the one in the wrong.
    /// </para>
    ///
    /// <para>
    /// Where each event <i>begins</i> is not solved. The offsets run before
    /// the terminator and there are fewer of them than there are events -
    /// Coderiant has one event and no offsets whatever - so they are not
    /// simply one per event, and guessing at the pairing would be worse than
    /// admitting it.
    /// </para>
    /// </summary>
    public IReadOnlyList<ushort> EventIds
    {
        get
        {
            if (Script.Length < 12)
            {
                return [];
            }

            uint capacity = BinaryPrimitives.ReadUInt32LittleEndian(Script.AsSpan(4));
            if (capacity == 0 || capacity > 4000 || 0x0C + (capacity * 4) > Script.Length)
            {
                return [];
            }

            var found = new List<ushort>();
            bool pastTerminator = false;
            for (int i = 0; i < capacity * 2; i++)
            {
                ushort word = BinaryPrimitives.ReadUInt16LittleEndian(Script.AsSpan(0x0C + (i * 2)));
                if (!pastTerminator)
                {
                    pastTerminator = word == 0xFFFF;
                    continue;   // still in the offsets
                }

                if (word != 0xFFFF)
                {
                    found.Add(word);
                }
            }

            return found;
        }
    }

    /// <summary>Whether the server could ask this entity for that event.</summary>
    public bool Has(ushort eventId) => EventIds.Contains(eventId);

    /// <summary>
    /// The id the zone's own scripts are filed under, rather than any NPC's.
    ///
    /// Every zone has exactly one of these and it is always the first block.
    /// </summary>
    public const uint ZoneItself = 0x7FFFFFF0;

    public bool IsZoneItself => EntityId == ZoneItself;
}

/// <summary>
/// Where cutscenes live.
///
/// <para>
/// The server does not send a cutscene. It sends "play event 7 on entity
/// 0x010E6003" and expects the client to already have it - which is why an
/// unanswered event leaves a character standing invisible to everyone else,
/// and why this file had to be found before any cutscene could play.
/// </para>
///
/// <para>
/// It is <c>5820 + zone</c>, one file per zone, and the shape is as plain as
/// the dialogue tables:
/// </para>
///
/// <code>
///   uint32   how many blocks
///   uint32   the length of each, in order
///   then the blocks, back to back
/// </code>
///
/// <para>
/// Each block opens with the entity id it belongs to, so a block is found by
/// the number the server just sent rather than by counting. Southern San
/// d'Oria has 502 of them: 501 entities and, first, one filed under
/// <see cref="FfxiEventScripts.ZoneItself"/> for whatever belongs to the zone
/// rather than to anybody in it.
/// </para>
///
/// <para>
/// Checked rather than assumed: the lengths sum to exactly the file size with
/// nothing over - 4 + 502*4 + 911,840 = 913,852 - and 501 of the 502 first
/// words are distinct ids inside that zone's range.
/// </para>
///
/// <para>
/// What is <i>inside</i> a block is not decoded yet. It opens with the id, a
/// count, and a table of sixteen-bit numbers, and past that it is bytecode: a
/// virtual machine that moves cameras, poses actors and calls up the dialogue
/// this project can already read. Getting a script out is the half that was
/// missing; running one is its own problem.
/// </para>
/// </summary>
public sealed class FfxiEventTable
{
    /// <summary>Event scripts are this many files before the zone's number.</summary>
    public const int FileIdOffset = 5820;

    private readonly Dictionary<uint, byte[]> _byEntity = [];

    /// <summary>An empty table: every lookup misses.</summary>
    public static FfxiEventTable Empty { get; } = new();

    private FfxiEventTable()
    {
    }

    private FfxiEventTable(byte[] file)
    {
        if (file.Length < 4)
        {
            return;
        }

        uint count = BinaryPrimitives.ReadUInt32LittleEndian(file);

        // A length table this size has to fit before any block does.
        long header = 4L + ((long)count * 4);
        if (count == 0 || count > 100_000 || header > file.Length)
        {
            return;
        }

        long at = header;
        for (uint i = 0; i < count; i++)
        {
            uint length = BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(4 + ((int)i * 4)));
            if (at + length > file.Length)
            {
                return;   // truncated, or this is not what we think it is
            }

            if (length >= 4)
            {
                uint entity = BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan((int)at));
                _byEntity[entity] = file[(int)at..(int)(at + length)];
            }

            at += length;
        }
    }

    /// <summary>Reads a zone's event scripts, or an empty table if it has none.</summary>
    public static FfxiEventTable Load(FfxiFileTable files, int zone)
    {
        try
        {
            if (files.Path(FileIdOffset + zone) is { } path && File.Exists(path))
            {
                return new FfxiEventTable(File.ReadAllBytes(path));
            }
        }
        catch (Exception)
        {
        }

        return Empty;
    }

    /// <summary>How many entities in this zone have scripts.</summary>
    public int Count => _byEntity.Count;

    /// <summary>Every entity that has any, in id order.</summary>
    public IEnumerable<uint> Entities => _byEntity.Keys.OrderBy(id => id);

    /// <summary>What this entity can be made to play, or null if it has nothing.</summary>
    public FfxiEventScripts? For(uint entityId) =>
        _byEntity.TryGetValue(entityId, out byte[]? script)
            ? new FfxiEventScripts(entityId, script)
            : null;

    /// <summary>What belongs to the zone rather than to anybody standing in it.</summary>
    public FfxiEventScripts? Zone() => For(FfxiEventScripts.ZoneItself);
}
