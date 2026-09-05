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
    /// <summary>One event: the number the server names it by, and where it starts.</summary>
    /// <param name="Offset">Into <see cref="Script"/>, ready to read from.</param>
    public readonly record struct Entry(ushort EventId, int Offset);

    /// <summary>
    /// Every event this entity has, and where each one begins.
    ///
    /// <para>
    /// A block carries a capacity, then two parallel tables of sixteen-bit
    /// numbers: where each event starts, and the id the server names it by.
    /// The first table ends at <c>0xFFFF</c> - a terminator, not a hole, which
    /// is the whole of why this took two attempts. Read as holes, a third of
    /// the blocks pointed past their own end; read as a terminator, all 1,626
    /// blocks across seven zones resolve with nothing left over.
    /// </para>
    ///
    /// <para>
    /// Offsets are from the end of the index, which is <c>0x0C + capacity *
    /// 4</c>. An entry whose id is 0xFFFF has somewhere to start and no name to
    /// be called by - reachable from inside a script rather than from the
    /// server - so those are left out here.
    /// </para>
    ///
    /// <para>
    /// Checked against LandSandBoat, which is the only way to be sure a number
    /// found in a file is a number a server would send: Ambrotien's script
    /// starts 2001, 2008, 2009, 2010 and 2011 and his block holds all five;
    /// Ailevia's holds her 655 and 615, at 0x69 and 0xBB.
    /// </para>
    /// </summary>
    public IReadOnlyList<Entry> Events
    {
        get
        {
            if (Script.Length < 12)
            {
                return [];
            }

            uint capacity = BinaryPrimitives.ReadUInt32LittleEndian(Script.AsSpan(4));
            int code = 0x0C + ((int)capacity * 4);
            if (capacity == 0 || capacity > 4000 || code > Script.Length)
            {
                return [];
            }

            var found = new List<Entry>();
            for (int i = 0; i < capacity; i++)
            {
                ushort offset = BinaryPrimitives.ReadUInt16LittleEndian(Script.AsSpan(0x0C + (i * 2)));
                if (offset == 0xFFFF)
                {
                    break;   // the end of the table, not a gap in it
                }

                ushort id = BinaryPrimitives.ReadUInt16LittleEndian(
                    Script.AsSpan(0x0C + ((int)capacity * 2) + (i * 2)));
                if (id != 0xFFFF && code + offset <= Script.Length)
                {
                    found.Add(new Entry(id, code + offset));
                }
            }

            return found;
        }
    }

    /// <summary>Where an event starts, or null if this entity has no such event.</summary>
    public int? Start(ushort eventId)
    {
        foreach (Entry entry in Events)
        {
            if (entry.EventId == eventId)
            {
                return entry.Offset;
            }
        }

        return null;
    }

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
