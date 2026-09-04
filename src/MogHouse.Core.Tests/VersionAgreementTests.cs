using System.Text.RegularExpressions;

namespace MogHouse.Core.Tests;

/// <summary>
/// The version is written in five places and they have to agree.
///
/// <para>
/// This has gone wrong twice. The window title was missed for 0.2.0, so a
/// released build introduced itself as 0.1.2; and the notarizer's default
/// stayed at 0.1.2 while the packager said 0.2.0, which only ever went
/// unnoticed because both were passed --version explicitly. Neither is the
/// kind of mistake a person catches by reading, and both ship.
/// </para>
///
/// <para>
/// One source of truth would be better, but the five are a C++ header, three
/// shell scripts and a PowerShell script, and there is no honest way to share
/// a constant between them without a build step that generates one. Checking
/// that they agree costs nothing and catches exactly what went wrong.
/// </para>
/// </summary>
public class VersionAgreementTests
{
    /// <summary>
    /// The repository root, found by walking up from the test binary, or null
    /// when the tests are run from somewhere that is not a checkout.
    /// </summary>
    private static string? RepositoryRoot()
    {
        var at = new DirectoryInfo(AppContext.BaseDirectory);
        for (int i = 0; i < 8 && at is not null; i++, at = at.Parent)
        {
            if (Directory.Exists(Path.Combine(at.FullName, "renderer")) &&
                Directory.Exists(Path.Combine(at.FullName, "tools")))
            {
                return at.FullName;
            }
        }

        return null;
    }

    /// <summary>Every file that names a version, and the pattern that finds it.</summary>
    private static readonly (string Path, string Pattern)[] Sites =
    [
        (Path.Combine("renderer", "viewer.h"), @"kWindowTitle = ""MogHouse XI - Alpha ([0-9.]+)"""),
        (Path.Combine("tools", "package-macos.sh"), @"^VERSION=""([0-9.]+)"""),
        (Path.Combine("tools", "package-linux.sh"), @"^VERSION=""([0-9.]+)"""),
        (Path.Combine("tools", "notarize-macos.sh"), @"^VERSION=""([0-9.]+)"""),
        (Path.Combine("tools", "package-windows.ps1"), @"\$Version = ""([0-9.]+)"""),
    ];

    [Fact]
    public void EveryPlaceThatNamesAVersionNamesTheSameOne()
    {
        if (RepositoryRoot() is not { } root)
        {
            // Published somewhere without the sources beside it. Nothing to
            // check rather than a failure: this guards the repository, not the
            // build output.
            return;
        }

        var found = new Dictionary<string, string>();
        foreach ((string relative, string pattern) in Sites)
        {
            string path = Path.Combine(root, relative);
            Assert.True(File.Exists(path), $"{relative} is missing - if it moved, this list has to move with it");

            Match match = Regex.Match(File.ReadAllText(path), pattern, RegexOptions.Multiline);
            Assert.True(match.Success, $"no version found in {relative} - the pattern here has gone stale");
            found[relative] = match.Groups[1].Value;
        }

        string[] distinct = found.Values.Distinct().ToArray();
        Assert.True(distinct.Length == 1,
                    "the version is not the same everywhere: " +
                    string.Join(", ", found.Select(entry => $"{entry.Key} says {entry.Value}")));
    }
}
