using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MogHouse.Core.Ffxi;

namespace MogHouse.App.ViewModels;

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

    /// <summary>
    /// Back to the login page, for the account someone did not mean to use.
    ///
    /// Nothing to tell the server: the zone session has not been opened yet at
    /// this point, and the lobby connection is not ours to keep alive.
    /// </summary>
    /// <summary>
    /// Off to make one. The lobby connection stays open behind this - creating
    /// a character happens on it.
    /// </summary>
    [RelayCommand]
    private void CreateCharacter() =>
        _shell.Navigate(new CreateCharacterViewModel(_shell, _sessionHash, _host));

    [RelayCommand]
    private void SignOut()
    {
        _shell.SelectedCharacter = null;
        _shell.Status = "Signed out.";
        _shell.Navigate(new LoginViewModel(_shell));
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
            _shell.SelectedCharacter = SelectedCharacter;
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
