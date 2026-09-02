using MogHouse.Core.Ffxi;
using MogHouse.Core.Interop;

namespace MogHouse.Core.Screens;

/// <summary>
/// Asking who is signing in, and to what.
///
/// Drawn in the renderer's own window rather than in a separate toolkit's, so
/// there is one window for the whole client and the world can sit behind this
/// the way the retail launcher has always looked.
/// </summary>
public static class SignInScreen
{
    /// <summary>
    /// What the player filled in, or null if they closed the window or asked to
    /// quit - either way, a caller should stop rather than ask again.
    /// </summary>
    /// <param name="MakeAccount">
    /// Set when the player asked for an account rather than offering one. The
    /// server is still needed to ask, which is why this comes back through the
    /// same record instead of being a separate answer.
    /// </param>
    public sealed record Credentials(string Host, string Username, string Password,
                                     bool MakeAccount = false);

    private const string SignIn = "SIGN IN";
    private const string NewAccount = "NEW ACCOUNT";
    private const string Remember = "REMEMBER SERVER";
    private const string Quit = "QUIT";

    /// <summary>
    /// Shows the screen until it is filled in or abandoned.
    ///
    /// <paramref name="message"/> is what to say above the buttons - empty on
    /// the first showing, and the reason it failed on any showing after that,
    /// which is why a caller loops on this rather than calling it once.
    /// </summary>
    /// <param name="account">
    /// An account to start with, for the one that was just created - it should
    /// not have to be typed a second time. The saved profile fills the field in
    /// otherwise.
    /// </param>
    public static Credentials? Show(ScreenHost screens, string message = "", string? account = null)
    {
        // Whatever was used last, so a returning player presses one button. The
        // store keeps passwords obfuscated rather than encrypted, so it is
        // filled in only when it was deliberately saved before.
        FfxiServerProfile? saved = FfxiServerProfileStore.Profiles.FirstOrDefault();

        string host = saved?.Host ?? "";
        string username = account ?? saved?.Username ?? "";

        // Never the saved password beside a different account than it belongs
        // to - a freshly made one has no password here, and offering the old
        // one would be offering the wrong one.
        string password = account is null ? saved?.Password ?? "" : "";

        while (true)
        {
            NativeFormRow[] rows =
            [
                NativeFormRow.Field("SERVER", host),
                NativeFormRow.Field("ACCOUNT", username),
                NativeFormRow.Secret("PASSWORD", password),
                NativeFormRow.Button(SignIn),
                NativeFormRow.Button(NewAccount),
                NativeFormRow.Button(Remember),
                NativeFormRow.Button(Quit),
            ];

            NativeFormResult? result = screens.Ask("MOGHOUSE XI", message, rows);
            if (result is null)
            {
                return null;   // the window closed
            }

            // Whatever was typed, kept across a re-show. Read before the button
            // is looked at, so a failed sign-in comes back filled in rather
            // than blank.
            host = result[0].Trim();
            username = result[1].Trim();
            password = result[2];

            switch (rows[result.Button].Text)
            {
                case Quit:
                    return null;

                case Remember:
                    message = Save(host, username, password);
                    continue;

                case NewAccount when host.Length == 0:
                case SignIn when host.Length == 0:
                    message = "A SERVER IS NEEDED.";
                    continue;

                case NewAccount:
                    // Only the server is needed to ask; the account and its
                    // password are chosen on the screen that follows.
                    return new Credentials(host, username, password, MakeAccount: true);

                case SignIn when username.Length == 0 || password.Length == 0:
                    message = "AN ACCOUNT AND PASSWORD ARE NEEDED.";
                    continue;

                case SignIn:
                    return new Credentials(host, username, password);
            }
        }
    }

    /// <summary>
    /// Keeps the server for next time, and says what happened.
    ///
    /// Explicit rather than automatic, which is how the old screen worked too:
    /// the password is obfuscated at rest and that is not encryption, so
    /// writing one to disk should be something the player asked for.
    /// </summary>
    private static string Save(string host, string username, string password)
    {
        if (host.Length == 0)
        {
            return "A SERVER IS NEEDED BEFORE IT CAN BE REMEMBERED.";
        }

        FfxiServerProfile profile =
            FfxiServerProfileStore.Profiles.FirstOrDefault(p => p.Host == host)
            ?? FfxiServerProfileStore.CreateAndSave(host, host);

        profile.Host = host;
        profile.Username = username;
        profile.Password = password;
        FfxiServerProfileStore.Save();

        return "SERVER REMEMBERED.";
    }
}
