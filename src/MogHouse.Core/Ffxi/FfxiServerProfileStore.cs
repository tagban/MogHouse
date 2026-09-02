using System.Text.Json;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// The saved list of FFXI server profiles - persisted as
/// ffxi-server-profiles.json right next to the running executable, not
/// %AppData% or the registry. Deliberate: the long-term goal is a portable
/// app (self-contained exe or a plain zip folder with its config alongside
/// it, plus a sibling "Playonline Assets" folder for game data) - nothing
/// should depend on a per-user profile directory. Same profile shape as
/// Invigoration's HotlineServerProfileStore (a sibling project by the same
/// author), reimplemented here rather than referenced since MogHouse has
/// no dependency on Invigoration.Core.
/// </summary>
public static class FfxiServerProfileStore
{
    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };
    private static readonly Lock SyncRoot = new();
    private static List<FfxiServerProfile>? _cache;
    private static string? _configDirectoryOverride;

    /// <summary>Test-only hook - redirects storage to an isolated directory so tests don't leak profiles into the user's real %AppData%.</summary>
    public static string? ConfigDirectoryOverride
    {
        get => _configDirectoryOverride;
        set
        {
            _configDirectoryOverride = value;
            _cache = null;
        }
    }

    private static string ConfigDirectory => ConfigDirectoryOverride ?? DefaultConfigDirectory();

    public static string FilePath => Path.Combine(ConfigDirectory, "ffxi-server-profiles.json");

    public static List<FfxiServerProfile> Profiles => _cache ??= LoadFromDisk();

    public static event Action? ProfilesChanged;

    /// <summary>
    /// Next to the running executable - AppContext.BaseDirectory, not %AppData%.
    /// Portable: works unpacked from a zip, no installer, no registry.
    ///
    /// Unless that directory cannot be written to, which is not a case a zip
    /// folder ever hits but a packaged one does: a Flatpak mounts /app
    /// read-only, and the same is true of a .app under /Applications installed
    /// for all users, or an install under Program Files. Writing beside the
    /// executable is the intent, so it is still tried first and still wins
    /// wherever it works; the per-user directory is only where the state goes
    /// when the alternative is losing it.
    /// </summary>
    public static string DefaultConfigDirectory() =>
        _resolvedConfigDirectory ??= ResolveConfigDirectory();

    private static string? _resolvedConfigDirectory;

    private static string ResolveConfigDirectory()
    {
        string beside = AppContext.BaseDirectory;

        // Inside a .app, never write beside the executable even when the
        // directory happens to be writable. A macOS bundle is code-signed as a
        // sealed unit, and adding a file to Contents/MacOS invalidates that
        // signature - `codesign --verify` then reports "a sealed resource is
        // missing or invalid" and names the file. That is not a theoretical
        // risk: writing a log there broke a freshly notarized build, and it
        // would do the same to any copy a player runs from a writable folder.
        // A bundle is an installed application rather than a portable folder,
        // so the per-user directory is the right home for its state anyway.
        if (!IsInsideAppBundle(beside) && IsWritable(beside))
        {
            return beside;
        }

        // LocalApplicationData is XDG_DATA_HOME on Linux (~/.local/share), which
        // a Flatpak redirects into its own sandbox, and %LocalAppData% on
        // Windows. Falling back to the temp directory is a last resort that at
        // least keeps the session working rather than throwing on startup.
        string data = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (data.Length == 0)
        {
            data = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".local", "share");
        }
        if (data.Length == 0)
        {
            return Path.GetTempPath();
        }

        string directory = Path.Combine(data, "MogHouse");
        try
        {
            Directory.CreateDirectory(directory);
        }
        catch (Exception)
        {
            return Path.GetTempPath();
        }
        return directory;
    }

    /// <summary>
    /// Whether this directory sits inside a macOS application bundle. The
    /// executable of a bundle lives in Contents/MacOS, which is the shape
    /// checked for rather than the ".app" suffix alone - a folder someone
    /// happened to name that way is not a bundle.
    /// </summary>
    private static bool IsInsideAppBundle(string directory)
    {
        if (!OperatingSystem.IsMacOS())
        {
            return false;
        }

        string trimmed = directory.TrimEnd(Path.DirectorySeparatorChar);
        return trimmed.EndsWith(Path.Combine(".app", "Contents", "MacOS"), StringComparison.Ordinal);
    }

    /// <summary>
    /// Whether a directory can actually be written to, established by writing
    /// rather than by reading permissions - the attributes can say yes on a
    /// read-only mount, a sandbox, or a folder owned by another user, and only
    /// the attempt is conclusive.
    /// </summary>
    private static bool IsWritable(string directory)
    {
        try
        {
            string probe = Path.Combine(directory, $".moghouse-write-{Environment.ProcessId}.tmp");
            using (FileStream stream = File.Create(probe, 1, FileOptions.DeleteOnClose))
            {
                return true;
            }
        }
        catch (Exception)
        {
            return false;
        }
    }

    public static FfxiServerProfile? Find(string id) =>
        string.IsNullOrEmpty(id) ? null : Profiles.FirstOrDefault(p => p.Id == id);

    public static FfxiServerProfile CreateAndSave(string name, string host)
    {
        var profile = new FfxiServerProfile
        {
            Name = string.IsNullOrWhiteSpace(name) ? "New Server" : name.Trim(),
            Host = host,
        };
        Profiles.Add(profile);
        Save();
        return profile;
    }

    public static void Delete(string id)
    {
        Profiles.RemoveAll(p => p.Id == id);
        Save();
    }

    public static void Save()
    {
        lock (SyncRoot)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(Profiles, JsonOptions));
            ProfilesChanged?.Invoke();
        }
    }

    private static List<FfxiServerProfile> LoadFromDisk()
    {
        if (!File.Exists(FilePath))
        {
            return [];
        }

        var loaded = JsonSerializer.Deserialize<List<FfxiServerProfile>>(File.ReadAllText(FilePath), JsonOptions);
        return loaded ?? [];
    }
}
