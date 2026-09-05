using System.Buffers.Binary;
using System.Text;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// Names for the things in a zone, which the server does not send.
///
/// An NPC arrives from the server with a position, a look and an empty name.
/// The name lives in the client's own files, one table per zone, which is why
/// a server database can have no row for a zone the client still populates -
/// and why an NPC could speak into the chat log with nothing in front of the
/// colon until this was read.
///
/// The file is file id 6720 + zone: a flat run of 32-byte records, 28 bytes of
/// NUL-padded name followed by the entity id the server uses. That id is
/// <c>0x1000000 | zone &lt;&lt; 12 | targid</c>, so every record identifies its own
/// zone. The renderer reads the same table for nameplates; see
/// renderer/ffxi/entitynames.h.
/// </summary>
public sealed class FfxiEntityNames
{
    /// <summary>Zone 0's names are file 6720; every zone follows its id.</summary>
    public const int EntityNameFileIdOffset = 6720;

    private const int RecordLength = 32;
    private const int NameLength = 28;

    private readonly Dictionary<uint, string> _names;

    private FfxiEntityNames(Dictionary<uint, string> names) => _names = names;

    public int Count => _names.Count;

    /// <summary>An empty table, for zones whose file is missing.</summary>
    public static FfxiEntityNames Empty { get; } = new([]);

    /// <summary>
    /// The name for an entity id, or null.
    ///
    /// Ids the table does not carry are normal rather than a fault: players
    /// are not in it, and neither is anything the server spawned itself.
    /// </summary>
    public string? Lookup(uint entityId) =>
        _names.TryGetValue(entityId, out string? name) ? name : null;

    public static FfxiEntityNames Load(FfxiFileTable table, int zoneId)
    {
        string? path = table.Path(EntityNameFileIdOffset + zoneId);
        if (path is null || !File.Exists(path))
        {
            return Empty;
        }

        byte[] data;
        try
        {
            data = File.ReadAllBytes(path);
        }
        catch (IOException)
        {
            // A missing name costs a label, not the zone.
            return Empty;
        }

        var names = new Dictionary<uint, string>();
        for (int offset = 0; offset + RecordLength <= data.Length; offset += RecordLength)
        {
            uint id = BinaryPrimitives.ReadUInt32LittleEndian(data.AsSpan(offset + NameLength, 4));
            if (id == 0)
            {
                continue;
            }

            // The name runs to the first NUL. Blank records are placeholders
            // rather than absent - the first entry of every zone is "none".
            int length = 0;
            while (length < NameLength && data[offset + length] != 0)
            {
                length++;
            }

            if (length == 0)
            {
                continue;
            }

            names[id] = Encoding.UTF8.GetString(data, offset, length);
        }

        return names.Count == 0 ? Empty : new FfxiEntityNames(names);
    }
}
