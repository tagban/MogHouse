namespace MogHouse.Core.Ffxi;

/// <summary>
/// Resolves FFXI file ids to paths, the way the client does.
///
/// The retail install indexes its content by a flat file id rather than by
/// path. Two tables at the install root map one to the other, with one entry
/// per id in each:
///
///     VTABLE.DAT   u8   the ROM number holding it, 0 if it is not installed
///     FTABLE.DAT   u16  (directory &lt;&lt; 7) | file
///
/// The renderer has its own copy of this in C++. Duplicated rather than
/// exposed across the interop boundary on purpose: it is twenty lines either
/// side, and widening the C ABI to carry a file system is a worse trade than
/// writing the lookup twice.
/// </summary>
public sealed class FfxiFileTable
{
    private readonly string _root;
    private readonly byte[] _vtable;
    private readonly byte[] _ftable;

    /// <summary>A zone's map data sits at its zone id plus this.</summary>
    public const int ZoneFileIdOffset = 100;

    public FfxiFileTable(string installRoot)
    {
        _root = installRoot;
        // Qualified: this class has its own Path method, which shadows the one
        // in System.IO inside the class body.
        _vtable = File.ReadAllBytes(System.IO.Path.Combine(installRoot, "VTABLE.DAT"));
        _ftable = File.ReadAllBytes(System.IO.Path.Combine(installRoot, "FTABLE.DAT"));

        if (_ftable.Length != _vtable.Length * 2)
        {
            throw new InvalidDataException("VTABLE and FTABLE disagree on how many ids there are");
        }
    }

    public int Count => _vtable.Length;

    /// <summary>The path for a file id, or null if that id is not installed.</summary>
    public string? Path(int fileId)
    {
        if (fileId < 0 || fileId >= _vtable.Length)
        {
            return null;
        }

        byte rom = _vtable[fileId];
        if (rom == 0)
        {
            return null;
        }

        ushort packed = BitConverter.ToUInt16(_ftable, fileId * 2);

        // ROM 1 lives in a folder called plain "ROM"; the rest carry a number.
        string folder = rom == 1 ? "ROM" : $"ROM{rom}";
        return System.IO.Path.Combine(_root, folder, (packed >> 7).ToString(), $"{packed & 0x7F}.DAT");
    }

    /// <summary>The map data for a zone, or null if it is not installed.</summary>
    public string? ZonePath(int zoneId) => Path(zoneId + ZoneFileIdOffset);

    /// <summary>
    /// Where the retail client is installed, from MOGHOUSE_FFXI_INSTALL or the
    /// usual Windows location.
    /// </summary>
    public static string DefaultInstallRoot() =>
        Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_INSTALL") is { Length: > 0 } configured
            ? configured
            : @"C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI";
}
