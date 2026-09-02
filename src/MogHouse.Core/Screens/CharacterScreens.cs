using MogHouse.Core.Ffxi;
using MogHouse.Core.Interop;

namespace MogHouse.Core.Screens;

/// <summary>
/// Choosing who to play, and making one when there is nobody to choose.
/// </summary>
public static class CharacterScreens
{
    private const string Create = "CREATE A CHARACTER";
    private const string Quit = "QUIT";
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
    public static Choice? Select(ScreenHost screens, IReadOnlyList<FfxiCharacter> characters,
                                 string message = "")
    {
        List<FfxiCharacter> named =
            [.. characters.Where(c => !string.IsNullOrWhiteSpace(c.Name))];

        var rows = new List<NativeFormRow>();
        foreach (FfxiCharacter character in named)
        {
            // What tells two characters apart at a glance, which is what this
            // screen is for: who they are, what they are, and where they left off.
            rows.Add(NativeFormRow.Button(
                $"{character.Name}  {character.RaceName}  {character.JobSummary}  {character.ZoneName}".ToUpperInvariant()));
        }

        rows.Add(NativeFormRow.Button(Create));
        rows.Add(NativeFormRow.Button(Quit));

        if (named.Count == 0 && message.Length == 0)
        {
            message = "THIS ACCOUNT HAS NO CHARACTERS YET.";
        }

        NativeFormResult? result = screens.Ask("CHOOSE A CHARACTER", message, rows);
        if (result is null)
        {
            return null;
        }

        // By position rather than by label: two characters can be described the
        // same way, and a name is not unique across worlds.
        if (result.Button >= 0 && result.Button < named.Count)
        {
            return new Choice(named[result.Button], false);
        }

        return rows[result.Button].Text == Create ? Choice.New : null;
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
