using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Linq;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PortJeuno.Core.Ffxi;

namespace PortJeuno.App.ViewModels;

public partial class LoginViewModel : ViewModelBase
{
    private readonly MainViewModel _shell;

    /// <summary>
    /// Saved servers. These live in a JSON file next to the executable rather
    /// than in a user profile directory, so a build and its settings can be
    /// copied around as one folder - the portability the project is aiming for.
    /// Passwords are obfuscated at rest, which is not encryption and is not
    /// meant to be: it keeps them from sitting in plain sight in a text file.
    /// </summary>
    public ObservableCollection<FfxiServerProfile> Profiles { get; } = [];

    [ObservableProperty]
    public partial FfxiServerProfile? SelectedProfile { get; set; }

    [ObservableProperty]
    public partial string ProfileName { get; set; } = "";

    [ObservableProperty]
    public partial string Host { get; set; } = "127.0.0.1";

    [ObservableProperty]
    public partial string Username { get; set; } = "";

    [ObservableProperty]
    public partial string Password { get; set; } = "";

    [ObservableProperty]
    public partial int AuthPort { get; set; } = FfxiConstants.AuthPort;

    [ObservableProperty]
    public partial string? Error { get; set; }

    [ObservableProperty]
    public partial bool IsBusy { get; set; }

    public LoginViewModel(MainViewModel shell)
    {
        _shell = shell;
        ReloadProfiles();
        SelectedProfile = Profiles.FirstOrDefault();
    }

    private void ReloadProfiles()
    {
        Profiles.Clear();
        foreach (FfxiServerProfile profile in FfxiServerProfileStore.Profiles)
        {
            Profiles.Add(profile);
        }
    }

    /// <summary>Picking a saved server fills the form from it.</summary>
    partial void OnSelectedProfileChanged(FfxiServerProfile? value)
    {
        if (value is null)
        {
            return;
        }

        ProfileName = value.Name;
        Host = value.Host;
        Username = value.Username;
        Password = value.Password;
        AuthPort = value.AuthPort;
    }

    [RelayCommand]
    private void SaveProfile()
    {
        Error = null;

        if (Host.Length == 0)
        {
            Error = "A host is required to save a profile.";
            return;
        }

        string name = ProfileName.Length > 0 ? ProfileName : Host;

        // Update the selected profile in place when the name still matches,
        // so editing a password doesn't quietly leave a duplicate behind.
        FfxiServerProfile profile = SelectedProfile is not null && SelectedProfile.Name == name
            ? SelectedProfile
            : FfxiServerProfileStore.CreateAndSave(name, Host);

        profile.Name = name;
        profile.Host = Host;
        profile.Username = Username;
        profile.Password = Password;
        profile.AuthPort = AuthPort;

        FfxiServerProfileStore.Save();

        ReloadProfiles();
        SelectedProfile = Profiles.FirstOrDefault(p => p.Id == profile.Id);
        _shell.Status = $"Saved '{name}' to {FfxiServerProfileStore.FilePath}";
    }

    [RelayCommand]
    private void DeleteProfile()
    {
        if (SelectedProfile is null)
        {
            return;
        }

        string name = SelectedProfile.Name;
        FfxiServerProfileStore.Delete(SelectedProfile.Id);

        ReloadProfiles();
        SelectedProfile = Profiles.FirstOrDefault();
        _shell.Status = $"Deleted profile '{name}'.";
    }

    [RelayCommand]
    private async Task LoginAsync()
    {
        Error = null;
        IsBusy = true;

        try
        {
            var profile = new FfxiServerProfile
            {
                Host = Host,
                Username = Username,
                Password = Password,
                AuthPort = AuthPort,
            };

            (FfxiLoginResponse login, IReadOnlyList<FfxiCharacter> characters) =
                await _shell.Session.LoginAsync(profile);

            if (login.Result != FfxiLoginResult.Success)
            {
                Error = login.ErrorMessage ?? $"Login failed: {login.Result}";
                return;
            }

            _shell.Navigate(new CharacterSelectViewModel(_shell, characters, login.SessionHash!, Host));
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
