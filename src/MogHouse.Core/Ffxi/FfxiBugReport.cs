using System.Text;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// Writes down where somebody was standing when they saw something wrong.
///
/// <para>
/// Reporting a graphics fault by hand is a chore and the useful half is always
/// missing: which zone, standing where, facing what, at what hour and in what
/// weather. Without those, looking into it means guessing at camera angles
/// until the thing comes back into view - which is how the afternoon of
/// 2026-09-03 went, twice.
/// </para>
///
/// <para>
/// So <c>/bug</c> takes what the client already knows and appends it to a file
/// beside the log, with a line that puts the standalone renderer at the same
/// spot. One report is then enough to reproduce the fault without the person
/// who found it being present.
/// </para>
///
/// <para>
/// Deliberately a plain file rather than an issue raised over the network:
/// somebody in the middle of playing should not wait on an HTTP round trip, or
/// need a token, or lose the report because they were offline. Collecting them
/// afterwards is somebody else's job and an easy one.
/// </para>
/// </summary>
public static class FfxiBugReport
{
    /// <summary>Where the reports go - beside moghouse.log, which is already writable.</summary>
    public static string FilePath =>
        Path.Combine(FfxiServerProfileStore.DefaultConfigDirectory(), "bug-reports.md");

    /// <summary>Everything worth knowing about the moment somebody hit /bug.</summary>
    public sealed record Context(
        string What,
        string Who,
        uint ZoneNo,
        string ZoneName,
        float X,
        float Vertical,
        float Depth,
        sbyte Facing,
        int VanadielHour,
        uint GameTime,
        FfxiWeather Weather);

    /// <summary>
    /// Appends one report and returns the path it went to, or null if it could
    /// not be written - which is worth telling the reporter, because a bug
    /// report that silently went nowhere is worse than none.
    /// </summary>
    public static string? Append(Context now, DateTimeOffset? at = null)
    {
        var report = new StringBuilder();
        report.Append('\n');
        report.Append($"## {(at ?? DateTimeOffset.Now):yyyy-MM-dd HH:mm:ss} - {Summarise(now.What)}\n\n");
        report.Append($"{now.What}\n\n");
        report.Append("| | |\n|---|---|\n");
        report.Append($"| zone | {now.ZoneNo} ({now.ZoneName}) |\n");
        report.Append($"| standing at | {now.X:F1} {now.Vertical:F1} {now.Depth:F1} |\n");
        report.Append($"| facing | {now.Facing} |\n");
        report.Append($"| character | {now.Who} |\n");
        report.Append($"| the hour | {now.VanadielHour:D2}:00 (game time {now.GameTime}) |\n");
        report.Append($"| weather | {now.Weather} |\n\n");

        // The point of the whole thing: a line that puts somebody else at the
        // same spot, without a server or an account.
        report.Append("Stand there:\n\n");
        report.Append("```sh\n");
        report.Append($"MOGHOUSE_TIME={now.VanadielHour:D2}00 " +
                      $"MOGHOUSE_WEATHER={(int)now.Weather} " +
                      $"MOGHOUSE_CAMERA=\"{now.X:F1},{now.Vertical + 2.0f:F1},{now.Depth:F1}\" \\\n");
        report.Append($"  ./build-renderer/moghouse-renderer \"$MOGHOUSE_FFXI_INSTALL/<zone {now.ZoneNo}>.DAT\"\n");
        report.Append("```\n");

        try
        {
            string path = FilePath;
            if (!File.Exists(path))
            {
                File.WriteAllText(path,
                    "# Bugs, as they were found\n\n" +
                    "Written by `/bug` from inside the client. Each entry carries where the\n" +
                    "reporter was standing, so it can be looked at without them.\n");
            }

            File.AppendAllText(path, report.ToString());
            return path;
        }
        catch (Exception)
        {
            // Best effort. A full disk or a read-only directory is not worth
            // ending somebody's session over.
            return null;
        }
    }

    /// <summary>The first few words, for the heading.</summary>
    private static string Summarise(string what)
    {
        string trimmed = what.Trim();
        return trimmed.Length <= 60 ? trimmed : trimmed[..57] + "...";
    }
}
