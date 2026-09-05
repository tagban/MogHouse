using System.Buffers.Binary;
using Microsoft.Win32.SafeHandles;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// One 32x32 item icon, expanded from the paletted image the DAT stores.
/// </summary>
/// <param name="Rgba">Width * Height * 4 bytes, row 0 at the top.</param>
public sealed record FfxiItemIcon(int Width, int Height, byte[] Rgba);

/// <summary>
/// Everything the client knows about an item without asking the server.
///
/// The numbers are as the DAT stores them, not as LandSandBoat stores them.
/// The one place they differ is <see cref="Jobs"/> - see that field.
/// </summary>
public sealed record FfxiItem(
    ushort Id,
    string Name,
    string LogSingular,
    string LogPlural,
    string Description,
    ushort Flags,
    ushort StackSize,
    ushort Type,
    ushort Level,
    ushort Slots,
    ushort Races,
    uint Jobs,
    ushort Damage,
    ushort Delay,
    ushort Skill)
{
    /// <summary>Whether this goes in an equipment slot rather than a bag.</summary>
    public bool IsEquipment => Slots != 0;

    /// <summary>
    /// The DAT's type for a linkshell or a linkpearl.
    ///
    /// Both Linkshell (513) and Linkpearl (515) carry 6 here, where a crystal
    /// carries 8 and a weapon its own. The server keeps the same idea as a
    /// bitmask, ITEM_LINKSHELL = 0x80; the file keeps it as an ordinal.
    /// </summary>
    public const ushort LinkshellType = 6;

    /// <summary>
    /// Whether this is a linkshell, which is worn but has no equipment fields.
    ///
    /// A linkshell record stops before the block that holds
    /// <see cref="Slots"/>, so it reads as Slots 0 and
    /// <see cref="IsEquipment"/> false - correctly, since it is not equipment
    /// in the sense the rest of that block describes. It is still worn, in
    /// <see cref="FfxiEquipSlot.Linkshell"/>, and this is the only thing in
    /// the record that says so.
    /// </summary>
    public bool IsLinkshell => Type == LinkshellType;

    /// <summary>Whether this can be worn at all, by either route.</summary>
    public bool IsWearable => IsEquipment || IsLinkshell;

    /// <summary>Whether a slot number (SLOTTYPE, 0 main to 15 back) can take this.</summary>
    public bool FitsSlot(int slot) => slot is >= 0 and < 16 && (Slots & (1 << slot)) != 0;

    /// <summary>
    /// Whether a job can wear this. Job numbers are the game's own - 1 warrior,
    /// 2 monk, and so on - which is what the DAT indexes by.
    /// </summary>
    public bool AllowsJob(int job) => job is >= 0 and < 32 && (Jobs & (1u << job)) != 0;
}

/// <summary>
/// Reads item names, descriptions, stats and icons out of the retail client's
/// item DATs.
///
/// There are four of them and they carve the id space into fixed ranges. Both
/// languages are present in every install, as the same four files twice:
///
///     contents   English  Japanese  records  first id
///     general      73        4        4096    0x0000
///     usable       74        5        4096    0x1000
///     weapon       75        6        6656    0x4000
///     armour       76        7        6144    0x2800
///
/// A record is a fixed 0xC00 bytes and its position is the item id minus the
/// file's first id - there is no index to consult. Every byte is stored
/// rotated right by five bits, which is the whole of the "encryption".
///
/// Verified by reading all 21,088 English records: every one parses, every
/// one carries a name, and the id in the record matches the id its position
/// implies in all of them.
///
/// The layout was derived from the files themselves. Where a field's meaning
/// was a guess, it was checked against LandSandBoat's own item tables - level,
/// slot and job list agree for every item spot-checked. No code or data comes
/// from that server, which is GPL; only the confirmation that a reading was
/// right.
/// </summary>
public sealed class FfxiItemTable : IDisposable
{
    /// <summary>Bytes per record. Data first, then the icon.</summary>
    private const int RecordSize = 0xC00;

    /// <summary>Where the icon starts inside a record.</summary>
    private const int IconOffset = 0x280;

    /// <summary>
    /// One of the four DATs, and where its records keep their string table.
    ///
    /// That offset is just the size of the record's fixed header, so it varies
    /// with how much a category has to say: a crystal needs less room than a
    /// sword. It does not vary within a file.
    /// </summary>
    private sealed record Source(int FileId, int TableOffset)
    {
        public SafeFileHandle? Handle { get; set; }
        public ushort FirstId { get; set; }
        public int Count { get; set; }
    }

    /// <summary>English, in id order.</summary>
    private static readonly (int FileId, int TableOffset)[] English =
        [(73, 0x18), (74, 0x1C), (75, 0x38), (76, 0x2C)];

    /// <summary>Japanese. Same four files, same four layouts, different text.</summary>
    private static readonly (int FileId, int TableOffset)[] Japanese =
        [(4, 0x18), (5, 0x1C), (6, 0x38), (7, 0x2C)];

    private readonly List<Source> _sources = [];
    private readonly Dictionary<ushort, FfxiItem> _cache = [];
    private readonly Lock _gate = new();

    /// <summary>An empty table: every lookup misses. For when there is no install.</summary>
    public static FfxiItemTable Empty { get; } = new();

    private FfxiItemTable()
    {
    }

    /// <param name="files">The install's file table, to turn file ids into paths.</param>
    /// <param name="japanese">Read the Japanese text rather than the English.</param>
    public FfxiItemTable(FfxiFileTable files, bool japanese = false)
    {
        foreach ((int fileId, int tableOffset) in japanese ? Japanese : English)
        {
            if (files.Path(fileId) is not { } path || !File.Exists(path))
            {
                continue;
            }

            long length = new FileInfo(path).Length;
            if (length == 0 || length % RecordSize != 0)
            {
                continue;
            }

            SafeFileHandle handle = File.OpenHandle(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            var source = new Source(fileId, tableOffset)
            {
                Handle = handle,
                Count = (int)(length / RecordSize),
            };

            // Record 0 is often blank, so the first id comes from record 1.
            // Reading it here also proves the file decodes at all.
            byte[] second = Record(source, 1);
            uint id = BinaryPrimitives.ReadUInt32LittleEndian(second);
            if (id == 0 || id > ushort.MaxValue)
            {
                handle.Dispose();
                continue;
            }

            source.FirstId = (ushort)(id - 1);
            _sources.Add(source);
        }
    }

    /// <summary>Whether any of the four files was found.</summary>
    public bool IsLoaded => _sources.Count > 0;

    /// <summary>The item, or null if that id is not in any of the four files.</summary>
    public FfxiItem? Item(ushort id)
    {
        lock (_gate)
        {
            if (_cache.TryGetValue(id, out FfxiItem? cached))
            {
                return cached;
            }

            if (Locate(id) is not var (source, index))
            {
                return null;
            }

            FfxiItem? item = Parse(source, Record(source, index), id);
            if (item is not null)
            {
                _cache[id] = item;
            }

            return item;
        }
    }

    /// <summary>
    /// The item's icon, expanded to straight RGBA, or null if that id is not
    /// in any of the four files.
    ///
    /// Not cached: an icon is 4KB expanded and a caller that wants many of them
    /// wants them on a texture atlas, not in a dictionary.
    /// </summary>
    public FfxiItemIcon? Icon(ushort id)
    {
        lock (_gate)
        {
            return Locate(id) is var (source, index) ? ReadIcon(Record(source, index)) : null;
        }
    }

    /// <summary>Which file holds an id, and where in it.</summary>
    private (Source Source, int Index)? Locate(ushort id)
    {
        foreach (Source source in _sources)
        {
            int index = id - source.FirstId;
            if (index >= 0 && index < source.Count)
            {
                return (source, index);
            }
        }

        return null;
    }

    /// <summary>One decoded record. Every byte is rotated right by five bits.</summary>
    private static byte[] Record(Source source, int index)
    {
        byte[] buffer = new byte[RecordSize];
        RandomAccess.Read(source.Handle!, buffer, (long)index * RecordSize);
        for (int i = 0; i < buffer.Length; i++)
        {
            buffer[i] = (byte)((buffer[i] >> 5) | (buffer[i] << 3));
        }

        return buffer;
    }

    private static ushort U16(byte[] d, int at) => BinaryPrimitives.ReadUInt16LittleEndian(d.AsSpan(at));

    private static uint U32(byte[] d, int at) => BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(at));

    private static FfxiItem? Parse(Source source, byte[] d, ushort id)
    {
        int table = source.TableOffset;

        // Five strings, always: the name, one that is always blank, the
        // singular and plural the chat log uses, and the description.
        if (U32(d, table) != 5)
        {
            return null;
        }

        string[] text = new string[5];
        for (int i = 0; i < 5; i++)
        {
            text[i] = String(d, table + (int)U32(d, table + 4 + (i * 8)));
        }

        // Weapons and armour carry equipment fields where the smaller
        // categories carry nothing; only those two have a level worth reading.
        bool equipment = source.TableOffset is 0x2C or 0x38;
        bool weapon = source.TableOffset == 0x38;

        return new FfxiItem(
            Id: id,
            Name: text[0],
            LogSingular: text[2],
            LogPlural: text[3],
            Description: text[4],
            Flags: U16(d, 0x04),
            StackSize: U16(d, 0x06),
            Type: U16(d, 0x08),
            Level: equipment ? U16(d, 0x0E) : (ushort)0,
            Slots: equipment ? U16(d, 0x10) : (ushort)0,
            Races: equipment ? U16(d, 0x12) : (ushort)0,
            Jobs: equipment ? U32(d, 0x14) : 0,
            Damage: weapon ? U16(d, 0x1C) : (ushort)0,
            Delay: weapon ? U16(d, 0x1E) : (ushort)0,
            Skill: weapon ? U16(d, 0x22) : (ushort)0);
    }

    /// <summary>
    /// One string from the table. The offset points at a small block - a
    /// count, then that many 24-byte attributes, none of which has yet been
    /// seen to hold anything - and the text follows it, NUL terminated.
    /// </summary>
    private static string String(byte[] d, int block)
    {
        if (block < 0 || block + 4 > d.Length)
        {
            return string.Empty;
        }

        uint attributes = U32(d, block);
        int at = block + 4 + ((int)attributes * 0x18);
        if (attributes > 8 || at >= d.Length)
        {
            return string.Empty;
        }

        int end = Array.IndexOf(d, (byte)0, at);
        if (end < 0)
        {
            end = d.Length;
        }

        return Decode(d.AsSpan(at, end - at));
    }

    /// <summary>The eight elements, in the order the game always lists them.</summary>
    private static readonly string[] Elements =
        ["Fire", "Ice", "Wind", "Earth", "Lightning", "Water", "Light", "Dark"];

    /// <summary>
    /// Turns a description's bytes into readable text.
    ///
    /// Mostly ASCII, but not entirely. A resistance is written with a symbol
    /// rather than a word - 0xEF then 0x1F to 0x26, which is Fire through Dark
    /// in the usual order. That was confirmed against LandSandBoat's own
    /// modifier list: Scorpion Harness reads 0xEF20 -20, 0xEF24 +15, 0xEF26 +15
    /// and the server gives it ice -20, water +15, dark +15.
    ///
    /// The retail client draws those as the little coloured orbs. Spelling the
    /// element out is what the community does in writing, and is what a caller
    /// without an orb to draw wants.
    ///
    /// A few other high bytes lead a two-byte pair borrowed from Japanese
    /// punctuation - a wave dash, a bullet, the arrows an enchantment uses for
    /// "here to there". Those get an ASCII stand-in; anything else two-byte is
    /// dropped rather than turned into mojibake.
    /// </summary>
    private static string Decode(ReadOnlySpan<byte> text)
    {
        var built = new System.Text.StringBuilder(text.Length);
        for (int i = 0; i < text.Length; i++)
        {
            byte b = text[i];
            if (b is 0x0A or >= 0x20 and < 0x7F)
            {
                built.Append((char)b);
            }
            else if (b == 0xEF && i + 1 < text.Length)
            {
                byte symbol = text[++i];
                built.Append(symbol is >= 0x1F and <= 0x26 ? Elements[symbol - 0x1F] : ' ');
            }
            else if (b is 0x81 or 0x85 or 0x87 or 0x9A && i + 1 < text.Length)
            {
                built.Append(text[++i] switch
                {
                    0x45 when b == 0x81 => "-",
                    0x60 when b == 0x81 => "~",
                    0xCB when b == 0x81 => "<=>",
                    0xCC when b == 0x81 => "=>",
                    _ => string.Empty,
                });
            }
        }

        return built.ToString();
    }

    /// <summary>
    /// The icon, which is an ordinary Windows DIB with a name stuck on the
    /// front: a length, a flag byte, a sixteen-character label like
    /// "armor   12568   ", then a BITMAPINFOHEADER, a 256-entry palette and
    /// one byte per pixel. The rows run bottom-up, as DIB rows do.
    ///
    /// Alpha is the usual FFXI half-scale, where 0x80 means opaque.
    /// </summary>
    private static FfxiItemIcon? ReadIcon(byte[] d)
    {
        const int header = IconOffset + 4 + 1 + 16;
        if (U32(d, IconOffset) == 0 || U32(d, header) != 40)
        {
            return null;
        }

        int width = (int)U32(d, header + 4);
        int height = (int)U32(d, header + 8);
        int bits = U16(d, header + 14);
        if (width <= 0 || height <= 0 || bits != 8)
        {
            return null;
        }

        int palette = header + 40;
        int pixels = palette + (256 * 4);
        if (pixels + (width * height) > d.Length)
        {
            return null;
        }

        byte[] rgba = new byte[width * height * 4];
        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                int entry = palette + (d[pixels + ((height - 1 - y) * width) + x] * 4);
                int o = ((y * width) + x) * 4;
                rgba[o + 0] = d[entry + 2];
                rgba[o + 1] = d[entry + 1];
                rgba[o + 2] = d[entry + 0];
                rgba[o + 3] = (byte)Math.Min(255, d[entry + 3] * 2);
            }
        }

        return new FfxiItemIcon(width, height, rgba);
    }

    public void Dispose()
    {
        foreach (Source source in _sources)
        {
            source.Handle?.Dispose();
        }

        _sources.Clear();
    }
}
