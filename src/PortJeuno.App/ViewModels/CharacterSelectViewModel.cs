using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PortJeuno.Core.Ffxi;

namespace PortJeuno.App.ViewModels;

public partial class CharacterSelectViewModel : ViewModelBase
{
    private readonly MainViewModel _shell;
    private readonly byte[] _sessionHash;
    private readonly string _host;

    public ObservableCollection<FfxiCharacter> Characters { get; }

    [ObservableProperty]
    public partial FfxiCharacter? SelectedCharacter { get; set; }

    [ObservableProperty]
    public partial string? Error { get; set; }

    [ObservableProperty]
    public partial bool IsBusy { get; set; }

    public CharacterSelectViewModel(MainViewModel shell, IReadOnlyList<FfxiCharacter> characters, byte[] sessionHash, string host)
    {
        _shell = shell;
        _sessionHash = sessionHash;
        _host = host;

        // The server pads the roster out to the account's slot limit; empty
        // slots come back with a single space for a name rather than an empty
        // string, so filtering on whitespace is what actually works.
        Characters = new ObservableCollection<FfxiCharacter>(
            characters.Where(c => !string.IsNullOrWhiteSpace(c.Name)));

        SelectedCharacter = Characters.FirstOrDefault();
    }

    [RelayCommand]
    private async Task EnterWorldAsync()
    {
        if (SelectedCharacter is null)
        {
            return;
        }

        Error = null;
        IsBusy = true;

        try
        {
            await _shell.Session.ConnectToZoneAsync(SelectedCharacter, _sessionHash, _host);
            await _shell.Session.StartHeartbeatAsync();
            _shell.Navigate(new GameViewModel(_shell));
        }
        catch (Exception ex)
        {
            Error = ex.Message;
        }
        finally
        {
            IsBusy = false;
        }
    }
}
