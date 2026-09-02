using MogHouse.Core.Ffxi;
using MogHouse.Core.Interop;

namespace MogHouse.Core.Screens;

/// <summary>
/// Choosing who to play, and making one when there is nobody to choose.
/// </summary>
public static class CharacterScreens
{
    private const string Back = "BACK";

    /// <summary>
    /// What the player decided at character select.
    /// </summary>
    /// <param name="Chosen">Who to play, when they picked somebody.</param>
    /// <param name="WantsNew">Whether they asked to make one instead.</param>
    public sealed record Choice(FfxiCharacter? Chosen, bool WantsNew)
    {
        public static readonly Choice New = new(null, true);
    }

    /// <summary>
    /// Lists the account's characters as buttons. Returns null if the player
    /// closed the window or asked to quit.
    ///
    /// A fresh account comes back as sixteen empty slots rather than an empty
    /// list, so the unnamed ones are left out - they are slots, not people, and
    /// zoning in as one hangs waiting for a reply that never comes.
    /// </summary>
    /// <summary>
    /// The id given to the figure that stands for "make a new one". Well clear
    /// of the roster's own indices, so a click is never ambiguous.
    /// </summary>
    private const uint NewCharacterId = 0xFFFF_FF01;

    /// <summary>
    /// The id of the first character in the line-up. Anything but zero, because
    /// zero is what <see cref="LiveRadar.TakeTalk"/> answers when nobody has
    /// been clicked - given to the first character, it reads as that character
    /// being chosen the instant the screen appears, which is precisely what it
    /// did.
    /// </summary>
    private const uint FirstCharacterId = 1;

    /// <summary>
    /// A Mithra stands in for the character that does not exist yet. The retail
    /// screen shows an empty slot; a figure to walk up to and click reads
    /// better in a line-up, and it is the one race nobody has to be told is not
    /// theirs.
    /// </summary>
    private const ushort NewCharacterRace = (ushort)FfxiRaceId.Mithra;

    /// <summary>A pale blank shape - a person, but nobody yet.</summary>
    private const int BlankStyle = 1;

    /// <summary>Themselves, but faded until the cursor is over them.</summary>
    private const int FadedStyle = 2;

    public static Choice? Select(ScreenHost screens, LiveRadar world,
                                 IReadOnlyList<FfxiCharacter> characters, string message = "")
    {
        List<FfxiCharacter> named =
            [.. characters.Where(c => !string.IsNullOrWhiteSpace(c.Name))];

        // Everyone on the account, standing in the world, plus the figure that
        // means "one more". The renderer stands them on the floor and looks at
        // them; all this decides is who is in the row and in what order.
        var cast = new List<(uint Id, string Name, ushort Race, ushort Face, int Style)>();
        for (int i = 0; i < named.Count; i++)
        {
            // Faded until pointed at, the way an invisible player looks, so the
            // one under the cursor is plainly the one that would be picked.
            cast.Add((FirstCharacterId + (uint)i, named[i].Name, named[i].Race, named[i].Face, FadedStyle));
        }

        cast.Add((NewCharacterId, "NEW CHARACTER", NewCharacterRace, 0, BlankStyle));

        world.ShowLineup(cast);

        // No panel. The people standing there are the choice - a dialog listing
        // the same names in front of them is asking the question twice, and it
        // covers the very thing it is asking about.
        screens.Clear();

        if (named.Count == 0)
        {
            world.Say(null, "No characters on this account yet - pick the Mithra to make one.");
        }
        else if (message.Length > 0)
        {
            // Whatever went wrong last time goes to the chat line rather than
            // into a box over the roster.
            world.Say(null, message);
        }

        try
        {
            return WaitForClick(screens, world, named);
        }
        finally
        {
            // Down on every way out of here, including the window closing, so
            // the roster is never left standing in the world behind whatever
            // comes next.
            world.HideLineup();
        }
    }

    /// <summary>
    /// Waits for one of the figures to be clicked. Null if the window closes
    /// first, which is the player leaving.
    /// </summary>
    private static Choice? WaitForClick(ScreenHost screens, LiveRadar world,
                                        IReadOnlyList<FfxiCharacter> named)
    {
        while (!screens.Closed)
        {
            // The id is the one handed to ShowLineup, so it says directly which
            // of them was picked.
            uint clicked = world.TakeTalk();
            if (clicked == NewCharacterId)
            {
                return Choice.New;
            }

            if (clicked >= FirstCharacterId && clicked - FirstCharacterId < named.Count)
            {
                return new Choice(named[(int)(clicked - FirstCharacterId)], false);
            }

            Thread.Sleep(16);
        }

        return null;
    }

    /// <summary>Everything the server needs to build somebody, and how to say it.</summary>
    private static readonly (string Label, FfxiRaceId Value)[] Races =
    [
        ("HUME MALE", FfxiRaceId.HumeMale),
        ("HUME FEMALE", FfxiRaceId.HumeFemale),
        ("ELVAAN MALE", FfxiRaceId.ElvaanMale),
        ("ELVAAN FEMALE", FfxiRaceId.ElvaanFemale),
        ("TARUTARU MALE", FfxiRaceId.TarutaruMale),
        ("TARUTARU FEMALE", FfxiRaceId.TarutaruFemale),
        ("MITHRA", FfxiRaceId.Mithra),
        ("GALKA", FfxiRaceId.Galka),
    ];

    private static readonly (string Label, FfxiStartingJob Value)[] Jobs =
    [
        ("WARRIOR", FfxiStartingJob.Warrior),
        ("MONK", FfxiStartingJob.Monk),
        ("WHITE MAGE", FfxiStartingJob.WhiteMage),
        ("BLACK MAGE", FfxiStartingJob.BlackMage),
        ("RED MAGE", FfxiStartingJob.RedMage),
        ("THIEF", FfxiStartingJob.Thief),
    ];

    private static readonly (string Label, FfxiNation Value)[] Nations =
    [
        ("BASTOK", FfxiNation.Bastok),
        ("SAN D'ORIA", FfxiNation.SanDOria),
        ("WINDURST", FfxiNation.Windurst),
    ];

    private static readonly (string Label, FfxiBodySize Value)[] Sizes =
    [
        ("MEDIUM", FfxiBodySize.Medium),
        ("SMALL", FfxiBodySize.Small),
        ("LARGE", FfxiBodySize.Large),
    ];

    /// <summary>
    /// Makes a character. Returns what to ask the server for, or null if the
    /// player backed out.
    ///
    /// <para>
    /// The choices are buttons that show what they are set to and step on when
    /// pressed, rather than fields to type a number into. The form widget has
    /// text boxes and buttons and nothing else, and a button that names its own
    /// value needs neither a new kind of row nor anyone to know that a Galka is
    /// an 8.
    /// </para>
    /// </summary>
    public static FfxiNewCharacter? Make(ScreenHost screens, string suggestedName = "")
    {
        string name = suggestedName;
        int race = 0, job = 0, nation = 0, size = 0;
        string message = "";

        while (true)
        {
            NativeFormRow[] rows =
            [
                NativeFormRow.Field("NAME", name),
                NativeFormRow.Button($"RACE: {Races[race].Label}"),
                NativeFormRow.Button($"JOB: {Jobs[job].Label}"),
                NativeFormRow.Button($"NATION: {Nations[nation].Label}"),
                NativeFormRow.Button($"BUILD: {Sizes[size].Label}"),
                NativeFormRow.Button("CREATE"),
                NativeFormRow.Button(Back),
            ];

            NativeFormResult? result = screens.Ask("CREATE A CHARACTER", message, rows);
            if (result is null)
            {
                return null;
            }

            name = result[0].Trim();

            switch (result.Button)
            {
                case 1: race = (race + 1) % Races.Length; break;
                case 2: job = (job + 1) % Jobs.Length; break;
                case 3: nation = (nation + 1) % Nations.Length; break;
                case 4: size = (size + 1) % Sizes.Length; break;

                case 5 when name.Length == 0:
                    message = "A NAME IS NEEDED.";
                    break;

                case 5:
                    return new FfxiNewCharacter(name, Races[race].Value, 0,
                                                Sizes[size].Value, Jobs[job].Value,
                                                Nations[nation].Value);

                default:
                    return null;   // Back
            }
        }
    }
}
