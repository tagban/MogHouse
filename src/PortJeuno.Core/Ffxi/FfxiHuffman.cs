using System.Buffers.Binary;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// FFXI's packet compression. The server calls this "zlib" and keeps it in a
/// file named zlib.cpp, but it is not zlib and shares nothing with it: it's a
/// static Huffman codec whose code table is fixed data, loaded from two files
/// rather than derived per-stream. Every zone/map packet except the opening
/// plaintext 0x00A is compressed with this and then Blowfish-encrypted, so
/// this is the gate on reading or sending anything in the zone protocol.
///
/// Ported from LandSandBoat/server's src/common/zlib.cpp (branch `base`,
/// 2026-08-28) and verified against that same code compiled standalone with
/// the real tables - see FfxiHuffmanTests.
///
/// Two things about it are easy to get wrong:
///  - **Sizes are in bits, not bytes.** `Compress` returns a bit count, and
///    `Decompress` takes one. The server's own `zlib_compressed_size` helper
///    is `(bits + 7) / 8`, and the packet's on-wire size field stores the bit
///    count directly.
///  - **The output has a one-byte header.** Compressed data starts with a
///    literal `0x01` marker at offset 0, with the bitstream from offset 1.
///    `Decompress` rejects anything else, and the returned bit count covers
///    only the bitstream - hence the `+ 8` the server adds on the way out and
///    the `- 8` implied on the way back in.
///
/// The tables themselves are not shipped with this project: they're GPLv3
/// files from the LandSandBoat repo whose contents very likely derive from
/// the retail client. <see cref="FfxiHuffmanTables"/> loads them from a path
/// instead, so distributing them stays a deliberate decision rather than an
/// accident of vendoring.
/// </summary>
public sealed class FfxiHuffman
{
    private readonly FfxiHuffmanTables _tables;

    public FfxiHuffman(FfxiHuffmanTables tables)
    {
        _tables = tables;
    }

    /// <summary>Marker byte at offset 0 of every compressed buffer.</summary>
    public const byte Marker = 0x01;

    /// <summary>The server's `zlib_compressed_size` - bits to whole bytes.</summary>
    public static int CompressedByteLength(int bits) => (bits + 7) / 8;

    /// <summary>
    /// Compresses <paramref name="input"/> into <paramref name="output"/>,
    /// returning the total bit count *including* the 8 bits of marker byte -
    /// matching `zlib_compress`'s `return read + 8`. Returns -1 if the output
    /// buffer is too small, as the original does.
    /// </summary>
    public int Compress(ReadOnlySpan<byte> input, Span<byte> output)
    {
        if (output.Length < 2)
        {
            return -1;
        }

        uint[] enc = _tables.Encode;
        uint maxBits = (uint)(output.Length - 1) * 8;

        ulong acc = 0;
        int accBits = 0;
        int outPos = 1; // offset 0 is the marker
        uint written = 0;

        foreach (byte b in input)
        {
            int index = (sbyte)b;
            uint length = enc[index + 0x180];

            if (length + written >= maxBits)
            {
                return -1;
            }

            uint value = enc[index + 0x80];
            uint code = length >= 32 ? value : value & ((1u << (int)length) - 1);

            acc |= (ulong)code << accBits;
            accBits += (int)length;
            written += length;

            while (accBits >= 8)
            {
                output[outPos++] = (byte)(acc & 0xFF);
                acc >>= 8;
                accBits -= 8;
            }
        }

        if (accBits > 0)
        {
            // The trailing partial byte's unused high bits stay zero; the
            // decoder never reads them, because each code carries its own
            // length.
            output[outPos] = (byte)(acc & 0xFF);
        }

        output[0] = Marker;
        return (int)written + 8;
    }

    /// <summary>
    /// Decompresses <paramref name="input"/> (marker byte included) into
    /// <paramref name="output"/>. <paramref name="bits"/> is the bitstream
    /// length *excluding* the marker byte - i.e. what `Compress` returned
    /// minus 8. Returns the number of bytes written, or -1 if the marker is
    /// wrong.
    ///
    /// This walks the decode tree a bit at a time. The reference adds an
    /// 8-bit lookup table in front of that walk as a pure speed optimization
    /// for codes of 8 bits or fewer; it resolves to the same symbols, so it's
    /// left out here rather than reproduced - the tests check this port
    /// against output produced *with* that fast path enabled, across all 256
    /// symbols, so the equivalence is verified rather than assumed.
    /// </summary>
    public int Decompress(ReadOnlySpan<byte> input, int bits, Span<byte> output)
    {
        if (input.Length < 1 || input[0] != Marker)
        {
            return -1;
        }

        uint[] tree = _tables.DecodeTree;
        uint stride = _tables.NodeBase;

        ReadOnlySpan<byte> data = input[1..];
        int written = 0;
        int bitPos = 0;

        while (bitPos < bits && written < output.Length)
        {
            int node = FfxiHuffmanTables.RootNode;

            while (true)
            {
                int bit = (data[bitPos >> 3] >> (bitPos & 7)) & 1;
                node = (int)((tree[node + bit] - stride) / 4);
                bitPos++;

                // A node whose two child slots are both null is a leaf, and
                // its symbol lives in the fourth slot.
                if (tree[node] == 0 && tree[node + 1] == 0)
                {
                    output[written++] = (byte)tree[node + 3];
                    break;
                }
            }
        }

        return written;
    }
}

/// <summary>
/// The two fixed code tables FFXI's compression needs, `compress.dat` (2KB)
/// and `decompress.dat` (10KB).
///
/// These files are NOT bundled with PortJeuno. They're GPLv3 in the
/// LandSandBoat repo and their contents very likely originate in the retail
/// client, so redistributing them is a licensing decision for whoever ships a
/// build - not something to make silently by committing them. Point
/// <see cref="Load"/> at wherever they live, or set the
/// <c>PORTJEUNO_FFXI_RES</c> environment variable.
/// </summary>
public sealed class FfxiHuffmanTables
{
    public const string EncodeFileName = "compress.dat";
    public const string DecodeFileName = "decompress.dat";

    /// <summary>
    /// The decode tree's root. `populate_jump_table` sets the table base to
    /// `dec[0] - 4`, so the very first entry always resolves to index 1 -
    /// the root is a constant, not something to search for.
    /// </summary>
    public const int RootNode = 1;

    /// <summary>Code words, indexed by <c>(sbyte)value + 0x80</c>.</summary>
    public uint[] Encode { get; }

    /// <summary>Code bit-lengths live in the same array at <c>(sbyte)value + 0x180</c>.</summary>
    public uint[] DecodeTree { get; }

    /// <summary>
    /// The offset the raw table values are biased by. The tables were dumped
    /// from a running process, so their "pointers" are absolute addresses from
    /// whatever machine produced them; subtracting this base and dividing by 4
    /// turns them back into indices.
    /// </summary>
    public uint NodeBase { get; }

    private FfxiHuffmanTables(uint[] encode, uint[] decodeTree)
    {
        Encode = encode;
        DecodeTree = decodeTree;
        NodeBase = decodeTree[0] - 4;
    }

    /// <summary>
    /// Loads both tables from <paramref name="directory"/>. Throws with the
    /// full path if either is missing - a wrong path here otherwise surfaces
    /// much later as unreadable packets.
    /// </summary>
    public static FfxiHuffmanTables Load(string directory)
    {
        return new FfxiHuffmanTables(
            ReadWords(Path.Combine(directory, EncodeFileName)),
            ReadWords(Path.Combine(directory, DecodeFileName)));
    }

    /// <summary>
    /// Looks for the tables in the usual places, returning null rather than
    /// throwing when they aren't installed - callers that can degrade (tests,
    /// tooling) can then say so plainly instead of crashing.
    /// </summary>
    public static FfxiHuffmanTables? TryLoadDefault()
    {
        foreach (string candidate in DefaultSearchPaths())
        {
            if (File.Exists(Path.Combine(candidate, EncodeFileName)) &&
                File.Exists(Path.Combine(candidate, DecodeFileName)))
            {
                return Load(candidate);
            }
        }

        return null;
    }

    /// <summary>
    /// Where <see cref="TryLoadDefault"/> looks, in order. Everything is
    /// relative to the executable rather than to a user profile, matching the
    /// project's portability goal - a build plus its assets should run from a
    /// copied folder with no install step.
    /// </summary>
    public static IEnumerable<string> DefaultSearchPaths()
    {
        string? fromEnvironment = Environment.GetEnvironmentVariable("PORTJEUNO_FFXI_RES");
        if (!string.IsNullOrWhiteSpace(fromEnvironment))
        {
            yield return fromEnvironment;
        }

        string baseDirectory = AppContext.BaseDirectory;
        yield return Path.Combine(baseDirectory, "res");
        yield return Path.Combine(baseDirectory, "PlayonlineAssets", "res");
        yield return Path.Combine(baseDirectory, "..", "Playonline Assets", "res");
    }

    private static uint[] ReadWords(string path)
    {
        if (!File.Exists(path))
        {
            throw new FileNotFoundException(
                $"FFXI compression table not found: {path}. These tables aren't bundled with PortJeuno - " +
                $"point PORTJEUNO_FFXI_RES at a directory containing {EncodeFileName} and {DecodeFileName}.",
                path);
        }

        byte[] bytes = File.ReadAllBytes(path);
        var words = new uint[bytes.Length / 4];
        for (int i = 0; i < words.Length; i++)
        {
            words[i] = BinaryPrimitives.ReadUInt32LittleEndian(bytes.AsSpan(i * 4, 4));
        }
        return words;
    }
}
