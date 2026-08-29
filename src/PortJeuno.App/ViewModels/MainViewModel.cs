using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using PortJeuno.Core.Ffxi;

namespace PortJeuno.App.ViewModels;

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

    public MainViewModel()
    {
        Tables = FfxiHuffmanTables.TryLoadDefault();
        Session = new FfxiGameSession(Tables is null ? null : new FfxiHuffman(Tables));

        Session.Status += message => Dispatcher.UIThread.Post(() => Status = message);

        CurrentPage = new LoginViewModel(this);

        if (Tables is null)
        {
            Status = "Compression tables not found - set PORTJEUNO_FFXI_RES to a directory containing " +
                     $"{FfxiHuffmanTables.EncodeFileName} and {FfxiHuffmanTables.DecodeFileName}.";
        }
    }

    public void Navigate(ViewModelBase page) => CurrentPage = page;
}
