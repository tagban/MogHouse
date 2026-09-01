using Avalonia;
using System;

namespace MogHouse.App;

sealed class Program
{
    // Initialization code. Don't use any Avalonia, third-party APIs or any
    // SynchronizationContext-reliant code before AppMain is called: things aren't initialized
    // yet and stuff might break.
    [STAThread]
    public static void Main(string[] args)
    {
        StartLogging();
        HideRuntimeFolder();
        BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
    }

    /// <summary>
    /// Sends everything this client says to a file, if asked.
    ///
    /// This is a windowed application, so it has no console: anything written
    /// to standard output goes nowhere, and redirecting it captures an empty
    /// file. Which means that for every bug reported from inside the game
    /// there has been nothing to read afterwards, and the only way to work out
    /// what happened has been to reason about it - twice at length, and wrong
    /// both times.
    ///
    /// MOGHOUSE_LOG=path writes it down instead. Unbuffered, because the
    /// interesting case is a client that stopped.
    /// </summary>
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

    private static void StartLogging()
    {
        string? path = Environment.GetEnvironmentVariable("MOGHOUSE_LOG");
        if (string.IsNullOrWhiteSpace(path))
        {
            return;
        }

        try
        {
            var file = new System.IO.StreamWriter(path, append: false) { AutoFlush = true };
            Console.SetOut(file);
            Console.SetError(file);
            Console.WriteLine($"MogHouse {DateTimeOffset.Now:u}");

            AppDomain.CurrentDomain.UnhandledException += (_, e) =>
                Console.WriteLine($"UNHANDLED: {e.ExceptionObject}");

            // An exception inside a Dispatcher.Post is swallowed and the app
            // carries on half-broken, which is its own kind of invisible.
            System.Threading.Tasks.TaskScheduler.UnobservedTaskException += (_, e) =>
                Console.WriteLine($"UNOBSERVED: {e.Exception}");
        }
        catch (Exception)
        {
        }
    }

    // Avalonia configuration, don't remove; also used by visual designer.
    public static AppBuilder BuildAvaloniaApp()
        => AppBuilder.Configure<App>()
            .UsePlatformDetect()
#if DEBUG
            .WithDeveloperTools()
#endif
            .WithInterFont()
            .LogToTrace();
}
