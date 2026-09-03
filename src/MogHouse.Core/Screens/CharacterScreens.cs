using System.Linq;
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
        var cast = new List<(uint Id, string Name, ushort Race, ushort Face, int Style, int Size)>();
        for (int i = 0; i < named.Count; i++)
        {
            // Faded until pointed at, the way an invisible player looks, so the
            // one under the cursor is plainly the one that would be picked.
            cast.Add((FirstCharacterId + (uint)i, named[i].Name, named[i].Race, named[i].Face, FadedStyle,
                      named[i].Size));
        }

        cast.Add((NewCharacterId, "NEW CHARACTER", NewCharacterRace, 0, BlankStyle, 1));

        world.ShowLineup(cast);

        // No panel in the ordinary case. The people standing there are the
        // choice - a dialog listing the same names in front of them is asking
        // the question twice, and it covers the very thing it is asking about.
        //
        // A failure is the exception, and goes in front.
        //
        // This used to go to the chat line, on the reasoning that a box over
        // the roster covers the very thing it is about. The reasoning was
        // sound and the place was not: the chat log is switched off during the
        // screens - there is no world for it to be about yet - so every
        // explanation was written to a panel that is not drawn. A server
        // refusing a character with "still logged in, try again in a minute"
        // looked exactly like a client that did nothing when clicked.
        //
        // Acknowledged rather than flashed past, because it ends the step:
        // whoever clicked is owed a reason they cannot miss. The line-up is
        // put up afterwards, so nothing covers it.
        if (message.Length > 0)
        {
            screens.Tell("THAT DID NOT WORK", message);
        }

        // Same reasoning: an empty account needs telling what to do, and the
        // chat line it used to be told through is not drawn here either.
        if (named.Count == 0)
        {
            screens.Tell("NOBODY HERE YET",
                         "THIS ACCOUNT HAS NO CHARACTERS. PICK THE MITHRA TO MAKE ONE.");
        }

        screens.Clear();

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
    /// <summary>
    /// The races as people think of them, with the two the lobby server
    /// numbers separately for each gender. Mithra and Galka have one each.
    /// </summary>
    private static readonly (string Label, FfxiRaceId Male, FfxiRaceId Female)[] Races =
    [
        ("HUME", FfxiRaceId.HumeMale, FfxiRaceId.HumeFemale),
        ("ELVAAN", FfxiRaceId.ElvaanMale, FfxiRaceId.ElvaanFemale),
        ("TARUTARU", FfxiRaceId.TarutaruMale, FfxiRaceId.TarutaruFemale),
        ("MITHRA", FfxiRaceId.Mithra, FfxiRaceId.Mithra),
        ("GALKA", FfxiRaceId.Galka, FfxiRaceId.Galka),
    ];

    private static readonly string[] Genders = ["MALE", "FEMALE"];

    /// <summary>
    /// Eight faces, each with two hairstyles. The game stores the pair as one
    /// number, face times two plus hair, and that number is the model id of
    /// the head - which is why a face is a face and a hair colour at once.
    /// </summary>
    private static readonly string[] Faces = ["1", "2", "3", "4", "5", "6", "7", "8"];

    private static readonly string[] Hairs = ["A", "B"];

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
    /// The id of the figure shown while a character is being made. Its own,
    /// clear of the roster and of the blank figure, so nothing else answers
    /// for it.
    /// </summary>
    private const uint PreviewId = 0xFFFF_FF02;

    /// <summary>
    /// Makes a character. Returns what to ask the server for, or null if the
    /// player backed out.
    ///
    /// <para>
    /// Every choice is a dropdown, and the character stands in the world beside
    /// the screen looking the way the choices say: change the race and the
    /// figure changes race, change the face and the face changes. The screen
    /// stands aside so the figure can be seen, and each pick hands the form
    /// back at once so the figure never lags the choice.
    /// </para>
    ///
    /// <para>
    /// Size scales the figure a little, the way the game does; the exact
    /// factors are the renderer's approximation.
    /// </para>
    /// </summary>
    public static FfxiNewCharacter? Make(ScreenHost screens, LiveRadar world, string suggestedName = "")
    {
        string name = suggestedName;
        int race = 0, gender = 0, face = 0, hair = 0, size = 0, job = 0, nation = 0;
        string message = "";

        // Rows by index, so the switch below reads as the screen does.
        const int NameRow = 0, RaceRow = 1, GenderRow = 2, FaceRow = 3, HairRow = 4,
                  SizeRow = 5, JobRow = 6, NationRow = 7, CreateRow = 8;

        screens.Aside(true);
        try
        {
            while (true)
            {
                (string raceLabel, FfxiRaceId male, FfxiRaceId female) = Races[race];
                bool hasGender = male != female;
                if (!hasGender)
                {
                    // Mithra are women and Galka are men, and the dropdown
                    // says which rather than offering a choice there is not.
                    gender = male == FfxiRaceId.Mithra ? 1 : 0;
                }
                FfxiRaceId raceId = gender == 0 ? male : female;
                byte faceValue = (byte)(face * 2 + hair);

                // The figure, as the choices currently describe them. Re-shown
                // rather than updated: the line-up is the one thing that
                // stands a look in the world, and one figure is a line-up of
                // one.
                world.ShowLineup([(PreviewId, name.Length > 0 ? name : "NEW CHARACTER",
                                   (ushort)raceId, faceValue, 0, (int)Sizes[size].Value)]);

                string[] raceLabels = [.. Races.Select(r => r.Label)];
                string[] genderOptions = hasGender ? Genders : [Genders[gender]];

                NativeFormRow[] rows =
                [
                    NativeFormRow.Field("NAME", name),
                    NativeFormRow.Choice("RACE", raceLabels, race),
                    NativeFormRow.Choice("GENDER", genderOptions, hasGender ? gender : 0, enabled: hasGender),
                    NativeFormRow.Choice("FACE", Faces, face),
                    NativeFormRow.Choice("HAIR", Hairs, hair),
                    NativeFormRow.Choice("SIZE", [.. Sizes.Select(s => s.Label)], size),
                    NativeFormRow.Choice("JOB", [.. Jobs.Select(j => j.Label)], job),
                    NativeFormRow.Choice("NATION", [.. Nations.Select(n => n.Label)], nation),
                    NativeFormRow.Button("CREATE"),
                    NativeFormRow.Button(Back),
                ];

                NativeFormResult? result = screens.Ask("CREATE A CHARACTER", message, rows);
                if (result is null)
                {
                    return null;
                }

                name = result[NameRow].Trim();
                message = "";

                switch (result.Button)
                {
                    case RaceRow:
                        race = Math.Clamp(result.Choice(RaceRow, race), 0, Races.Length - 1);
                        break;
                    case GenderRow when hasGender:
                        gender = Math.Clamp(result.Choice(GenderRow, gender), 0, 1);
                        break;
                    case FaceRow:
                        face = Math.Clamp(result.Choice(FaceRow, face), 0, Faces.Length - 1);
                        break;
                    case HairRow:
                        hair = Math.Clamp(result.Choice(HairRow, hair), 0, Hairs.Length - 1);
                        break;
                    case SizeRow:
                        size = Math.Clamp(result.Choice(SizeRow, size), 0, Sizes.Length - 1);
                        break;
                    case JobRow:
                        job = Math.Clamp(result.Choice(JobRow, job), 0, Jobs.Length - 1);
                        break;
                    case NationRow:
                        nation = Math.Clamp(result.Choice(NationRow, nation), 0, Nations.Length - 1);
                        break;

                    case CreateRow when FfxiCharacterCreation.WhyNameIsInvalid(name) is { } why:
                        message = why.ToUpperInvariant();
                        break;

                    case CreateRow:
                        return new FfxiNewCharacter(name, raceId, faceValue,
                                                    Sizes[size].Value, Jobs[job].Value,
                                                    Nations[nation].Value);

                    case GenderRow:
                        break;   // a race with one gender; nothing to change

                    default:
                        return null;   // Back
                }
            }
        }
        finally
        {
            // Both put back on every way out, so the next screen is centred
            // over a dimmed world and the figure is not left standing there.
            screens.Aside(false);
            world.HideLineup();
        }
    }
}
