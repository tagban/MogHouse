// Log in, zone in, and draw the world - with no Avalonia, and the renderer on
// the main thread.
//
// This is the shape the client is moving to, small enough to read in one go. It
// exists because the client cannot currently do this at all on macOS: LiveRadar
// starts the render loop on a thread of its own, the loop creates the window,
// and AppKit refuses to make an NSWindow anywhere but the main thread. The
// symptom is a successful login followed by a black window and a renderer log
// that says "SDL_Init failed: No available video device".
//
// The order here is the whole point:
//
//   1. log in and zone in first, on this thread, while there is no window yet
//   2. then hand this thread - the main one - to the render loop, which blocks
//   3. the session keeps running on its own background threads throughout
//
// Nothing about it is macOS-specific. Windows has no main-thread rule but is
// happy to obey one, and Linux prefers it.
//
// Credentials come from the environment and are never stored:
//
//   MOGHOUSE_TEST_HOST=your.server
//   MOGHOUSE_TEST_USER=name
//   MOGHOUSE_TEST_PASS=secret
//   dotnet run --project tools/loginzone
//
// Prefix the command with a space if your shell keeps history.

using System;
using System.Collections.Generic;
using System.Threading.Tasks;
using MogHouse.Core.Ffxi;

internal static class Program
{
    private static async Task<int> Main()
    {
        string host = Environment.GetEnvironmentVariable("MOGHOUSE_TEST_HOST") ?? "";
        string user = Environment.GetEnvironmentVariable("MOGHOUSE_TEST_USER") ?? "";
        string pass = Environment.GetEnvironmentVariable("MOGHOUSE_TEST_PASS") ?? "";

        if (host.Length == 0 || user.Length == 0 || pass.Length == 0)
        {
            Console.WriteLine("set MOGHOUSE_TEST_HOST, MOGHOUSE_TEST_USER and MOGHOUSE_TEST_PASS");
            return 2;
        }

        Console.WriteLine($"main thread id={Environment.CurrentManagedThreadId}");

        var session = new FfxiGameSession();
        var profile = new FfxiServerProfile { Host = host, Username = user, Password = pass };

        // --- log in, before there is a window -----------------------------------

        Console.WriteLine($"connecting to {host}...");
        (FfxiLoginResponse login, IReadOnlyList<FfxiCharacter> characters) =
            await session.LoginAsync(profile);

        if (login.Result != FfxiLoginResult.Success)
        {
            Console.WriteLine($"login failed: {login.ErrorMessage ?? login.Result.ToString()}");
            return 1;
        }

        Console.WriteLine($"logged in - {characters.Count} character(s)");
        if (characters.Count == 0)
        {
            Console.WriteLine("no characters on this account; make one in the client first");
            return 1;
        }

        FfxiCharacter character = characters[0];
        Console.WriteLine($"entering the world as {character.Name}...");

        await session.ConnectToZoneAsync(character, login.SessionHash!, host);
        await session.StartHeartbeatAsync();

        FfxiZoneLoginReply? state = session.ZoneState;
        if (state is null)
        {
            Console.WriteLine("zoned in but the server sent no zone state");
            return 1;
        }

        Console.WriteLine($"in zone {state.ZoneNo} at {session.PosX:F1} {session.PosVertical:F1} {session.PosDepth:F1}");

        // --- then give this thread to the renderer ------------------------------

        // ownThread: false is the whole difference. The loop is not started for
        // us; it is started below, here, on the main thread.
        LiveRadar? world = LiveRadar.Open(
            (int)state.ZoneNo, session.PosX, session.PosVertical, session.PosDepth,
            serverClock: 0, playerName: character.Name, playerLook: null, ownThread: false);

        if (world is null)
        {
            Console.WriteLine("could not open the world window - see the message above");
            return 1;
        }

        // Feeding the session from a background thread while the main one draws,
        // which is the arrangement the client will use. Position only: this is a
        // demonstration of the handoff, not a replacement for GameViewModel.
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
}
