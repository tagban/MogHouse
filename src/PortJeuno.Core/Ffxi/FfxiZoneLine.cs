using System.Buffers.Binary;
using System.Globalization;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// One zone line: a spot in this zone that, when walked into, asks the server
/// to move the character to another zone.
/// </summary>
/// <param name="Id">
/// The four-character client token ('z6j0') packed into a uint32 as its
/// little-endian bytes - which is what the request packet carries.
/// </param>
/// <param name="Token">The token in its readable form, for logs.</param>
/// <param name="From">Where the line sits in the current zone.</param>
/// <param name="Destination">The destination zone's name, as used for its data directory.</param>
/// <param name="Radius">
/// How close counts as touching it. The data gives a two-element `scale`; this
/// takes the larger, since the pair describes a box and being generous costs
/// only an early zone rather than a missed one.
/// </param>
public sealed record FfxiZoneLine(
    uint Id,
    string Token,
    float FromX,
    float FromVertical,
    float FromDepth,
    string Destination,
    float Radius)
{
    /// <summary>
    /// Packs a four-character token the way the server does: each character
    /// shifted into place by its index, so 'z6j0' is 'z' | '6'&lt;&lt;8 | 'j'&lt;&lt;16 | '0'&lt;&lt;24.
    /// </summary>
    public static uint PackId(string token)
    {
        if (token.Length != 4)
        {
            throw new ArgumentException($"Zone line token '{token}' is not four characters.", nameof(token));
        }

        uint id = 0;
        for (int i = 0; i < 4; i++)
        {
            id |= (uint)(byte)token[i] << (i * 8);
        }
        return id;
    }

    /// <summary>Squared distance from a point, ignoring height - zone lines are effectively vertical columns.</summary>
    public float DistanceSquaredTo(float x, float depth)
    {
        float dx = x - FromX;
        float dz = depth - FromDepth;
        return (dx * dx) + (dz * dz);
    }
}

/// <summary>
/// Reads the `zonelines:` block out of a zone's `zone.yaml`.
///
/// The full file is YAML, but the part needed here is a fixed, shallow shape,
/// so this reads it directly rather than pulling in a YAML parser for four
/// keys. If the schema ever grows, swap this for a real parser rather than
/// extending the pattern matching.
///
/// Shape:
///   zonelines:
///     z6j0:
///       from:  [x, y, z]
///       to:    bastok_mines
///       at:    [x, y, z, rotation]
///       scale: [a, b]
/// </summary>
public static class FfxiZoneLineReader
{
    public static IReadOnlyList<FfxiZoneLine> Read(string zoneYamlPath)
    {
        if (!File.Exists(zoneYamlPath))
        {
            return [];
        }

        var lines = new List<FfxiZoneLine>();
        string[] text = File.ReadAllLines(zoneYamlPath);

        int start = Array.FindIndex(text, l => l.TrimEnd() == "zonelines:");
        if (start < 0)
        {
            return [];
        }

        string? token = null;
        float[]? from = null;
        string? destination = null;
        float radius = 2f;

        void Flush()
        {
            if (token is not null && from is { Length: >= 3 } && destination is not null)
            {
                lines.Add(new FfxiZoneLine(
                    FfxiZoneLine.PackId(token), token,
                    from[0], from[1], from[2],
                    destination, radius));
            }

            token = null;
            from = null;
            destination = null;
            radius = 2f;
        }

        for (int i = start + 1; i < text.Length; i++)
        {
            string line = text[i];
            if (line.Trim().Length == 0)
            {
                continue;
            }

            int indent = line.Length - line.TrimStart().Length;

            // A top-level key ends the block.
            if (indent == 0)
            {
                break;
            }

            string trimmed = line.Trim();

            // A four-character key at the shallower indent starts a new entry.
            if (trimmed.EndsWith(':') && !trimmed.Contains(' '))
            {
                Flush();
                token = trimmed[..^1];
                continue;
            }

            if (trimmed.StartsWith("from:", StringComparison.Ordinal))
            {
                from = ParseNumbers(trimmed["from:".Length..]);
            }
            else if (trimmed.StartsWith("to:", StringComparison.Ordinal))
            {
                destination = trimmed["to:".Length..].Trim();
            }
            else if (trimmed.StartsWith("scale:", StringComparison.Ordinal))
            {
                float[] scale = ParseNumbers(trimmed["scale:".Length..]);
                radius = scale.Length > 0 ? scale.Max() : 2f;
            }
        }

        Flush();
        return lines;
    }

    private static float[] ParseNumbers(string text) =>
        text.Trim().Trim('[', ']')
            .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(p => float.TryParse(p, NumberStyles.Float, CultureInfo.InvariantCulture, out float v) ? v : 0f)
            .ToArray();
}

/// <summary>
/// GP_CLI_COMMAND_MAPRECT (C2S 0x05E) - "requesting to change zones after
/// touching a zone line". The reply is a 0x00B, which is where the zone
/// handoff and key rotation come from.
/// </summary>
public static class FfxiZoneLinePacket
{
    public const ushort PacketId = 0x05E;
    public const int PacketSize = 24;

    private const int OffsetIdAndSize = 0;
    private const int OffsetSync = 2;
    private const int OffsetRectId = 4;
    private const int OffsetX = 8;
    private const int OffsetVertical = 12;
    private const int OffsetDepth = 16;
    private const int OffsetActIndex = 20;
    private const int OffsetMyRoomExitBit = 22;
    private const int OffsetMyRoomExitMode = 23;

    public static byte[] Build(uint rectId, float x, float vertical, float depth, ushort actIndex, ushort sync)
    {
        var packet = new byte[PacketSize];

        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetIdAndSize, 2), FfxiZonePacket.PackIdAndSize(PacketId, PacketSize));
        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetSync, 2), sync);
        BinaryPrimitives.WriteUInt32LittleEndian(packet.AsSpan(OffsetRectId, 4), rectId);

        // Same axis order as every other position on the wire.
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(OffsetX, 4), x);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(OffsetVertical, 4), vertical);
        BinaryPrimitives.WriteSingleLittleEndian(packet.AsSpan(OffsetDepth, 4), depth);

        BinaryPrimitives.WriteUInt16LittleEndian(packet.AsSpan(OffsetActIndex, 2), actIndex);

        // Both are validated against their enums; 0 is the default member of
        // each, and anything else would be rejected.
        packet[OffsetMyRoomExitBit] = 0;
        packet[OffsetMyRoomExitMode] = 0;

        return packet;
    }
}
