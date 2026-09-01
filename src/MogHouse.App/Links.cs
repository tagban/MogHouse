using System;
using System.Diagnostics;
using System.Text.RegularExpressions;

namespace MogHouse.App;

/// <summary>
/// Finding links in text, and opening them.
///
/// The server talks in links - LandSandBoat's own welcome says "please visit
/// https://github.com/LandSandBoat/server" the moment you zone in - and a link
/// you cannot click is a link you have to retype.
/// </summary>
public static partial class Links
{
    /// <summary>Where a bug goes.</summary>
    public const string Issues = "https://github.com/tagban/MogHouse/issues";

    /// <summary>The dev channel.</summary>
    public const string Discord = "https://discord.gg/6KmGcxcHQu";

    /// <summary>
    /// Deliberately narrow: http and https only.
    ///
    /// Chat is other people's text, and a pattern that matched every scheme
    /// would turn whatever someone typed into something this client offers to
    /// launch. A trailing bracket or full stop is left out of the match
    /// because a sentence ending in a link is more common than a link ending
    /// in punctuation.
    /// </summary>
    [GeneratedRegex(@"https?://[^\s<>""']+[^\s<>""'.,;:!?)\]}]", RegexOptions.IgnoreCase)]
    private static partial Regex UrlPattern();

    /// <summary>The first link in some text, or null.</summary>
    public static string? FirstIn(string? text)
    {
        if (string.IsNullOrEmpty(text))
        {
            return null;
        }
        Match match = UrlPattern().Match(text);
        return match.Success ? match.Value : null;
    }

    /// <summary>
    /// Hands a URL to whatever the machine opens links with.
    ///
    /// UseShellExecute is what makes that the browser rather than an attempt to
    /// run the string as a program, and on .NET it is off by default.
    /// </summary>
    public static void Open(string? url)
    {
        if (string.IsNullOrWhiteSpace(url) ||
            !Uri.TryCreate(url, UriKind.Absolute, out Uri? parsed) ||
            (parsed.Scheme != Uri.UriSchemeHttp && parsed.Scheme != Uri.UriSchemeHttps))
        {
            return;
        }

        try
        {
            Process.Start(new ProcessStartInfo(parsed.AbsoluteUri) { UseShellExecute = true });
        }
        catch (Exception)
        {
            // Nothing useful to do about a machine with no browser, and a
            // client that falls over because a link would not open is worse
            // than one that quietly does not open it.
        }
    }
}
