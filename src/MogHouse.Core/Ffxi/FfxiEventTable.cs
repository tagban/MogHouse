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
    /// <summary>The index start: the header is the id, the capacity and one word.</summary>
    private const int IndexAt = 0x0A;

    /// <summary>
    /// One event: the id the server calls it by, and the code that runs.
    /// </summary>
    /// <param name="Id">
    /// What the server names it. <c>0xFFFF</c> means the slot has no name the
    /// server can use - it still has code, and is presumably reached from
    /// another event rather than asked for directly.
    /// </param>
    /// <param name="Code">The slot's bytecode, still undecoded.</param>
    public sealed record Event(ushort Id, byte[] Code);

    /// <summary>
    /// Every event in the block, in slot order, with the code of each.
    ///
    /// <para>
    /// The index is <c>capacity * 4</c> bytes of sixteen-bit words starting at
    /// byte ten, and it is exactly:
    /// </para>
    ///
    /// <code>
    /// capacity - 1  where events 1..n begin, as byte offsets into the code
    /// 1             0xFFFF
    /// capacity      the ids the server names them by
    /// </code>
    ///
    /// <para>
    /// which sums to <c>capacity * 2</c> words. Event zero is not given an
    /// offset because it always begins at zero, and that off-by-one is the
    /// whole of why this looked unsolved: read from byte twelve instead of
    /// ten, the runs come out a word short, the terminator lands at
    /// <c>capacity - 2</c> in most blocks but not all, and some entities
    /// appear to have events with no offsets at all. Read from ten the
    /// terminator is at <c>capacity - 1</c> in every block of every zone
    /// tried - 502 in Southern San d'Oria, 173 in Valkurm Dunes, 76 in West
    /// Ronfaure - the offsets always ascend, and every one lands inside the
    /// code rather than past its end.
    /// </para>
    ///
    /// <para>
    /// The retail files are the source of truth here; LandSandBoat is only how
    /// the reading was checked, because it is the one place that says what
    /// number a server would actually send: Coderiant's 583, Glenne's 520 and
    /// 513, Ailevia's 655 and 615, and all five of Ambrotien's 2001 and
    /// 2008-2011 come back from this.
    /// </para>
    /// </summary>
    public IReadOnlyList<Event> Events
    {
        get
        {
            if (Script.Length < IndexAt + 4)
            {
                return [];
            }

            uint capacity = BinaryPrimitives.ReadUInt32LittleEndian(Script.AsSpan(4));
            long indexBytes = (long)capacity * 4;
            if (capacity == 0 || capacity > 4000 || IndexAt + indexBytes > Script.Length)
            {
                return [];
            }

            ushort Word(int i) => BinaryPrimitives.ReadUInt16LittleEndian(Script.AsSpan(IndexAt + (i * 2)));

            // The terminator is where it is supposed to be, or this is not the
            // shape we think and reading on would invent events.
            if (Word((int)capacity - 1) != 0xFFFF)
            {
                return [];
            }

            int codeAt = IndexAt + (int)indexBytes;
            int codeLength = Script.Length - codeAt;

            var starts = new int[capacity + 1];
            starts[0] = 0;
            for (int i = 1; i < capacity; i++)
            {
                starts[i] = Word(i - 1);
                if (starts[i] < starts[i - 1] || starts[i] > codeLength)
                {
                    return [];
                }
            }

            starts[capacity] = codeLength;

            var found = new List<Event>((int)capacity);
            for (int i = 0; i < capacity; i++)
            {
                ushort id = Word((int)capacity + i);
                found.Add(new Event(id, Script[(codeAt + starts[i])..(codeAt + starts[i + 1])]));
            }

            return found;
        }
    }

    /// <summary>
    /// Which events this entity has, as the server names them.
    ///
    /// Slots the server cannot name - <c>0xFFFF</c> - are left out, so this is
    /// the list of things that can actually be asked for.
    /// </summary>
    public IReadOnlyList<ushort> EventIds =>
        Events.Where(e => e.Id != 0xFFFF).Select(e => e.Id).ToArray();

    /// <summary>The code for one event, or null if this entity has no such event.</summary>
    public byte[]? CodeFor(ushort eventId) =>
        Events.FirstOrDefault(e => e.Id == eventId)?.Code;

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
