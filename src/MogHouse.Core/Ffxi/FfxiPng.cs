using System.Buffers.Binary;
using System.IO.Compression;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// Turns the renderer's screenshot into something worth sending.
///
/// <para>
/// The renderer writes a BMP because that needs no encoder, which is the right
/// trade for a debugging aid written to disk. It is the wrong one for a bug
/// report: a 2560x1440 frame is eleven megabytes uncompressed, over the limit
/// Discord will take from most accounts, and a waste of everyone's evening
/// where a PNG of the same frame is one or two.
/// </para>
///
/// <para>
/// So it is converted here rather than encoded there. .NET has had zlib since
/// 6, and a PNG is a header, a zlib stream of filtered scanlines and a footer -
/// which is about eighty lines and no dependency, against pulling an imaging
/// library into a client that draws its own graphics.
/// </para>
///
/// <para>
/// No filtering beyond "none" per row. Paeth would shrink a screenshot further
/// and it is not worth the arithmetic here: zlib alone takes a game frame down
/// by roughly ten to one, which is the difference that mattered.
/// </para>
/// </summary>
public static class FfxiPng
{
    private static ReadOnlySpan<byte> Signature => [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A];

    /// <summary>
    /// Reads a 24-bit BMP and writes it beside itself as a PNG, returning the
    /// new path - or null if it could not be read, in which case the caller
    /// should send what it has rather than nothing.
    /// </summary>
    public static string? FromBmp(string bmpPath)
    {
        try
        {
            byte[] bmp = File.ReadAllBytes(bmpPath);
            if (bmp.Length < 54 || bmp[0] != 'B' || bmp[1] != 'M')
            {
                return null;
            }

            int pixelOffset = BinaryPrimitives.ReadInt32LittleEndian(bmp.AsSpan(10, 4));
            int width = BinaryPrimitives.ReadInt32LittleEndian(bmp.AsSpan(18, 4));
            int height = BinaryPrimitives.ReadInt32LittleEndian(bmp.AsSpan(22, 4));
            int bits = BinaryPrimitives.ReadInt16LittleEndian(bmp.AsSpan(28, 2));

            if (width <= 0 || height <= 0 || bits != 24)
            {
                return null;
            }

            int rowBytes = width * 3;
            int padding = (4 - (rowBytes % 4)) % 4;
            if (pixelOffset + (long)(rowBytes + padding) * height > bmp.Length)
            {
                return null;
            }

            // Each PNG scanline is a filter byte then the row. BMP is BGR and
            // bottom-up; PNG is RGB and top-down, so this walks the source
            // backwards and swaps two channels on the way.
            var raw = new byte[(rowBytes + 1) * height];
            for (int y = 0; y < height; y++)
            {
                int from = pixelOffset + (height - 1 - y) * (rowBytes + padding);
                int to = y * (rowBytes + 1);
                raw[to++] = 0;   // filter: none
                for (int x = 0; x < width; x++)
                {
                    raw[to + x * 3 + 0] = bmp[from + x * 3 + 2];   // R
                    raw[to + x * 3 + 1] = bmp[from + x * 3 + 1];   // G
                    raw[to + x * 3 + 2] = bmp[from + x * 3 + 0];   // B
                }
            }

            string pngPath = Path.ChangeExtension(bmpPath, ".png");
            using (var file = File.Create(pngPath))
            {
                file.Write(Signature);

                var header = new byte[13];
                BinaryPrimitives.WriteInt32BigEndian(header.AsSpan(0, 4), width);
                BinaryPrimitives.WriteInt32BigEndian(header.AsSpan(4, 4), height);
                header[8] = 8;    // bits per channel
                header[9] = 2;    // colour type: truecolour, no alpha
                header[10] = 0;   // deflate
                header[11] = 0;   // adaptive filtering
                header[12] = 0;   // no interlace
                WriteChunk(file, "IHDR", header);

                using var squashed = new MemoryStream();
                using (var deflate = new ZLibStream(squashed, CompressionLevel.Optimal, leaveOpen: true))
                {
                    deflate.Write(raw);
                }
                WriteChunk(file, "IDAT", squashed.ToArray());
                WriteChunk(file, "IEND", []);
            }

            return pngPath;
        }
        catch (Exception)
        {
            return null;
        }
    }

    /// <summary>
    /// Writes straight RGBA out as a PNG with its alpha kept - which is what
    /// an item icon needs and <see cref="FromBmp"/> cannot give it, a BMP
    /// having thrown the alpha away before it ever got here.
    /// </summary>
    public static void WriteRgba(string path, int width, int height, ReadOnlySpan<byte> rgba)
    {
        int rowBytes = width * 4;
        var raw = new byte[(rowBytes + 1) * height];
        for (int y = 0; y < height; y++)
        {
            raw[y * (rowBytes + 1)] = 0;   // filter: none
            rgba.Slice(y * rowBytes, rowBytes).CopyTo(raw.AsSpan((y * (rowBytes + 1)) + 1));
        }

        using var file = File.Create(path);
        file.Write(Signature);

        var header = new byte[13];
        BinaryPrimitives.WriteInt32BigEndian(header.AsSpan(0, 4), width);
        BinaryPrimitives.WriteInt32BigEndian(header.AsSpan(4, 4), height);
        header[8] = 8;    // bits per channel
        header[9] = 6;    // colour type: truecolour with alpha
        WriteChunk(file, "IHDR", header);

        using var squashed = new MemoryStream();
        using (var deflate = new ZLibStream(squashed, CompressionLevel.Optimal, leaveOpen: true))
        {
            deflate.Write(raw);
        }
        WriteChunk(file, "IDAT", squashed.ToArray());
        WriteChunk(file, "IEND", []);
    }

    /// <summary>Length, type, payload, and a CRC over the last two.</summary>
    private static void WriteChunk(Stream into, string type, ReadOnlySpan<byte> payload)
    {
        Span<byte> length = stackalloc byte[4];
        BinaryPrimitives.WriteInt32BigEndian(length, payload.Length);
        into.Write(length);

        Span<byte> tagged = new byte[4 + payload.Length];
        for (int i = 0; i < 4; i++)
        {
            tagged[i] = (byte)type[i];
        }
        payload.CopyTo(tagged[4..]);
        into.Write(tagged);

        Span<byte> crc = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(crc, Crc32(tagged));
        into.Write(crc);
    }

    private static readonly uint[] CrcTable = BuildCrcTable();

    private static uint[] BuildCrcTable()
    {
        var table = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            uint c = n;
            for (int k = 0; k < 8; k++)
            {
                c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[n] = c;
        }
        return table;
    }

    private static uint Crc32(ReadOnlySpan<byte> data)
    {
        uint c = 0xFFFFFFFFu;
        foreach (byte b in data)
        {
            c = CrcTable[(c ^ b) & 0xFF] ^ (c >> 8);
        }
        return c ^ 0xFFFFFFFFu;
    }
}
