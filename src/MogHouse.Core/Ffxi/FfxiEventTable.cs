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
    /// A block carries a count, then two tables of sixteen-bit numbers. The
    /// second is the event ids, and 0xFFFF is a hole rather than an event.
    /// </para>
    ///
    /// <para>
    /// Cross-checked against LandSandBoat, which is the only way to be sure a
    /// number found in a file is the number a server would send. Ambrotien's
    /// script starts 2001, 2008, 2009, 2010 and 2011, and his block holds all
    /// five - among thirty more the client knows and the server has not been
    /// taught yet. Ailevia's holds her 655 and 615.
    /// </para>
    ///
    /// <para>
    /// The first table is presumably where each event begins, and is not read
    /// here: on most blocks its values land inside, and on plenty they do not
    /// - entity 0x010E6004 offers 0x279 for a block 296 bytes long - so
    /// whatever it is, it is not simply an offset from the end of the index.
    /// Reading it wrongly would be worse than not reading it.
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

            uint count = BinaryPrimitives.ReadUInt32LittleEndian(Script.AsSpan(4));
            int at = 0x0C + ((int)count * 2);
            if (count == 0 || count > 4000 || at + ((int)count * 2) > Script.Length)
            {
                return [];
            }

            var found = new List<ushort>();
            for (int i = 0; i < count; i++)
            {
                ushort id = BinaryPrimitives.ReadUInt16LittleEndian(Script.AsSpan(at + (i * 2)));
                if (id != 0xFFFF)
                {
                    found.Add(id);
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
