using System.Runtime.InteropServices;
using System.Text.Json;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// Where the game's files are.
///
/// Not everyone ran an installer. A folder copied off another machine is just
/// as valid, and on anything but Windows it is the only way there is, so this
/// looks in the places such a folder tends to sit and remembers what it is
/// told.
/// </summary>
public static class FfxiInstall
{
    /// <summary>Where a remembered choice is kept, beside the server profiles.</summary>
    public static string SettingsPath =>
        Path.Combine(Path.GetDirectoryName(FfxiServerProfileStore.FilePath)!, "ffxi-install.json");

    private sealed record Remembered(string Path);

    /// <summary>
    /// Whether this folder is the game: both index files and the data they
    /// index. A folder that merely exists is not enough - accept one and every
    /// later failure reads as a missing DAT rather than as the wrong folder.
    /// </summary>
    public static bool IsInstall(string? path) =>
        !string.IsNullOrWhiteSpace(path) &&
        File.Exists(Path.Combine(path, "FTABLE.DAT")) &&
        File.Exists(Path.Combine(path, "VTABLE.DAT")) &&
        Directory.Exists(Path.Combine(path, "ROM"));

    /// <summary>
    /// Makes sense of whatever folder someone picked.
    ///
    /// People point at what they can see: the ROM folder, or the numbered
    /// folder inside it that actually holds the DATs. All of those are inside
    /// the install rather than being it, so walk up until the real root turns
    /// up. A few levels is plenty - ROM/1/ is two deep - and stopping keeps
    /// this from wandering off to the drive root.
    /// </summary>
    public static string? ResolveChosen(string? chosen)
    {
        if (string.IsNullOrWhiteSpace(chosen))
        {
            return null;
        }

        var here = new DirectoryInfo(chosen);
        for (int up = 0; up < 5 && here is not null; up++, here = here.Parent)
        {
            if (IsInstall(here.FullName))
            {
                return here.FullName;
            }
        }

        // And one level down, for someone who picked the folder the install
        // sits in rather than the install.
        try
        {
            foreach (DirectoryInfo child in new DirectoryInfo(chosen).EnumerateDirectories())
            {
                if (IsInstall(child.FullName))
                {
                    return child.FullName;
                }
            }
        }
        catch (Exception)
        {
            // An unreadable folder is a "no", not a crash.
        }

        return null;
    }

    /// <summary>The install to use, or null when nothing has been found or chosen.</summary>
    public static string? Find()
    {
        if (Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_INSTALL") is { Length: > 0 } fromEnv &&
            IsInstall(fromEnv))
        {
            return fromEnv;
        }

        if (LoadRemembered() is { } remembered && IsInstall(remembered))
        {
            return remembered;
        }

        if (OperatingSystem.IsWindows() && FromRegistry() is { } recorded && IsInstall(recorded))
        {
            return recorded;
        }

        foreach (string candidate in Likely())
        {
            if (IsInstall(candidate))
            {
                return candidate;
            }
        }

        return null;
    }

    /// <summary>
    /// Remembers a choice, and tells this process about it - the renderer
    /// reads the environment, and it is in the same process as the launcher.
    /// </summary>
    public static void Remember(string path)
    {
        MogHouse.Core.Interop.NativeEnvironment.Set("MOGHOUSE_FFXI_INSTALL", path);

        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(SettingsPath)!);
            File.WriteAllText(SettingsPath, JsonSerializer.Serialize(new Remembered(path)));
        }
        catch (Exception)
        {
            // Not being able to remember is worth carrying on without.
        }
    }

    /// <summary>
    /// The install the player has confirmed before, if any.
    ///
    /// Distinct from <see cref="Find"/>, which will happily guess. This is
    /// what decides whether the first-run screen appears: a guess should be
    /// shown to someone before it is relied on, and on macOS and Linux the
    /// guess is far more likely to be wrong, because the game usually lives
    /// inside a Wine or CrossOver prefix rather than anywhere predictable.
    /// </summary>
    public static string? Confirmed()
    {
        // Someone who set this said where the game is as plainly as someone who
        // picked it from a chooser, so it counts as confirmed and the first-run
        // screen stays away. It also gives a script a way past a prompt that
        // waits for a person - which is what a headless run would otherwise
        // hang on forever.
        if (Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_INSTALL") is { Length: > 0 } fromEnv &&
            IsInstall(fromEnv))
        {
            return fromEnv;
        }

        string? remembered = LoadRemembered();
        return remembered is not null && IsInstall(remembered) ? remembered : null;
    }

    /// <summary>
    /// The install to use, asking the player for it if this is a first run.
    ///
    /// <para>
    /// The whole of the first-run check: if a location has been confirmed
    /// before, it is returned and nothing is shown. Otherwise the platform's
    /// own folder chooser opens, and what comes back is resolved, checked and
    /// remembered - so this happens once per machine rather than once per
    /// launch.
    /// </para>
    ///
    /// <para>
    /// This has to run before anything else the client draws. With no install
    /// there are no DATs, so no glyph atlas and nothing to render a screen
    /// with; a native chooser is the only prompt that works at that point.
    /// It therefore <b>must be called on the main thread</b>, like anything
    /// else that opens a window on macOS.
    /// </para>
    ///
    /// <para>
    /// Returns null if the player cancels, or if what they picked was not an
    /// installation after <paramref name="attempts"/> tries. Picking the ROM
    /// folder, or a numbered folder inside it, is fine - see
    /// <see cref="ResolveChosen"/>.
    /// </para>
    /// </summary>
    public static string? EnsureChosen(int attempts = 3, Action<string>? say = null)
    {
        if (Confirmed() is { } already)
        {
            return already;
        }

        for (int attempt = 0; attempt < attempts; attempt++)
        {
            say?.Invoke("Where is Final Fantasy XI? Pick the folder holding FTABLE.DAT and ROM.");

            // Opened at the guess when there is one. Confirmed() deliberately
            // refuses to act on a guess without being shown it, which is right;
            // starting the chooser somewhere else as well only makes the person
            // go and find the folder themselves. On Windows that usually means
            // the dialog is already on the install and this is one click.
            string? picked = MogHouse.Core.Interop.NativeViewer.PickFolder(Find());
            if (picked is null)
            {
                // Cancelled, or no chooser available. Either way there is
                // nothing to fall back on, so say so rather than looping at
                // someone who has already declined.
                say?.Invoke("No folder chosen.");
                return null;
            }

            if (ResolveChosen(picked) is { } root)
            {
                Remember(root);
                say?.Invoke($"Using the installation at {root}");
                return root;
            }

            say?.Invoke($"{picked} does not look like a Final Fantasy XI installation - " +
                        "it should contain FTABLE.DAT, VTABLE.DAT and a ROM folder.");
        }

        return null;
    }

    private static string? LoadRemembered()
    {
        try
        {
            return File.Exists(SettingsPath)
                ? JsonSerializer.Deserialize<Remembered>(File.ReadAllText(SettingsPath))?.Path
                : null;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static IEnumerable<string> Likely()
    {
        yield return @"C:\Program Files (x86)\PlayOnline\SquareEnix\FINAL FANTASY XI";
        yield return @"C:\Program Files\PlayOnline\SquareEnix\FINAL FANTASY XI";

        string home = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        if (home.Length == 0)
        {
            yield break;
        }

        yield return Path.Combine(home, "FINAL FANTASY XI");
        yield return Path.Combine(home, "Games", "FINAL FANTASY XI");
        yield return Path.Combine(home, "Library", "Application Support", "FINAL FANTASY XI");

        // Wine and CrossOver, which is how this runs anywhere but Windows.
        yield return Path.Combine(home, ".wine", "drive_c", "Program Files (x86)", "PlayOnline", "SquareEnix", "FINAL FANTASY XI");
        yield return Path.Combine(home, "Library", "Application Support", "CrossOver", "Bottles", "FFXI",
                                  "drive_c", "Program Files (x86)", "PlayOnline", "SquareEnix", "FINAL FANTASY XI");

        // A prefix the user has already pointed Wine at.
        if (Environment.GetEnvironmentVariable("WINEPREFIX") is { Length: > 0 } prefix)
        {
            foreach (string candidate in InsidePrefix(prefix))
            {
                yield return candidate;
            }
        }

        // Bottles are named by whoever made them, so the one above only helps
        // someone who called theirs "FFXI". Walk whatever is actually there.
        // Whisky is the wrapper most Mac players use now; CrossOver is the paid
        // one. Neither is enumerated if it is not installed, and a prefix that
        // does not hold the game just fails IsInstall like any other candidate.
        foreach (string bottles in new[]
                 {
                     Path.Combine(home, "Library", "Application Support", "CrossOver", "Bottles"),
                     Path.Combine(home, "Library", "Containers", "com.isaacmarovitz.Whisky", "Bottles"),
                     Path.Combine(home, "Library", "Application Support", "Whisky", "Bottles"),
                 })
        {
            foreach (string bottle in Subdirectories(bottles))
            {
                foreach (string candidate in InsidePrefix(bottle))
                {
                    yield return candidate;
                }
            }
        }
    }

    /// <summary>
    /// The two places the game sits inside a Wine prefix. 64-bit prefixes keep
    /// a 32-bit installer's output under "Program Files (x86)"; a 32-bit prefix
    /// has only "Program Files".
    /// </summary>
    private static IEnumerable<string> InsidePrefix(string prefix)
    {
        yield return Path.Combine(prefix, "drive_c", "Program Files (x86)", "PlayOnline", "SquareEnix", "FINAL FANTASY XI");
        yield return Path.Combine(prefix, "drive_c", "Program Files", "PlayOnline", "SquareEnix", "FINAL FANTASY XI");
    }

    /// <summary>
    /// Subdirectories of a path that may not exist, or may not be readable.
    /// Enumeration happens here rather than in the caller because an iterator
    /// cannot catch around a yield.
    /// </summary>
    private static string[] Subdirectories(string path)
    {
        try
        {
            return Directory.Exists(path) ? Directory.GetDirectories(path) : [];
        }
        catch (Exception)
        {
            // An unreadable bottles folder is not worth failing detection over;
            // the picker is the fallback for everything this does not find.
            return [];
        }
    }

    [System.Runtime.Versioning.SupportedOSPlatform("windows")]
    private static string? FromRegistry()
    {
        foreach (string key in new[]
                 {
                     @"SOFTWARE\WOW6432Node\PlayOnlineUS\InstallFolder",
                     @"SOFTWARE\WOW6432Node\PlayOnline\InstallFolder",
                     @"SOFTWARE\PlayOnlineUS\InstallFolder",
                     @"SOFTWARE\PlayOnline\InstallFolder",
                 })
        {
            try
            {
                using Microsoft.Win32.RegistryKey? handle = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(key);
                // The values are numbered rather than named: 0001 is the game.
                if (handle?.GetValue("0001") is string path && path.Length > 0)
                {
                    return path;
                }
            }
            catch (Exception)
            {
                // A key we cannot read is a "no".
            }
        }

        return null;
    }
}
