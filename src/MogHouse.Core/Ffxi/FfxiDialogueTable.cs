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

    private FfxiDialogueTable(string[] lines) => _lines = lines;

    public int Count => _lines.Length;

    /// <summary>An empty table, for zones whose file is missing.</summary>
    public static FfxiDialogueTable Empty { get; } = new([]);

    /// <summary>The line with this id, or null if the zone has no such line.</summary>
    public string? Line(int id) =>
        id >= 0 && id < _lines.Length && _lines[id].Length > 0 ? _lines[id] : null;

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
        for (int i = 0; i < count; i++)
        {
            int offset = (int)BinaryPrimitives.ReadUInt32LittleEndian(plain.AsSpan(4 + i * 4, 4));
            lines[i] = offset > 0 && offset < plain.Length ? Decode(plain, offset + 4) : "";
        }

        return new FfxiDialogueTable(lines);
    }

    /// <summary>One entry, from its first byte of text to its terminator.</summary>
    private static string Decode(byte[] plain, int at)
    {
        var text = new StringBuilder();
        for (int i = at; i < plain.Length; i++)
        {
            byte b = plain[i];
            if (b == 0x00 || b == 0x7F)
            {
                break;
            }

            if (b == 0x07)
            {
                // A line break inside one entry, not a separator between two.
                text.Append('\n');
            }
            else if ((b == 0x81 || b == 0x87) && i + 1 < plain.Length)
            {
                // Two byte sequences. 0x87 0xB2 and 0x87 0xB3 are the quotes
                // menu names are given in - "Map", "Markers" - and are worth
                // keeping; the rest render as nothing in English text.
                byte pair = plain[++i];
                if (b == 0x87 && (pair == 0xB2 || pair == 0xB3))
                {
                    text.Append('"');
                }
            }
            else if (b >= 0x20)
            {
                text.Append((char)b);
            }
        }

        return text.ToString();
    }
}
