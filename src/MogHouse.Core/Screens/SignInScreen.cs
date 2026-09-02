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
    private const string Saved = "LOAD";
    private const string NewAccount = "NEW ACCOUNT";
    private const string Remember = "SAVE PROFILE";
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
        // Every server that has been kept, not just the first. Copied once so
        // the list cannot change underneath the cycling below.
        List<FfxiServerProfile> profiles = [.. FfxiServerProfileStore.Profiles];
        int chosen = 0;

        FfxiServerProfile? saved = profiles.FirstOrDefault();

        string name = saved?.Name ?? "";
        string host = saved?.Host ?? "";
        string username = account ?? saved?.Username ?? "";

        // Never the saved password beside a different account than it belongs
        // to - a freshly made one has no password here, and offering the old
        // one would be offering the wrong one.
        string password = account is null ? saved?.Password ?? "" : "";

        while (true)
        {
            var rows = new List<NativeFormRow>
            {
                // What to call this sign-in. Kept first because it is what the
                // picker below shows, and a saved server is far easier to find
                // by the name its owner gave it than by a host and an account.
                NativeFormRow.Field("PROFILE", name),
                NativeFormRow.Field("SERVER", host),
                NativeFormRow.Field("ACCOUNT", username),
                NativeFormRow.Secret("PASSWORD", password),
            };

            // Sign in first, always. Return presses the first button that can
            // be pressed, so whatever leads is what typing a password and
            // pressing return does - and with the picker in front of it, that
            // was "step to the next saved server" rather than "sign in".
            rows.Add(NativeFormRow.Button(SignIn));

            // A button that names the server it would load and steps to the
            // next one, rather than a list taking up the screen. Only worth
            // showing when there is more than one to step between - with a
            // single saved server it is already filled in.
            if (profiles.Count > 1)
            {
                rows.Add(NativeFormRow.Button($"{Saved}: {profiles[chosen].Name.ToUpperInvariant()}"));
            }

            rows.AddRange([
                NativeFormRow.Button(NewAccount),
                NativeFormRow.Button(Remember),
                NativeFormRow.Button(Quit),
            ]);

            NativeFormResult? result = screens.Ask("MOGHOUSE XI", message, rows);
            if (result is null)
            {
                return null;   // the window closed
            }

            // Whatever was typed, kept across a re-show. Read before the button
            // is looked at, so a failed sign-in comes back filled in rather
            // than blank.
            name = result[0].Trim();
            host = result[1].Trim();
            username = result[2].Trim();
            password = result[3];

            // The picker names the server it holds, so it is matched by what it
            // starts with rather than by its whole label.
            string pressed = rows[result.Button].Text;
            if (pressed.StartsWith(Saved, StringComparison.Ordinal))
            {
                chosen = (chosen + 1) % profiles.Count;

                FfxiServerProfile next = profiles[chosen];
                name = next.Name;
                host = next.Host;
                username = next.Username;
                password = next.Password;
                message = "";
                continue;
            }

            switch (pressed)
            {
                case Quit:
                    return null;

                case Remember:
                    message = Save(name, host, username, password);
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
    /// <summary>
    /// What to call a saved sign-in when its owner did not say: the host, and
    /// the account with it, because a host alone does not tell two accounts on
    /// the same server apart.
    /// </summary>
    private static string Describe(string host, string username) =>
        username.Length > 0 ? $"{host} ({username})" : host;

    private static string Save(string name, string host, string username, string password)
    {
        if (host.Length == 0)
        {
            return "A SERVER IS NEEDED BEFORE THE PROFILE CAN BE SAVED.";
        }

        if (name.Length == 0)
        {
            name = Describe(host, username);
        }

        // Matched on the name its owner gave it. Two sign-ins can differ only
        // by which account they use - a main and an alt on one server - and
        // matching on the host alone had the second quietly replace the first.
        FfxiServerProfile profile =
            FfxiServerProfileStore.Profiles.FirstOrDefault(
                p => string.Equals(p.Name, name, StringComparison.OrdinalIgnoreCase))
            ?? FfxiServerProfileStore.CreateAndSave(name, host);

        profile.Name = name;
        profile.Host = host;
        profile.Username = username;
        profile.Password = password;
        FfxiServerProfileStore.Save();

        return $"SAVED AS {name.ToUpperInvariant()}.";
    }
}
