using MogHouse.Core.Ffxi;

namespace MogHouse.Core.Tests.Ffxi;

public class BugCommandTests
{
    [Theory]
    [InlineData("/bug the torch is not lit")]
    [InlineData("/report the torch is not lit")]
    public void TakesWhatWasTyped(string line)
    {
        FfxiClientCommand command = FfxiClientCommands.Parse(line);

        Assert.Equal(FfxiClientCommandKind.Bug, command.Kind);
        Assert.Equal("the torch is not lit", command.Rest);
    }

    /// <summary>
    /// An empty report is refused rather than said out loud. Everything not
    /// recognised as a command is broadcast to the zone, and "/bug" arriving
    /// in everyone's chat log is not what the reporter meant.
    /// </summary>
    [Fact]
    public void RefusesAnEmptyOne()
    {
        Assert.Equal(FfxiClientCommandKind.Incomplete, FfxiClientCommands.Parse("/bug").Kind);
    }
}

public class FfxiBugReportTests
{
    private static FfxiBugReport.Context Somewhere(string what = "the torch is not lit") =>
        new(What: what, Who: "Tester", ZoneNo: 230, ZoneName: "Southern San d'Oria",
            X: -93.7f, Vertical: 0.1f, Depth: 19.1f, Facing: 42,
            VanadielHour: 23, GameTime: 1234, Weather: FfxiWeather.Clouds);

    /// <summary>
    /// The whole point: a report carries enough to stand where the reporter
    /// stood. Losing any of these turns looking into it back into guessing at
    /// camera angles.
    /// </summary>
    [Fact]
    public void CarriesEnoughToStandThere()
    {
        string written = Format(Somewhere());

        Assert.Contains("the torch is not lit", written);
        Assert.Contains("Southern San d'Oria", written);
        Assert.Contains("-93.7", written);
        Assert.Contains("19.1", written);
        Assert.Contains("42", written);              // facing
        Assert.Contains("Clouds", written);          // weather
        Assert.Contains("MOGHOUSE_CAMERA", written); // and a line to run
    }

    [Fact]
    public void ShortensALongOneForItsHeading()
    {
        string written = Format(Somewhere(new string('x', 200)));

        Assert.Contains("...", written);
    }

    /// <summary>
    /// Appends rather than replaces - a second report must not lose the first.
    /// </summary>
    [Fact]
    public void KeepsTheOnesBeforeIt()
    {
        string one = Format(Somewhere("first"));
        string two = Format(Somewhere("second"));

        Assert.DoesNotContain("second", one);
        Assert.DoesNotContain("first", two);
    }

    /// <summary>
    /// Exercises the formatting in a directory of its own.
    ///
    /// Takes the path rather than setting the store's global override, which
    /// is shared: doing it that way broke two unrelated tests, because xUnit
    /// runs classes in parallel and they were both moving the same static.
    /// </summary>
    private static string Format(FfxiBugReport.Context now)
    {
        string directory = Path.Combine(Path.GetTempPath(), Path.GetRandomFileName());
        Directory.CreateDirectory(directory);
        try
        {
            string? path = FfxiBugReport.Append(now, directory: directory);
            return path is null ? "" : File.ReadAllText(path);
        }
        finally
        {
            try { Directory.Delete(directory, recursive: true); } catch (Exception) { }
        }
    }
}
