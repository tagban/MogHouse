using System;
using System.IO;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using MogHouse.Core.Ffxi;

namespace MogHouse.App.ViewModels;

/// <summary>
/// The shell. Owns the one <see cref="FfxiGameSession"/> everything else
/// shares, and swaps <see cref="CurrentPage"/> as the flow moves from login to
/// character select to the game view.
/// </summary>
public partial class MainViewModel : ViewModelBase
{
    [ObservableProperty]
    public partial ViewModelBase CurrentPage { get; set; }

    [ObservableProperty]
    public partial string Status { get; set; } = "Not connected.";

    public FfxiGameSession Session { get; }

    /// <summary>
    /// Who was chosen at character select. The roster knows a character's race
    /// and face; the server never sends us the entity update it sends for
    /// everyone else, so this is where our own appearance comes from.
    /// </summary>
    public FfxiCharacter? SelectedCharacter { get; set; }

    /// <summary>Where the game's files are, once found or chosen.</summary>
    public string? InstallPath { get; set; }

    /// <summary>
    /// The window's folder picker, for the one screen that needs one. Set by
    /// MainWindow, because a view model has no window of its own.
    /// </summary>
    public Avalonia.Platform.Storage.IStorageProvider? StorageProvider { get; set; }

    /// <summary>
    /// Null when the FFXI compression tables aren't installed. The transport
    /// still works without them but nothing can be decoded, so the UI says so
    /// up front rather than appearing to connect and then showing nothing.
    /// </summary>
    public FfxiHuffmanTables? Tables { get; }

    /// <summary>Where the zone navmeshes live, if the user has pointed us at them.</summary>
    public string? NavMeshDirectory { get; }

    /// <summary>The server's data/zones directory, which holds zone lines.</summary>
    public string? ZoneDataDirectory { get; }

    public MainViewModel()
    {
        // Found before anything is built from it. This used to sit further
        // down, after the session was constructed with
        // `InstallPath is null ? null : new FfxiFileTable(InstallPath)` - and
        // InstallPath was still null on that line, so the session never got a
        // file table at all and NPC dialogue had nothing to look line ids up
        // in. The initialiser read correctly and ran too early.
        InstallPath = FfxiInstall.Find();

        Tables = FfxiHuffmanTables.TryLoadDefault();
        // Navmeshes come from the same place as the compression tables by
        // default, since both are server-side data the project does not ship.
        NavMeshDirectory = ServerData("MOGHOUSE_FFXI_NAVMESHES", "navmeshes");
        ZoneDataDirectory = ServerData("MOGHOUSE_FFXI_ZONEDATA", "zones");

        Session = new FfxiGameSession(
            Tables is null ? null : new FfxiHuffman(Tables),
            NavMeshDirectory,
            ZoneDataDirectory,
            // The install the user pointed us at is also where NPC
            // dialogue lives; the server only sends line ids.
            InstallPath is null ? null : new FfxiFileTable(InstallPath));

        Session.Status += message =>
        {
            // To the log as well as the screen. These lines are the session
            // explaining itself - "Placed by the server at...", "Ignored a
            // placement for..." - and they were the one place that said why a
            // teleport did or did not happen, visible for a moment in a status
            // bar and then gone.
            Console.WriteLine($"session: {message}");
            Dispatcher.UIThread.Post(() => Status = message);
        };

        // Without the game's files there is nothing this can do, so finding
        // them comes before anything else rather than failing later with a
        // missing DAT.
        // The screen appears when the game was not found *or* when what we
        // found has never been confirmed by the person running this. A guess
        // is worth showing before it is relied on, and outside Windows it is
        // usually a guess: there is no registry key to read, and the game
        // normally lives inside a Wine or CrossOver prefix.
        if (InstallPath is null || FfxiInstall.Confirmed() is null)
        {
            CurrentPage = new InstallViewModel(this) { Detected = InstallPath };
        }
        else
        {
            Environment.SetEnvironmentVariable("MOGHOUSE_FFXI_INSTALL", InstallPath);
            CurrentPage = new LoginViewModel(this);
        }

        // Said out loud either way. This is the one thing whose absence stops
        // the client talking to any server at all, and a packaged build that
        // shipped without it would otherwise fail at the login screen with
        // nothing in the log to say why.
        if (Tables is null)
        {
            Status = "Compression tables not found - set MOGHOUSE_FFXI_RES to a directory containing " +
                     $"{FfxiHuffmanTables.EncodeFileName} and {FfxiHuffmanTables.DecodeFileName}.";
            Console.WriteLine($"startup: {Status}");
        }
        else
        {
            Console.WriteLine("startup: compression tables loaded.");
        }

        Console.WriteLine($"startup: game files at {InstallPath ?? "(not found - asking)"}");
        Console.WriteLine($"startup: navmeshes {(NavMeshDirectory ?? "(none - the flat map will be empty)")}");
        Console.WriteLine($"startup: zone data {(ZoneDataDirectory ?? "(none - no zone lines)")}");
    }

    /// <summary>
    /// Server-side data: what the environment says, or a folder beside the
    /// executable, or nothing.
    ///
    /// Both of these are optional and the client says what it loses without
    /// them, but a packaged build that ships a folder nobody looks in is worse
    /// than one that ships nothing - which is exactly what happened the first
    /// time: 836 files of zone data in the zip, and "no zone lines" in the log.
    /// </summary>
    private static string? ServerData(string variable, string folder)
    {
        if (Environment.GetEnvironmentVariable(variable) is { Length: > 0 } configured)
        {
            return configured;
        }

        foreach (string root in new[] { AppContext.BaseDirectory, Path.Combine(AppContext.BaseDirectory, "data") })
        {
            string beside = Path.Combine(root, folder);
            if (Directory.Exists(beside))
            {
                return beside;
            }
        }

        return null;
    }

    public void Navigate(ViewModelBase page) => CurrentPage = page;
}
