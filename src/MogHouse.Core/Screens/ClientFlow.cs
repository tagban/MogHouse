using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Screens;

/// <summary>
/// The whole client, from first run to walking around.
///
/// <para>
/// This replaces a window per screen with one window for everything. The
/// renderer opens first and empty, the screens before the world are forms drawn
/// inside it, and the zone arrives underneath them when there is one - which is
/// what lets a scene sit behind the sign-in the way the retail launcher does,
/// and what makes the client one process with one window instead of a launcher
/// that hands off to a game.
/// </para>
///
/// <para>
/// Two rules shape the arrangement, and both were learned by breaking them:
/// </para>
///
/// <list type="bullet">
///   <item>
///     The renderer runs on the main thread. AppKit will not make an NSWindow
///     anywhere else, and SDL reports the refusal as "No available video
///     device", which reads like a driver fault and is not one.
///   </item>
///   <item>
///     Nothing here may be async on the way to that. After the first await a
///     console app's continuation resumes on a thread-pool thread, and every
///     line after it runs off the main thread - so the window creation fails
///     exactly as if it had been started on a worker in the first place.
///   </item>
/// </list>
/// </summary>
public static class ClientFlow
{
    /// <summary>
    /// Runs the client, returning a process exit code.
    ///
    /// <b>Call this from the main thread</b>, and from a Main that is not
    /// async - see the note on this class.
    /// </summary>
    public static int Run(TextWriter? log = null)
    {
        TextWriter say = log ?? Console.Out;

        // First run, before anything else: with no game files there are no
        // DATs, so no glyph atlas and nothing to draw a screen with. The
        // platform's own folder chooser is the only prompt that works at this
        // point, and like the window it has to be opened on this thread.
        if (FfxiInstall.EnsureChosen(say: line => say.WriteLine(line)) is null)
        {
            say.WriteLine("No Final Fantasy XI installation was chosen; there is nothing to run.");
            return 1;
        }

        LiveRadar world = LiveRadar.OpenEmpty(ownThread: false);

        // The screens are not the game: no radar, no chat log until there is a
        // world to have them about.
        world.ShowHud(false);

        // Everything that waits for a person or a server happens over here, so
        // the thread below is free to draw the screen they are waiting on.
        var session = new Thread(() => RunSession(world, say))
        {
            IsBackground = true,
            Name = "moghouse-session",
        };
        session.Start();

        return world.Run();
    }

    /// <summary>
    /// Signing in, choosing somebody, and then feeding the world - all of it
    /// blocking, on a thread that is not the one drawing.
    /// </summary>
    private static void RunSession(LiveRadar world, TextWriter say)
    {
        var screens = new ScreenHost(world);

        try
        {
            FfxiHuffmanTables? tables = FfxiHuffmanTables.TryLoadDefault();
            if (tables is null)
            {
                // Worth its own screen rather than a line in a log nobody is
                // looking at: without these every zone reply arrives, decrypts,
                // passes its checksum and then decodes to nothing, which looks
                // exactly like a server that never answered.
                screens.Tell("MISSING FILES",
                             "THE COMPRESSION TABLES WERE NOT FOUND. SET MOGHOUSE_FFXI_RES TO A " +
                             "FOLDER HOLDING COMPRESS.DAT AND DECOMPRESS.DAT.");
                return;
            }

            // Before anyone types a password, not after: without these a zone
            // cannot be decoded, and the failure lands in the renderer's log
            // while the player looks at a black world wondering what they did.
            if (LiveRadar.MissingZoneKeys() is { } missingKeys)
            {
                say.WriteLine(missingKeys);
                screens.Tell("MISSING FILES", missingKeys.ToUpperInvariant());
                return;
            }

            var game = new FfxiGameSession(new FfxiHuffman(tables));

            SignIn(screens, world, game, say);
        }
        catch (Exception error)
        {
            // A thread of our own means an exception here would otherwise take
            // the process down with a stack trace and no window, which tells a
            // player nothing at all.
            say.WriteLine($"the session ended badly: {error}");
            if (!screens.Closed)
            {
                screens.Tell("SOMETHING WENT WRONG", error.Message.ToUpperInvariant());
            }
        }
    }

    /// <summary>
    /// Loops the sign-in and character screens until the world is entered or
    /// the player leaves, so a wrong password or a server that is not up puts
    /// the screen back with the reason on it rather than ending the client.
    /// </summary>
    private static void SignIn(ScreenHost screens, LiveRadar world, FfxiGameSession game, TextWriter say)
    {
        string message = "";
        string? madeAccount = null;

        while (!screens.Closed)
        {
            SignInScreen.Credentials? credentials = SignInScreen.Show(screens, message, madeAccount);
            if (credentials is null)
            {
                return;
            }

            if (credentials.MakeAccount)
            {
                // Made, not signed in with: creation answers with no session,
                // so the account comes back and the sign-in screen is shown
                // again with it filled in.
                madeAccount = CreateAccountScreen.Show(
                    screens, credentials.Host, FfxiConstants.AuthPort, say);

                message = madeAccount is null
                    ? ""
                    : $"{madeAccount.ToUpperInvariant()} IS READY. SIGN IN WITH IT.";
                continue;
            }

            var profile = new FfxiServerProfile
            {
                Host = credentials.Host,
                Username = credentials.Username,
                Password = credentials.Password,
            };

            screens.Busy("SIGNING IN", $"CONNECTING TO {credentials.Host.ToUpperInvariant()}...");
            say.WriteLine($"connecting to {credentials.Host}...");

            FfxiLoginResponse login;
            IReadOnlyList<FfxiCharacter> characters;
            try
            {
                (login, characters) = game.LoginAsync(profile).GetAwaiter().GetResult();
            }
            catch (Exception error)
            {
                say.WriteLine($"could not reach {credentials.Host}: {error.Message}");
                message = $"COULD NOT REACH {credentials.Host.ToUpperInvariant()}.";
                continue;
            }

            if (login.Result != FfxiLoginResult.Success)
            {
                string why = login.ErrorMessage ?? login.Result.ToString();
                say.WriteLine($"login failed: {why}");
                message = why.ToUpperInvariant();
                continue;
            }

            say.WriteLine($"logged in - {characters.Count} slot(s)");

            if (ChooseCharacter(screens, world, game, profile, login, characters, say))
            {
                return;   // played, and the window has since closed
            }

            // Backed out of character select, so ask who they are again rather
            // than sitting on a screen with nothing on it.
            message = "";
        }
    }

    /// <summary>
    /// Character select, and creation when the account is empty or the player
    /// asks. Returns true once the world has been entered and left again, and
    /// false to go back to the sign-in screen.
    /// </summary>
    private static bool ChooseCharacter(ScreenHost screens, LiveRadar world, FfxiGameSession game,
                                        FfxiServerProfile profile, FfxiLoginResponse login,
                                        IReadOnlyList<FfxiCharacter> characters, TextWriter say)
    {
        string message = "";

        while (!screens.Closed)
        {
            CharacterScreens.Choice? choice = CharacterScreens.Select(screens, world, characters, message);
            if (choice is null)
            {
                return false;
            }

            if (choice.Chosen is { } character)
            {
                // A refusal here is usually temporary and always worth staying
                // signed in for - most often the server still holding a session
                // from a client that was closed less than a minute ago. Ending
                // the client over it would mean typing the password again to
                // retry something that fixes itself.
                string? refusedEntry = EnterWorld(world, screens, game, character,
                                                  profile.Host, login.SessionHash!, say);
                if (refusedEntry is null)
                {
                    return true;
                }

                message = refusedEntry;
                continue;
            }

            FfxiNewCharacter? wanted = CharacterScreens.Make(screens);
            if (wanted is null)
            {
                message = "";
                continue;
            }

            screens.Busy("CREATING", $"ASKING THE SERVER FOR {wanted.Name.ToUpperInvariant()}...");
            say.WriteLine($"creating {wanted.Name}...");

            string? refused = game.CreateCharacterAsync(wanted, login.SessionHash!)
                                  .GetAwaiter().GetResult();
            if (refused is not null)
            {
                say.WriteLine($"character creation refused: {refused}");
                message = refused.ToUpperInvariant();
                continue;
            }

            // The roster has to be read again for the new character's id, which
            // the creation exchange does not hand back.
            (login, characters) = game.LoginAsync(profile).GetAwaiter().GetResult();
            message = $"{wanted.Name.ToUpperInvariant()} IS READY.";
        }

        return false;
    }

    /// <summary>
    /// Zones in and then keeps the world and the session talking until the
    /// window closes.
    /// </summary>
    /// <returns>
    /// Null once the world has been entered and left again, or the reason it
    /// was refused - which the caller shows on character select so the player
    /// can simply try again.
    /// </returns>
    private static string? EnterWorld(LiveRadar world, ScreenHost screens, FfxiGameSession game,
                                      FfxiCharacter character, string host, byte[] sessionHash,
                                      TextWriter say)
    {
        screens.Busy("ENTERING THE WORLD", $"AS {character.Name.ToUpperInvariant()}...");
        say.WriteLine($"entering the world as {character.Name}...");

        try
        {
            game.ConnectToZoneAsync(character, sessionHash, host).GetAwaiter().GetResult();
            game.StartHeartbeatAsync().GetAwaiter().GetResult();
        }
        catch (FfxiLoginErrorException refused)
        {
            // The commonest of these by far is the server still holding a
            // session for a client closed moments ago, which clears itself
            // after about a minute - so it is worth saying that rather than
            // just repeating the server's own wording.
            say.WriteLine($"the server would not let {character.Name} in: {refused.Message}");
            return refused.Message.Contains("ALREADY_LOGGED_IN", StringComparison.OrdinalIgnoreCase)
                ? $"{character.Name.ToUpperInvariant()} IS STILL LOGGED IN. TRY AGAIN IN A MINUTE."
                : refused.Message.ToUpperInvariant();
        }

        FfxiZoneLoginReply? state = game.ZoneState;
        if (state is null)
        {
            return "THE SERVER LET US IN BUT SENT NO ZONE. THERE IS NOWHERE TO STAND.";
        }

        say.WriteLine($"in zone {state.ZoneNo} at {game.PosX:F1} {game.PosVertical:F1} {game.PosDepth:F1}");

        // The server sends entities, the tracker ages them, the renderer draws
        // their nameplates. Without this the world is right and empty of
        // everyone else. WorldLoop takes over feeding it once the zone is up.
        var tracker = new FfxiEntityTracker { SelfUniqueNo = state.UniqueNo };
        DateTimeOffset firstSeen = DateTimeOffset.UtcNow;
        foreach (FfxiEntityUpdate update in game.KnownEntities())
        {
            tracker.Observe(update, firstSeen);
        }

        // Who the player turned out to be. None of this was known when the
        // window opened, which is the whole reason it can be said now.
        world.SetPlayer(character.Name, FfxiAppearance.LookString(character.Race, character.Face));
        world.SetClock(state.GameTime);

        string zoneName = FfxiZoneNames.Label(state.ZoneNo) ?? $"ZONE {state.ZoneNo}";
        if (!world.LoadZone((int)state.ZoneNo, zoneName, game.PosX, game.PosVertical, game.PosDepth,
                            state.Direction))
        {
            return $"{zoneName.ToUpperInvariant()} IS NOT IN THIS INSTALLATION.";
        }

        // The screen comes down last, so there is never a frame of empty
        // window between the sign-in and the world.
        screens.Clear();

        // And now there is somewhere to be, so the radar and the chat log mean
        // something again.
        world.ShowHud(true);

        // Everything that happens while somebody is in the world - what they
        // typed, what they jumped over, what killed them, which zone they
        // walked into - lives in WorldLoop rather than here, and used to live
        // in a view model that also drew things.
        new WorldLoop(game, world, tracker, state.ZoneNo, character.Name, say).Run();

        say.WriteLine("the window closed; leaving the world");
        return null;
    }
}
