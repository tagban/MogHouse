using System.Text.Json;

namespace PortJeuno.Core.Ffxi;

/// <summary>
/// The saved list of FFXI server profiles - persisted as
/// ffxi-server-profiles.json right next to the running executable, not
/// %AppData% or the registry. Deliberate: the long-term goal is a portable
/// app (self-contained exe or a plain zip folder with its config alongside
/// it, plus a sibling "Playonline Assets" folder for game data) - nothing
/// should depend on a per-user profile directory. Same profile shape as
/// Invigoration's HotlineServerProfileStore (a sibling project by the same
/// author), reimplemented here rather than referenced since PortJeuno has
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

    /// <summary>Next to the running executable - AppContext.BaseDirectory, not %AppData%. Portable: works unpacked from a zip, no installer, no registry.</summary>
    public static string DefaultConfigDirectory() => AppContext.BaseDirectory;

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
