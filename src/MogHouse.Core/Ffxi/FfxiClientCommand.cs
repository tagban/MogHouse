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

    /// <summary>Say something on a particular channel.</summary>
    Chat,

    /// <summary>Say something to one person.</summary>
    Tell,

    /// <summary>Talk to the nearest NPC, or one named after the command.</summary>
    Talk,

    /// <summary>Accept the home point after dying.</summary>
    HomePoint,

    /// <summary>Recognised, but the line was missing something it needed.</summary>
    Incomplete,

    /// <summary>Recognised, but not something this client does yet.</summary>
    Unsupported,
}

/// <summary>The command a line was, and anything after it.</summary>
/// <param name="Channel">Which channel a Chat goes out on.</param>
/// <param name="Recipient">Who a Tell is addressed to.</param>
public sealed record FfxiClientCommand(
    FfxiClientCommandKind Kind,
    string Name,
    string Rest,
    FfxiChatKind Channel = FfxiChatKind.Say,
    string Recipient = "");

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
            "homepoint" or "hp" or "return" => new FfxiClientCommand(FfxiClientCommandKind.HomePoint, name, rest),
            "talk" or "trigger" => new FfxiClientCommand(FfxiClientCommandKind.Talk, name, rest),

            // The channels, under the names the real client answers to. Every
            // one has a short form because nobody types /linkshell twice.
            "say" or "s" => Speak(name, rest, FfxiChatKind.Say),
            "shout" or "sh" => Speak(name, rest, FfxiChatKind.Shout),
            "yell" or "y" => Speak(name, rest, FfxiChatKind.Yell),
            "party" or "p" => Speak(name, rest, FfxiChatKind.Party),
            "linkshell" or "l" or "ls" => Speak(name, rest, FfxiChatKind.Linkshell1),
            "linkshell2" or "l2" or "ls2" => Speak(name, rest, FfxiChatKind.Linkshell2),
            "unity" or "u" => Speak(name, rest, FfxiChatKind.Unity),
            "emote" or "em" or "me" => Speak(name, rest, FfxiChatKind.Emote),

            // A tell needs someone to tell. The name is the first word and
            // everything after it is the message, which is why this cannot go
            // through Speak.
            "tell" or "t" or "whisper" or "w" or "send" => Whisper(name, rest),

            _ => new FfxiClientCommand(FfxiClientCommandKind.Unsupported, name, rest),
        };
    }

    /// <summary>A channel command, or a complaint if there was nothing to say.</summary>
    private static FfxiClientCommand Speak(string name, string rest, FfxiChatKind channel) =>
        rest.Length == 0
            ? new FfxiClientCommand(FfxiClientCommandKind.Incomplete, name, "needs something to say")
            : new FfxiClientCommand(FfxiClientCommandKind.Chat, name, rest, channel);

    /// <summary>A tell, split into who and what.</summary>
    private static FfxiClientCommand Whisper(string name, string rest)
    {
        int space = rest.IndexOf(' ');
        if (space <= 0)
        {
            return new FfxiClientCommand(FfxiClientCommandKind.Incomplete, name,
                                         "needs a name and a message, as /" + name + " Someone hello");
        }

        string recipient = rest[..space];
        string message = rest[(space + 1)..].Trim();
        return message.Length == 0
            ? new FfxiClientCommand(FfxiClientCommandKind.Incomplete, name, "needs a message after the name")
            : new FfxiClientCommand(FfxiClientCommandKind.Tell, name, message, FfxiChatKind.Say, recipient);
    }
}
