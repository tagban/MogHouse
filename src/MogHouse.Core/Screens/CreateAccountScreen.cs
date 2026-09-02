using MogHouse.Core.Ffxi;
using MogHouse.Core.Interop;

namespace MogHouse.Core.Screens;

/// <summary>
/// Making an account on a server that allows it.
///
/// Servers decide for themselves whether to accept these - LandSandBoat gates
/// it behind ACCOUNT_CREATION in settings/login.lua - so a refusal here is a
/// normal answer rather than a fault, and it is shown as one.
/// </summary>
public static class CreateAccountScreen
{
    private const string Create = "CREATE";
    private const string Back = "BACK";

    /// <summary>
    /// Shows the screen until an account is made or the player goes back.
    ///
    /// Returns the name of the account that was created, so the sign-in screen
    /// can be filled in with it rather than asking for it a second time.
    /// Null means they backed out or the window closed.
    /// </summary>
    public static string? Show(ScreenHost screens, string host, int authPort, TextWriter say)
    {
        string username = "";
        string message = "";

        while (!screens.Closed)
        {
            NativeFormRow[] rows =
            [
                NativeFormRow.Field("ACCOUNT", username),
                NativeFormRow.Secret("PASSWORD"),
                NativeFormRow.Secret("PASSWORD AGAIN"),
                NativeFormRow.Button(Create),
                NativeFormRow.Button(Back),
            ];

            NativeFormResult? result = screens.Ask(
                "CREATE AN ACCOUNT", message.Length > 0 ? message : $"ON {host.ToUpperInvariant()}", rows);

            if (result is null || rows[result.Button].Text == Back)
            {
                return null;
            }

            username = result[0].Trim();
            string password = result[1];
            string again = result[2];

            if (username.Length == 0 || password.Length == 0)
            {
                message = "AN ACCOUNT AND PASSWORD ARE NEEDED.";
                continue;
            }

            if (password != again)
            {
                message = "THE PASSWORDS DO NOT MATCH.";
                continue;
            }

            screens.Busy("CREATING", $"ASKING {host.ToUpperInvariant()} FOR {username.ToUpperInvariant()}...");

            try
            {
                using var client = new FfxiAuthClient();
                client.ConnectAsync(host, authPort).GetAwaiter().GetResult();

                FfxiLoginResponse response =
                    client.CreateAccountAsync(username, password).GetAwaiter().GetResult();

                // A creation answers SuccessCreate rather than Success, and
                // carries no session with it - the account exists, and signing
                // in with it is a separate exchange.
                if (response.Result is not (FfxiLoginResult.SuccessCreate or FfxiLoginResult.Success))
                {
                    string why = response.ErrorMessage ?? $"THE SERVER REFUSED: {response.Result}";
                    say.WriteLine($"account creation refused: {why}");
                    message = why.ToUpperInvariant();
                    continue;
                }

                say.WriteLine($"created the account {username}");
                return username;
            }
            catch (Exception error)
            {
                say.WriteLine($"could not create an account on {host}: {error.Message}");
                message = error.Message.ToUpperInvariant();
            }
        }

        return null;
    }
}
