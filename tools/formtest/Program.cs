using MogHouse.Core.Interop;

namespace MogHouse.Tools.FormTest;

/// <summary>
/// Shows a login-shaped form through the interop boundary and prints what comes
/// back, so the C# to C++ crossing can be exercised without a server, an
/// account, or a zone.
///
/// The renderer runs on the main thread, because on macOS a window has to be
/// made there; the polling that would be the client's session loop runs beside
/// it, which is the same shape the real client uses.
/// </summary>
internal static class Program
{
    private static int Main()
    {
        // Deliberately not the login screen's real row list - this is here to
        // exercise the boundary, and it carries one of every row kind so a
        // marshalling fault shows up as a visibly wrong screen.
        NativeFormRow[] rows =
        [
            NativeFormRow.Label("SIGN IN TO CONTINUE"),
            NativeFormRow.Field("SERVER", "127.0.0.1"),
            NativeFormRow.Field("USERNAME", "tagban"),
            NativeFormRow.Secret("PASSWORD"),
            NativeFormRow.Button("LOG IN"),
            NativeFormRow.Button("REGISTER", enabled: false),
        ];

        using var viewer = new NativeViewer(new NativeViewerOptions
        {
            ZonePath = string.Empty,
            KeyTablePath = string.Empty,
            KeyTable2Path = string.Empty,

            // So this can be checked without anyone watching it.
            ScreenshotPath = Environment.GetEnvironmentVariable("MOGHOUSE_SCREENSHOT"),
            ScreenshotAfterFrames =
                int.TryParse(Environment.GetEnvironmentVariable("MOGHOUSE_SCREENSHOT_AFTER"), out int after)
                    ? after
                    : 0,
        });

        viewer.ShowForm("MOGHOUSE XI", "PRESS TAB TO MOVE BETWEEN FIELDS", rows);

        var poller = new Thread(() =>
        {
            while (true)
            {
                if (viewer.TakeFormResult() is { } result)
                {
                    Console.WriteLine($"pressed row {result.Button} ({rows[result.Button].Text})");
                    for (int i = 0; i < rows.Count(); i++)
                    {
                        Console.WriteLine($"  [{i}] {rows[i].Kind,-6} {rows[i].Text,-20} = \"{result[i]}\"");
                    }

                    viewer.HideForm();
                    return;
                }

                Thread.Sleep(16);
            }
        })
        { IsBackground = true };
        poller.Start();

        return viewer.Run();
    }
}
