using System.Text.RegularExpressions;

namespace MogHouse.Core.Tests;

/// <summary>
/// The escape-out-of-character-select id is written down twice.
///
/// The renderer posts it through the same channel a click uses, so it is a
/// number rather than a call, and a number written in two languages drifts.
/// This is the same guard <see cref="VersionAgreementTests"/> puts on the
/// version and hud_shader.h puts on its glyph counts.
/// </summary>
public class LineupCancelAgreementTests
{
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

    private static string? Find(string root, string relative, string pattern)
    {
        string path = Path.Combine(root, relative);
        if (!File.Exists(path))
        {
            return null;
        }

        Match found = Regex.Match(File.ReadAllText(path), pattern, RegexOptions.Multiline);
        return found.Success ? found.Groups[1].Value.ToUpperInvariant() : null;
    }

    [Fact]
    public void TheRendererAndTheScreenAgreeOnTheCancelId()
    {
        if (RepositoryRoot() is not { } root)
        {
            return;   // not a checkout; nothing to compare
        }

        string? renderer = Find(root, Path.Combine("renderer", "viewer.h"),
                                @"kLineupCancelled = 0x([0-9A-Fa-f]+)u");
        string? screen = Find(root, Path.Combine("src", "MogHouse.Core", "Screens", "CharacterScreens.cs"),
                              @"const uint Cancelled = 0x([0-9A-Fa-f]+)u");

        Assert.NotNull(renderer);
        Assert.NotNull(screen);
        Assert.Equal(renderer, screen);
    }
}
