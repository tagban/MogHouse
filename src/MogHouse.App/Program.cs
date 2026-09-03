using System;

namespace MogHouse.App;

/// <summary>
/// The executable. Everything it does is in <see cref="MogHouse.Core.Screens.ClientFlow"/>;
/// what is here is the little that has to happen before there is a window,
/// and the reason this file is so short is worth keeping in mind when adding
/// to it.
///
/// <para>
/// This used to be an Avalonia application: a launcher window for signing in
/// and choosing a character, which then opened the renderer's window beside
/// it for the world. Two windowing systems in one process, competing for the
/// one main thread that macOS insists windows are made on - which is what
/// black-screened the Mac after every successful login. The screens moved
/// into the renderer, the game logic moved into Core, and Avalonia went with
/// the last view that needed it. The renderer owns the main thread now and
/// the process has one window, on all three platforms.
/// </para>
/// </summary>
sealed class Program
{
    // STA for the folder chooser: on Windows it is a COM dialog, and COM
    // wants the thread that shows it to say so.
    [STAThread]
    public static int Main(string[] args)
    {
        StartLogging();
        HideRuntimeFolder();

        // Note what is NOT here: no await, and no thread of its own. This call
        // hands the main thread to the renderer, and the main thread is the
        // only one AppKit will make a window on.
        //
        // `args` is not looked at. `--screens` used to choose this path over
        // the launcher; it is the only path now, and a script still passing
        // the flag should keep working rather than be told off.
        return MogHouse.Core.Screens.ClientFlow.Run();
    }

    /// <summary>
    /// Marks the runtime folder hidden, so the folder a player opens holds an
    /// executable, their own settings and a README rather than the several
    /// hundred files underneath.
    ///
    /// Done here rather than when the package is built, for two reasons: a zip
    /// carries no Windows attributes, so it would not survive the trip; and
    /// Compress-Archive silently skips hidden folders, so setting it before
    /// zipping produced an archive with the runtime missing entirely.
    ///
    /// Best effort. A folder that will not take the attribute is untidy, not
    /// broken, and a development checkout has no such folder at all.
    /// </summary>
    private static void HideRuntimeFolder()
    {
        if (!OperatingSystem.IsWindows())
        {
            return;
        }

        try
        {
            var runtime = new System.IO.DirectoryInfo(
                System.IO.Path.Combine(AppContext.BaseDirectory, "data"));
            if (runtime.Exists && !runtime.Attributes.HasFlag(System.IO.FileAttributes.Hidden))
            {
                runtime.Attributes |= System.IO.FileAttributes.Hidden;
            }
        }
        catch (Exception)
        {
        }
    }

    /// <summary>
    /// Sends everything this client says to a file.
    ///
    /// This is a windowed application, so it has no console: anything written
    /// to standard output goes nowhere, and redirecting it captures an empty
    /// file. Which means that for every bug reported from inside the game
    /// there has been nothing to read afterwards, and the only way to work out
    /// what happened has been to reason about it - twice at length, and wrong
    /// both times.
    ///
    /// MOGHOUSE_LOG=path says where. Unbuffered, because the interesting case
    /// is a client that stopped.
    /// </summary>
    private static void StartLogging()
    {
        string? path = Environment.GetEnvironmentVariable("MOGHOUSE_LOG");
        if (string.IsNullOrWhiteSpace(path))
        {
            // Default rather than give up. A packaged build has no console and
            // nobody sets this, so without a default a released client says
            // nothing anywhere - and the one thing a bug report needs is these
            // two files. The config directory is used because it is already the
            // directory established to be writable: beside the executable where
            // that works, and a per-user directory where it does not, which is
            // the case inside a Flatpak or under a read-only /Applications.
            path = System.IO.Path.Combine(
                MogHouse.Core.Ffxi.FfxiServerProfileStore.DefaultConfigDirectory(), "moghouse.log");

            // The renderer writes its own file next to this one, taking the
            // path from this variable and appending ".renderer" - it is a
            // separate process-wide setting rather than something passed in, so
            // it has to be exported rather than just used locally.
            MogHouse.Core.Interop.NativeEnvironment.Set("MOGHOUSE_LOG", path);
        }

        try
        {
            var file = new System.IO.StreamWriter(path, append: false) { AutoFlush = true };
            Console.SetOut(file);
            Console.SetError(file);
            Console.WriteLine($"MogHouse {DateTimeOffset.Now:u}");

            AppDomain.CurrentDomain.UnhandledException += (_, e) =>
                Console.WriteLine($"UNHANDLED: {e.ExceptionObject}");

            // An exception in a task nobody awaited is swallowed and the client
            // carries on half-broken, which is its own kind of invisible.
            System.Threading.Tasks.TaskScheduler.UnobservedTaskException += (_, e) =>
                Console.WriteLine($"UNOBSERVED: {e.Exception}");
        }
        catch (Exception)
        {
        }
    }
}
