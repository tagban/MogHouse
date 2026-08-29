using System.Buffers.Binary;
using PortJeuno.Core.Ffxi;

namespace PortJeuno.Core.Tests.Ffxi;

public class FfxiZoneLineTests
{
    /// <summary>
    /// The server packs the four-character token by shifting each character in
    /// by its index, so the id is the token's little-endian bytes. Pinned
    /// against a real token from Bastok Mines.
    /// </summary>
    [Theory]
    [InlineData("z6j0", 0x306A367AU)]
    [InlineData("z6i2", 0x3269367AU)]
    [InlineData("zmra", 0x61726D7AU)]
    public void PackId_MatchesTheServersPacking(string token, uint expected)
    {
        Assert.Equal(expected, FfxiZoneLine.PackId(token));
    }

    [Fact]
    public void PackId_RejectsWrongLength()
    {
        Assert.Throws<ArgumentException>(() => FfxiZoneLine.PackId("z6j"));
        Assert.Throws<ArgumentException>(() => FfxiZoneLine.PackId("z6j00"));
    }

    /// <summary>Height is ignored - a zone line is effectively a vertical column.</summary>
    [Fact]
    public void DistanceSquaredTo_IgnoresHeight()
    {
        var line = new FfxiZoneLine(1, "z6i2", 10f, 999f, 20f, "somewhere", 5f);

        Assert.Equal(0f, line.DistanceSquaredTo(10f, 20f));
        Assert.Equal(25f, line.DistanceSquaredTo(15f, 20f));
    }
}

public class FfxiZoneLineReaderTests
{
    private const string SampleYaml = """
        zone:
          name: Bastok_Mines

        zonelines:

          z6i0:
            from:  [-15.963, -5.474, -136.021]
            to:    south_gustaberg
            at:    [-100.0, 0.0, 0.0, 1.570796]
            scale: [1.000, 5.000]

          z6i2:
            from:  [-104.078, 8.100, 84.583]
            to:    bastok_markets
            at:    [-202.0, 0.0, -197.0, 3.141593]
            scale: [1.000, 5.000]

        npcs:
          something: else
        """;

    private static string WriteTemp()
    {
        string path = Path.Combine(Path.GetTempPath(), $"zone-{Guid.NewGuid():N}.yaml");
        File.WriteAllText(path, SampleYaml);
        return path;
    }

    [Fact]
    public void Read_ParsesEveryLineInTheBlock()
    {
        string path = WriteTemp();
        try
        {
            IReadOnlyList<FfxiZoneLine> lines = FfxiZoneLineReader.Read(path);

            Assert.Equal(2, lines.Count);
            Assert.Equal("z6i0", lines[0].Token);
            Assert.Equal("south_gustaberg", lines[0].Destination);
            Assert.Equal("z6i2", lines[1].Token);
            Assert.Equal("bastok_markets", lines[1].Destination);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void Read_TakesPositionAndRadius()
    {
        string path = WriteTemp();
        try
        {
            FfxiZoneLine line = FfxiZoneLineReader.Read(path)[1];

            Assert.Equal(-104.078f, line.FromX, 3);
            Assert.Equal(8.100f, line.FromVertical, 3);
            Assert.Equal(84.583f, line.FromDepth, 3);

            // scale is a pair describing a box; the larger is used.
            Assert.Equal(5f, line.Radius);
        }
        finally
        {
            File.Delete(path);
        }
    }

    /// <summary>The block ends at the next top-level key, not the end of file.</summary>
    [Fact]
    public void Read_StopsAtTheNextTopLevelKey()
    {
        string path = WriteTemp();
        try
        {
            Assert.DoesNotContain(FfxiZoneLineReader.Read(path), l => l.Token == "npcs");
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void Read_MissingFile_ReturnsEmpty()
    {
        Assert.Empty(FfxiZoneLineReader.Read(Path.Combine(Path.GetTempPath(), "does-not-exist.yaml")));
    }
}

public class FfxiZoneLinePacketTests
{
    [Fact]
    public void Build_HasCorrectIdSizeAndFields()
    {
        byte[] packet = FfxiZoneLinePacket.Build(
            rectId: 0x3269367A, x: -104.1f, vertical: 8.1f, depth: 84.6f, actIndex: 1025, sync: 7);

        Assert.Equal(FfxiZoneLinePacket.PacketSize, packet.Length);
        (ushort id, int size) = FfxiZonePacket.UnpackIdAndSize(BinaryPrimitives.ReadUInt16LittleEndian(packet));
        Assert.Equal(FfxiZoneLinePacket.PacketId, id);
        Assert.Equal(FfxiZoneLinePacket.PacketSize, size);

        Assert.Equal(7, BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(2, 2)));
        Assert.Equal(0x3269367AU, BinaryPrimitives.ReadUInt32LittleEndian(packet.AsSpan(4, 4)));
        Assert.Equal(1025, BinaryPrimitives.ReadUInt16LittleEndian(packet.AsSpan(20, 2)));
    }

    /// <summary>
    /// Both trailing bytes are validated against enums whose default member is
    /// 0; anything else and the server drops the packet.
    /// </summary>
    [Fact]
    public void Build_LeavesTheValidatedExitFieldsAtDefault()
    {
        byte[] packet = FfxiZoneLinePacket.Build(1, 0, 0, 0, 0, 1);

        Assert.Equal(0, packet[22]);
        Assert.Equal(0, packet[23]);
    }
}
