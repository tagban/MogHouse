using PortJeuno.Core.Ffxi;

namespace PortJeuno.Core.Tests.Ffxi;

/// <summary>
/// The reference values here were NOT produced by this C# port. They came from
/// compiling LandSandBoat/server's actual zlib.cpp standalone (MSVC, this
/// environment's toolchain), loading the real compress.dat/decompress.dat
/// tables, and running the same inputs through it.
///
/// These tests skip rather than fail when the tables aren't installed, because
/// PortJeuno deliberately doesn't ship them - see FfxiHuffmanTables. A skip
/// says "not verified here" honestly; a silent pass would claim verification
/// that never happened. Set PORTJEUNO_FFXI_RES to a directory holding both
/// files to run them.
/// </summary>
public class FfxiHuffmanTests
{
    // "Questria", 8 bytes in.
    private const int ShortCaseBits = 96;
    private const string ShortCaseHex = "0132971932F483582CA20901";

    // Every byte value 0x00-0xFF, which exercises all 256 codes - including
    // the ones longer than 8 bits that the reference resolves with a
    // bit-by-bit tree walk rather than its 8-bit fast path.
    private const int AllBytesCaseBits = 2701;
    private const string AllBytesCaseHex = "0195EB3401F73A22A1351B054C7E83470011F9EF07DF084BA6934B64681C8A107A7FFA1E2809657008D44646DCCF2072FD256ABD72F785D4FCEE41C0A12D31204BC38F65B15DFFD2C2145DF2B47A7A1C251D823D2C5AFF3A5C52890418FB341E0632F7B414FD83067480A43F68F9091BC645A02F219E44808FB08223C3D7223C84264F4949DA0EF3188012F01516F58358990146BF40333E20A46C449E03BA3FAC5FC59EC0AEE7B1E03B91200C04A03261295FE88C832114FC06801C8D6E3428A1EA433910958F3958E265A4C7469031344B9D078CD8DF222D41EC34A647D2176569A23BB22559F2322E55D1941E666144EA258775397906E6FA841A1FC7A683624B0A31EA208871406EA2C31ED6220B3B90294A804437A8603626440A8DC88120E8411E267B16DBA402BDD8C724AC4A81E89153E2A08F3550940F49481266E413F972286751F805B046F2233D78A60F4403";

    private static FfxiHuffman RequireCodec()
    {
        FfxiHuffmanTables? tables = FfxiHuffmanTables.TryLoadDefault();
        Skip.If(tables is null,
            "FFXI compression tables not installed. Set PORTJEUNO_FFXI_RES to a directory containing " +
            $"{FfxiHuffmanTables.EncodeFileName} and {FfxiHuffmanTables.DecodeFileName}. These aren't bundled - see FfxiHuffmanTables.");
        return new FfxiHuffman(tables!);
    }

    private static byte[] AllByteValues()
    {
        var input = new byte[256];
        for (int i = 0; i < 256; i++)
        {
            input[i] = (byte)i;
        }
        return input;
    }

    [SkippableFact]
    public void Compress_ShortInput_MatchesRealReferenceBitsAndBytes()
    {
        FfxiHuffman codec = RequireCodec();
        byte[] input = "Questria"u8.ToArray();
        var output = new byte[256];

        int bits = codec.Compress(input, output);

        Assert.Equal(ShortCaseBits, bits);
        int meaningful = 1 + FfxiHuffman.CompressedByteLength(bits - 8);
        Assert.Equal(ShortCaseHex, Convert.ToHexString(output.AsSpan(0, meaningful)));
    }

    [SkippableFact]
    public void Compress_AllByteValues_MatchesRealReferenceBitsAndBytes()
    {
        FfxiHuffman codec = RequireCodec();
        var output = new byte[8192];

        int bits = codec.Compress(AllByteValues(), output);

        Assert.Equal(AllBytesCaseBits, bits);
        int meaningful = 1 + FfxiHuffman.CompressedByteLength(bits - 8);
        Assert.Equal(AllBytesCaseHex, Convert.ToHexString(output.AsSpan(0, meaningful)));
    }

    /// <summary>
    /// Decoding the reference bytes directly - not this port's own output -
    /// so a matching pair of encode/decode bugs can't cancel out and pass.
    /// </summary>
    [SkippableFact]
    public void Decompress_RealReferenceBytes_ProducesOriginalText()
    {
        FfxiHuffman codec = RequireCodec();
        byte[] compressed = Convert.FromHexString(ShortCaseHex);
        var output = new byte[256];

        int written = codec.Decompress(compressed, ShortCaseBits - 8, output);

        Assert.Equal(8, written);
        Assert.Equal("Questria", System.Text.Encoding.ASCII.GetString(output, 0, written));
    }

    [SkippableFact]
    public void Decompress_RealReferenceBytes_RecoversAll256Symbols()
    {
        FfxiHuffman codec = RequireCodec();
        byte[] compressed = Convert.FromHexString(AllBytesCaseHex);
        var output = new byte[8192];

        int written = codec.Decompress(compressed, AllBytesCaseBits - 8, output);

        Assert.Equal(256, written);
        Assert.Equal(AllByteValues(), output.AsSpan(0, written).ToArray());
    }

    [SkippableFact]
    public void CompressThenDecompress_RoundTrips()
    {
        FfxiHuffman codec = RequireCodec();
        byte[] input = AllByteValues();
        var compressed = new byte[8192];
        var output = new byte[8192];

        int bits = codec.Compress(input, compressed);
        int written = codec.Decompress(compressed, bits - 8, output);

        Assert.Equal(input, output.AsSpan(0, written).ToArray());
    }

    [SkippableFact]
    public void Decompress_WrongMarker_Rejected()
    {
        FfxiHuffman codec = RequireCodec();
        byte[] compressed = Convert.FromHexString(ShortCaseHex);
        compressed[0] = 0x02;

        Assert.Equal(-1, codec.Decompress(compressed, ShortCaseBits - 8, new byte[256]));
    }

    [SkippableFact]
    public void Compress_OutputTooSmall_ReturnsNegativeOne()
    {
        FfxiHuffman codec = RequireCodec();

        Assert.Equal(-1, codec.Compress(AllByteValues(), new byte[8]));
    }

    [Fact]
    public void CompressedByteLength_ConvertsBitsToWholeBytes()
    {
        // Matches the server's own zlib_compressed_size helper.
        Assert.Equal(0, FfxiHuffman.CompressedByteLength(0));
        Assert.Equal(1, FfxiHuffman.CompressedByteLength(1));
        Assert.Equal(1, FfxiHuffman.CompressedByteLength(8));
        Assert.Equal(2, FfxiHuffman.CompressedByteLength(9));
        Assert.Equal(11, FfxiHuffman.CompressedByteLength(88));
    }
}
