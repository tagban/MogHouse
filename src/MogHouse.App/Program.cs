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
