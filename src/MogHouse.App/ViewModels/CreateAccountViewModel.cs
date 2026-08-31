using System;
using System.Threading.Tasks;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using MogHouse.Core.Ffxi;

namespace MogHouse.App.ViewModels;

/// <summary>
/// Making an account on a server that allows it.
///
/// The server gates this on its own settings (login.lua's ACCOUNT_CREATION), so
/// a refusal here is a decision by whoever runs it rather than anything wrong
/// with what was typed.
///
/// Creating an account does not log you in - LOGIN_CREATE answers with a bare
/// success and no session - so this goes back to the login page with the name
/// filled in rather than pretending to continue.
/// </summary>
public partial class CreateAccountViewModel : ViewModelBase
{
    private readonly MainViewModel _shell;

    public CreateAccountViewModel(MainViewModel shell, string host, int authPort)
    {
        _shell = shell;
        Host = host;
        AuthPort = authPort;
    }

    [ObservableProperty]
    public partial string Host { get; set; } = "127.0.0.1";

    [ObservableProperty]
    public partial int AuthPort { get; set; } = FfxiConstants.AuthPort;

    [ObservableProperty]
    public partial string Username { get; set; } = "";

    [ObservableProperty]
    public partial string Password { get; set; } = "";

    [ObservableProperty]
    public partial string Confirm { get; set; } = "";

    [ObservableProperty]
    public partial string? Error { get; set; }

    [ObservableProperty]
    public partial bool IsBusy { get; set; }

    [RelayCommand]
    private void Back() => _shell.Navigate(new LoginViewModel(_shell));

    [RelayCommand]
    private async Task CreateAsync()
    {
        Error = null;

        // Checked here because the server cannot: it is only ever sent one
        // password, so a typo would become an account nobody can log into.
        if (Password != Confirm)
        {
            Error = "The passwords do not match.";
            return;
        }

        if (Username.Length == 0 || Password.Length == 0)
        {
            Error = "A username and a password are required.";
            return;
        }

        IsBusy = true;
        try
        {
            using var client = new FfxiAuthClient();
            await client.ConnectAsync(Host, AuthPort);
            FfxiLoginResponse response = await client.CreateAccountAsync(Username, Password);

            if (response.Result != FfxiLoginResult.SuccessCreate && response.Result != FfxiLoginResult.Success)
            {
                Error = response.ErrorMessage ?? $"The server refused: {response.Result}.";
                return;
            }

            _shell.Status = $"Account '{Username}' created. Log in with it.";

            // Back to the login page, carrying what was just made so it does
            // not have to be typed twice.
            var login = new LoginViewModel(_shell) { Host = Host, AuthPort = AuthPort, Username = Username };
            _shell.Navigate(login);
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
