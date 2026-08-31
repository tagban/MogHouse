using System;
using System.Linq;
using System.Threading.Tasks;
using Avalonia.Platform.Storage;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MogHouse.Core.Ffxi;

namespace MogHouse.App.ViewModels;

/// <summary>
/// The first thing anyone sees who does not have the game where we looked.
///
/// This client reads the retail files directly and ships none of them, so
/// without them it can do nothing at all - which makes this the one screen that
/// has to come before everything else, rather than a setting buried somewhere.
/// </summary>
public partial class InstallViewModel : ViewModelBase
{
    private readonly MainViewModel _shell;

    public InstallViewModel(MainViewModel shell)
    {
        _shell = shell;
    }

    [ObservableProperty]
    public partial string? Error { get; set; }

    [ObservableProperty]
    public partial string? Chosen { get; set; }

    [RelayCommand]
    private async Task BrowseAsync()
    {
        Error = null;

        IStorageProvider? storage = _shell.StorageProvider;
        if (storage is null)
        {
            Error = "Could not open a folder picker on this system.";
            return;
        }

        var picked = await storage.OpenFolderPickerAsync(new FolderPickerOpenOptions
        {
            Title = "Where is Final Fantasy XI?",
            AllowMultiple = false,
        });

        if (picked.Count == 0)
        {
            return;
        }

        string path = picked[0].Path.LocalPath;

        // Someone pointing at ROM, or at the numbered folder inside it, has
        // pointed inside the install rather than at it. Work out what they
        // meant rather than telling them they are wrong.
        string? resolved = FfxiInstall.ResolveChosen(path);
        if (resolved is null)
        {
            Error = $"That folder does not have FTABLE.DAT, VTABLE.DAT and ROM in it, and neither does anything around it.\n\nPick the folder called FINAL FANTASY XI.";
            Chosen = path;
            return;
        }

        FfxiInstall.Remember(resolved);
        Chosen = resolved;
        _shell.InstallPath = resolved;
        _shell.Status = $"Using the game files at {resolved}.";
        _shell.Navigate(new LoginViewModel(_shell));
    }
}
