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

    /// <summary>
    /// Writes an expansion's pair under ROM{rom}, the same three ids wide.
    /// </summary>
    private void Expansion(int rom, byte[] vtable, ushort[] ftable)
    {
        string folder = Path.Combine(_root, $"ROM{rom}");
        Directory.CreateDirectory(folder);
        byte[] packed = new byte[ftable.Length * 2];
        for (int i = 0; i < ftable.Length; i++)
        {
            BitConverter.GetBytes(ftable[i]).CopyTo(packed, i * 2);
        }
        File.WriteAllBytes(Path.Combine(folder, $"VTABLE{rom}.DAT"), vtable);
        File.WriteAllBytes(Path.Combine(folder, $"FTABLE{rom}.DAT"), packed);
    }

    /// <summary>
    /// The base tables say nothing about id 0; ROM2's says it is there. This
    /// is every expansion zone: Yhoator Jungle's map is file 223, and the
    /// base VTABLE has a zero for it.
    /// </summary>
    [Fact]
    public void AnExpansionSuppliesWhatTheBaseTableLacks()
    {
        Expansion(2, [2, 0, 0], [(ushort)((3 << 7) | 11), 0, 0]);

        string? path = Build().Path(0);

        Assert.Equal(Path.Combine(_root, "ROM2", "3", "11.DAT"), path);
    }

    [Fact]
    public void ALaterRomOverridesAnEarlierOne()
    {
        Expansion(2, [2, 2, 0], [(ushort)((3 << 7) | 11), (ushort)((1 << 7) | 1), 0]);
        Expansion(4, [0, 4, 0], [0, (ushort)((2 << 7) | 2), 0]);

        FfxiFileTable table = Build();

        Assert.Equal(Path.Combine(_root, "ROM4", "2", "2.DAT"), table.Path(1));
        Assert.Equal(Path.Combine(_root, "ROM2", "3", "11.DAT"), table.Path(0));
    }

    [Fact]
    public void AnExpansionWithTheWrongShapeIsIgnored()
    {
        Expansion(2, [2, 2], [(ushort)((3 << 7) | 11), (ushort)((1 << 7) | 1)]);

        FfxiFileTable table = Build();

        Assert.Null(table.Path(0));
        Assert.Equal(Path.Combine(_root, "ROM", "5", "42.DAT"), table.Path(1));
    }

    public void Dispose() => Directory.Delete(_root, recursive: true);
}
