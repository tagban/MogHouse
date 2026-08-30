using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class FfxiFileTableTests : IDisposable
{
    private readonly string _root = Directory.CreateTempSubdirectory("moghouse-filetable").FullName;

    /// <summary>
    /// Builds a table with three ids: 0 absent, 1 in ROM, 2 in ROM3.
    /// </summary>
    private FfxiFileTable Build()
    {
        byte[] vtable = [0, 1, 3];

        // (directory << 7) | file
        ushort[] ftable = [0, (ushort)((5 << 7) | 42), (ushort)((9 << 7) | 7)];
        byte[] packed = new byte[ftable.Length * 2];
        for (int i = 0; i < ftable.Length; i++)
        {
            BitConverter.GetBytes(ftable[i]).CopyTo(packed, i * 2);
        }

        File.WriteAllBytes(Path.Combine(_root, "VTABLE.DAT"), vtable);
        File.WriteAllBytes(Path.Combine(_root, "FTABLE.DAT"), packed);
        return new FfxiFileTable(_root);
    }

    [Fact]
    public void RomOneIsTheFolderCalledRom()
    {
        string? path = Build().Path(1);

        Assert.NotNull(path);
        Assert.Equal(Path.Combine(_root, "ROM", "5", "42.DAT"), path);
    }

    [Fact]
    public void OtherRomsCarryTheirNumber()
    {
        string? path = Build().Path(2);

        Assert.NotNull(path);
        Assert.Equal(Path.Combine(_root, "ROM3", "9", "7.DAT"), path);
    }

    [Fact]
    public void AnIdWithNoRomIsNotInstalled()
    {
        Assert.Null(Build().Path(0));
    }

    [Fact]
    public void AnIdPastTheEndIsNotInstalled()
    {
        Assert.Null(Build().Path(9999));
        Assert.Null(Build().Path(-1));
    }

    /// <summary>
    /// A zone's map data is its zone id plus 100 - derived in
    /// tools/zonenames.py by checking which offset makes the chunk ids read as
    /// abbreviations of the zone names, not by assuming.
    /// </summary>
    [Fact]
    public void AZoneResolvesThroughTheHundredOffset()
    {
        FfxiFileTable table = Build();

        Assert.Equal(table.Path(101), table.ZonePath(1));
    }

    [Fact]
    public void MismatchedTablesAreRefused()
    {
        File.WriteAllBytes(Path.Combine(_root, "VTABLE.DAT"), [1, 1, 1]);
        File.WriteAllBytes(Path.Combine(_root, "FTABLE.DAT"), [0, 0]);

        Assert.Throws<InvalidDataException>(() => new FfxiFileTable(_root));
    }

    public void Dispose() => Directory.Delete(_root, recursive: true);
}
