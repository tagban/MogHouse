using System.Buffers.Binary;
using System.Text;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// The lines of text one zone can say.
///
/// The server rarely sends words. It sends the id of a line and lets the
/// client find it, which is why an NPC talks in the language the client was
/// installed in rather than the one the server was built in. Every id in
/// TALKNUM and TALKNUMWORK indexes this table.
///
/// The file is XORed with 0x80 end to end. A u32 offset table starts at byte
/// 4 and runs until the first offset it points at; each entry is four bytes
/// of header, the text, then 0xFF - 0x7F once the XOR is undone.
///
/// Checked against LandSandBoat's own IDs.lua for Port Windurst, Bastok
/// Markets and Windurst Walls, which quote by id the strings they use: 107 of
/// 119 come back character for character, the remainder being comments that
/// paraphrase or truncate what the DAT holds.
/// </summary>
public sealed class FfxiDialogueTable
{
    /// <summary>Zone 0's dialogue is file 6420; every zone follows its id.</summary>
    public const int DialogueFileIdOffset = 6420;

    private readonly string[] _lines;
    private readonly string[][] _options;

    private FfxiDialogueTable(string[] lines, string[][] options)
    {
        _lines = lines;
        _options = options;
    }

    public int Count => _lines.Length;

    /// <summary>An empty table, for zones whose file is missing.</summary>
    public static FfxiDialogueTable Empty { get; } = new([], []);

    /// <summary>The line with this id, or null if the zone has no such line.</summary>
    public string? Line(int id) =>
        id >= 0 && id < _lines.Length && _lines[id].Length > 0 ? _lines[id] : null;

    /// <summary>
    /// The choices offered under a line, or empty when it is only speech.
    ///
    /// A menu is not held anywhere separate: 0x0B opens the list inside the
    /// text itself and each 0x07 after it starts the next choice, so
    /// "Set this as current home point?" carries its own Yes and No. Before
    /// this was read they were silently run onto the end of the line, which
    /// is why some NPC text ended in a stray "Yes.No."
    /// </summary>
    public IReadOnlyList<string> Options(int id) =>
        id >= 0 && id < _options.Length ? _options[id] : [];

    public static FfxiDialogueTable Load(FfxiFileTable table, int zoneId)
    {
        string? path = table.Path(DialogueFileIdOffset + zoneId);
        if (path is null || !File.Exists(path))
        {
            return Empty;
        }

        byte[] plain = File.ReadAllBytes(path);
        for (int i = 0; i < plain.Length; i++)
        {
            plain[i] ^= 0x80;
        }

        if (plain.Length < 8)
        {
            return Empty;
        }

        // The first offset is where the text begins, so it is also where the
        // offset table ends - the table does not carry its own length.
        int first = (int)BinaryPrimitives.ReadUInt32LittleEndian(plain.AsSpan(4, 4));
        if (first <= 4 || first > plain.Length)
        {
            return Empty;
        }

        int count = (first - 4) / 4;
        var lines = new string[count];
        var options = new string[count][];
        for (int i = 0; i < count; i++)
        {
            int offset = (int)BinaryPrimitives.ReadUInt32LittleEndian(plain.AsSpan(4 + i * 4, 4));
            if (offset > 0 && offset < plain.Length)
            {
                (lines[i], options[i]) = Decode(plain, offset + 4);
            }
            else
            {
                (lines[i], options[i]) = ("", []);
            }
        }

        return new FfxiDialogueTable(lines, options);
    }

    /// <summary>One entry, from its first byte of text to its terminator.</summary>
    private static (string Text, string[] Options) Decode(byte[] plain, int at)
    {
        var text = new StringBuilder();
        var options = new List<StringBuilder>();

        // Everything goes into the text until 0x0B says the rest is a menu.
        StringBuilder into = text;
        for (int i = at; i < plain.Length; i++)
        {
            byte b = plain[i];
            if (b == 0x00 || b == 0x7F)
            {
                break;
            }

            if (b == 0x0B)
            {
                options.Add(new StringBuilder());
                into = options[^1];
                continue;
            }

            if (b == 0x07)
            {
                // A line break inside the text, but the start of the next
                // choice once the menu has opened.
                if (into == text)
                {
                    text.Append('\n');
                }
                else
                {
                    options.Add(new StringBuilder());
                    into = options[^1];
                }
            }
            else if ((b == 0x1E || b == 0x1F) && i + 1 < plain.Length)
            {
                // A colour change, and the byte after it is which colour -
                // not a letter. Dropping the 0x1F and keeping its parameter
                // put a stray character in front of every coloured line:
                // "yYou will be able to use the Assist Channel", where the y
                // is 0x79 being mistaken for text. The colour itself is not
                // used yet; both bytes are stepped over so the words are right.
                ++i;
            }
            else if ((b == 0x81 || b == 0x87) && i + 1 < plain.Length)
            {
                // Two byte sequences. 0x87 0xB2 and 0x87 0xB3 are the quotes
                // menu names are given in - "Map", "Markers" - and are worth
                // keeping; the rest render as nothing in English text.
                byte pair = plain[++i];
                if (b == 0x87 && (pair == 0xB2 || pair == 0xB3))
                {
                    into.Append('"');
                }
            }
            else if (b >= 0x20)
            {
                into.Append((char)b);
            }
        }

        string[] choices = options.Select(o => o.ToString().Trim())
                                  .Where(o => o.Length > 0)
                                  .ToArray();
        return (text.ToString(), choices);
    }
}
