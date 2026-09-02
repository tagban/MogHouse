// Log in, zone in, and draw the world - with no Avalonia, and the renderer on
// the main thread.
//
// This is the shape the client is moving to, small enough to read in one go. It
// exists because the client cannot do this at all on macOS today: LiveRadar
// starts the render loop on a thread of its own, the loop creates the window,
// and AppKit refuses to make an NSWindow anywhere but the main thread. The
// symptom is a successful login followed by a black window and a renderer log
// saying "SDL_Init failed: No available video device".
//
// Two things here are easy to get wrong, and both were got wrong first:
//
//   * The session needs the Huffman codec. Without it every zone reply arrives,
//     decrypts, passes its checksum - and then decodes to nothing, because
//     Decode() skips decompression when there is no codec. It surfaces as
//     "Zone server did not answer, or its reply could not be decoded", which is
//     also exactly what a silent network looks like.
//
//   * Main must not be async. After the first await, a console app's
//     continuation resumes on a thread-pool thread; there is no synchronisation
//     context to come back to. Every line after that runs off the main thread,
//     so creating the window fails the same way it does inside the client.
//
// Credentials come from the environment and are never stored:
//
//   MOGHOUSE_TEST_HOST=your.server
//   MOGHOUSE_TEST_USER=name
//   MOGHOUSE_TEST_PASS=secret
//   MOGHOUSE_FFXI_RES=<dir holding compress.dat and decompress.dat>
//   dotnet run --project tools/loginzone
//
// Prefix the command with a space if your shell keeps history.

using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using MogHouse.Core.Ffxi;

internal static class Program
{
    /// <summary>What the login produced, carried back to the main thread.</summary>
    private sealed record LoggedIn(
        FfxiGameSession Session, string CharacterName, uint Zone, float X, float Vertical, float Depth);

    private static int Main()
    {
        Console.WriteLine($"main thread id={Environment.CurrentManagedThreadId}");

        // Driven to completion here rather than awaited, so this thread - the
        // main one - is the thread that goes on to create the window.
        LoggedIn? ready = SignInAsync().GetAwaiter().GetResult();
        if (ready is null)
        {
            return 1;
        }

        Console.WriteLine($"opening the world on thread {Environment.CurrentManagedThreadId}");

        // ownThread: false is the whole difference. The loop is not started for
        // us; it is started below, on this thread.
        LiveRadar? world = LiveRadar.Open(
            (int)ready.Zone, ready.X, ready.Vertical, ready.Depth,
            serverClock: 0, playerName: ready.CharacterName, playerLook: null, ownThread: false);

        if (world is null)
        {
            Console.WriteLine("could not open the world window - see the message above");
            return 1;
        }

        // Feeding the session from a background thread while the main one draws,
        // which is the arrangement the client will use. Position only: this
        // shows the handoff, it does not replace GameViewModel.
        FfxiGameSession session = ready.Session;
        _ = Task.Run(async () =>
        {
            while (!world.Closed)
            {
                if (world.IsLoading != true &&
                    world.Position() is (float x, float vertical, float depth, sbyte facing))
                {
                    session.PlaceAt(x, vertical, depth, facing);
                }
                await Task.Delay(100);
            }
        });

        Console.WriteLine("handing the main thread to the renderer; close the window to quit");
        int rc = world.Run();
        Console.WriteLine($"window closed, rc={rc}");
        return rc;
    }

    private static async Task<LoggedIn?> SignInAsync()
    {
        string host = Environment.GetEnvironmentVariable("MOGHOUSE_TEST_HOST") ?? "";
        string user = Environment.GetEnvironmentVariable("MOGHOUSE_TEST_USER") ?? "";
        string pass = Environment.GetEnvironmentVariable("MOGHOUSE_TEST_PASS") ?? "";

        if (host.Length == 0 || user.Length == 0 || pass.Length == 0)
        {
            Console.WriteLine("set MOGHOUSE_TEST_HOST, MOGHOUSE_TEST_USER and MOGHOUSE_TEST_PASS");
            return null;
        }

        FfxiHuffmanTables? tables = FfxiHuffmanTables.TryLoadDefault();
        if (tables is null)
        {
            Console.WriteLine("compression tables not found - set MOGHOUSE_FFXI_RES to a directory " +
                              "holding compress.dat and decompress.dat");
            return null;
        }

        var session = new FfxiGameSession(new FfxiHuffman(tables));
        var profile = new FfxiServerProfile { Host = host, Username = user, Password = pass };

        Console.WriteLine($"connecting to {host}...");
        (FfxiLoginResponse login, IReadOnlyList<FfxiCharacter> characters) =
            await session.LoginAsync(profile);

        if (login.Result != FfxiLoginResult.Success)
        {
            Console.WriteLine($"login failed: {login.ErrorMessage ?? login.Result.ToString()}");
            return null;
        }

        Console.WriteLine($"logged in - {characters.Count} character(s)");

        // A fresh account comes back as sixteen empty slots rather than an empty
        // list, and zoning in as one of those hangs waiting for a reply that
        // never arrives. Take the first slot with a name; make one if there is
        // none, which is what a brand new test account needs.
        FfxiCharacter? character = FirstNamed(characters);
        if (character is null)
        {
            string wantedName = Environment.GetEnvironmentVariable("MOGHOUSE_TEST_CHARACTER") ?? "Testy";
            Console.WriteLine($"no characters on this account - creating {wantedName}...");

            var wanted = new FfxiNewCharacter(wantedName, FfxiRaceId.HumeMale, 0,
                                              FfxiBodySize.Medium, FfxiStartingJob.Warrior,
                                              FfxiNation.Bastok);

            string? refused = await session.CreateCharacterAsync(wanted, login.SessionHash!);
            if (refused is not null)
            {
                Console.WriteLine($"character creation refused: {refused}");
                return null;
            }

            // The roster has to be read again for the new character's id.
            (login, characters) = await session.LoginAsync(profile);
            character = FirstNamed(characters);
            if (character is null)
            {
                Console.WriteLine("created a character but the roster still shows none");
                return null;
            }
            Console.WriteLine($"created {character.Name}");
        }

        Console.WriteLine($"entering the world as {character.Name}...");
        await session.ConnectToZoneAsync(character, login.SessionHash!, host);
        await session.StartHeartbeatAsync();

        FfxiZoneLoginReply? state = session.ZoneState;
        if (state is null)
        {
            Console.WriteLine("zoned in but the server sent no zone state");
            return null;
        }

        Console.WriteLine($"in zone {state.ZoneNo} at {session.PosX:F1} {session.PosVertical:F1} {session.PosDepth:F1}");
        return new LoggedIn(session, character.Name, state.ZoneNo,
                            session.PosX, session.PosVertical, session.PosDepth);
    }

    private static FfxiCharacter? FirstNamed(IReadOnlyList<FfxiCharacter> characters)
    {
        foreach (FfxiCharacter candidate in characters)
        {
            if (!string.IsNullOrWhiteSpace(candidate.Name))
            {
                return candidate;
            }
        }
        return null;
    }
}
