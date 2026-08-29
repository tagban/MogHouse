using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using PortJeuno.Core.Ffxi;

namespace PortJeuno.App.ViewModels;

public partial class LoginViewModel : ViewModelBase
{
    private readonly MainViewModel _shell;

    [ObservableProperty]
    public partial string Host { get; set; } = "127.0.0.1";

    [ObservableProperty]
    public partial string Username { get; set; } = "";

    [ObservableProperty]
    public partial string Password { get; set; } = "";

    [ObservableProperty]
    public partial string? Error { get; set; }

    [ObservableProperty]
    public partial bool IsBusy { get; set; }

    public LoginViewModel(MainViewModel shell) => _shell = shell;

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
