using System;
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
        Tables = FfxiHuffmanTables.TryLoadDefault();
        // Navmeshes come from the same place as the compression tables by
        // default, since both are server-side data the project does not ship.
        NavMeshDirectory = Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_NAVMESHES");
        ZoneDataDirectory = Environment.GetEnvironmentVariable("MOGHOUSE_FFXI_ZONEDATA");

        Session = new FfxiGameSession(
            Tables is null ? null : new FfxiHuffman(Tables),
            NavMeshDirectory,
            ZoneDataDirectory);

        Session.Status += message => Dispatcher.UIThread.Post(() => Status = message);

        // Without the game's files there is nothing this can do, so finding
        // them comes before anything else rather than failing later with a
        // missing DAT.
        InstallPath = FfxiInstall.Find();
        if (InstallPath is null)
        {
            CurrentPage = new InstallViewModel(this);
        }
        else
        {
            Environment.SetEnvironmentVariable("MOGHOUSE_FFXI_INSTALL", InstallPath);
            CurrentPage = new LoginViewModel(this);
        }

        if (Tables is null)
        {
            Status = "Compression tables not found - set MOGHOUSE_FFXI_RES to a directory containing " +
                     $"{FfxiHuffmanTables.EncodeFileName} and {FfxiHuffmanTables.DecodeFileName}.";
        }
    }

    public void Navigate(ViewModelBase page) => CurrentPage = page;
}
