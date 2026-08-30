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

        CurrentPage = new LoginViewModel(this);

        if (Tables is null)
        {
            Status = "Compression tables not found - set MOGHOUSE_FFXI_RES to a directory containing " +
                     $"{FfxiHuffmanTables.EncodeFileName} and {FfxiHuffmanTables.DecodeFileName}.";
        }
    }

    public void Navigate(ViewModelBase page) => CurrentPage = page;
}
