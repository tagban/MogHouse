namespace MogHouse.Core.Ffxi;

/// <summary>What a typed line turned out to be.</summary>
public enum FfxiClientCommandKind
{
    /// <summary>Not a command. Say it.</summary>
    None,

    /// <summary>Leave the world and go back to the character list.</summary>
    Logout,

    /// <summary>End the session and close the client.</summary>
    Shutdown,

    /// <summary>Recognised, but not something this client does yet.</summary>
    Unsupported,
}

/// <summary>The command a line was, and anything after it.</summary>
public sealed record FfxiClientCommand(FfxiClientCommandKind Kind, string Name, string Rest);

/// <summary>
/// Slash commands the client answers itself.
///
/// Most of what someone types is chat, and a few things are not. '!' commands
/// are not here at all - the server routes those, so they travel as ordinary
/// chat and this leaves them alone. '/' commands are the client's own, and the
/// two that matter are the two ways of leaving: /logout goes back to the
/// character list, /shutdown ends the session.
/// </summary>
public static class FfxiClientCommands
{
    /// <summary>
    /// Works out what a line is. Anything not recognised is chat, including a
    /// slash command we have not implemented - saying it out loud is wrong, so
    /// those come back as Unsupported rather than None.
    /// </summary>
    public static FfxiClientCommand Parse(string line)
    {
        string trimmed = line.Trim();
        if (trimmed.Length < 2 || trimmed[0] != '/')
        {
            return new FfxiClientCommand(FfxiClientCommandKind.None, "", line);
        }

        int space = trimmed.IndexOf(' ');
        string name = (space < 0 ? trimmed[1..] : trimmed[1..space]).ToLowerInvariant();
        string rest = space < 0 ? "" : trimmed[(space + 1)..].Trim();

        return name switch
        {
            "logout" => new FfxiClientCommand(FfxiClientCommandKind.Logout, name, rest),
            "shutdown" or "quit" => new FfxiClientCommand(FfxiClientCommandKind.Shutdown, name, rest),
            _ => new FfxiClientCommand(FfxiClientCommandKind.Unsupported, name, rest),
        };
    }
}
