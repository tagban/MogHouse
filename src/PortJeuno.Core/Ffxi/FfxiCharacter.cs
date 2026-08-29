namespace PortJeuno.Core.Ffxi;

/// <summary>
/// One character slot decoded from a LandSandBoat lpkt_chr_info2 packet
/// (login_packets.h's lpkt_chr_info_sub2 / TC_OPERATION_MAKE). Only the
/// fields useful for a login/roster screen are exposed here - the full
/// struct carries far more (fame, gold, chat/party counters, etc.) that a
/// client that only lists characters and logs in doesn't need.
/// </summary>
/// <param name="Race">
/// `mon_no` - the base model, which encodes race and sex together. See
/// <see cref="FfxiRace"/>.
/// </param>
/// <param name="Face">`face_no`. Faces are numbered per race, so it only means anything alongside <paramref name="Race"/>.</param>
/// <param name="Hair">`hair_no`.</param>
/// <param name="Size">`size` - model scale: 0 small, 1 medium, 2 large.</param>
/// <param name="Equipment">
/// `GrapIDTbl` - eight visible equipment model ids. Enough to know what a
/// character is wearing; drawing it would need the model files themselves.
/// </param>
public sealed record FfxiCharacter(
    uint ContentId,
    ushort CharIdMain,
    string Name,
    string WorldName,
    ushort Race,
    byte MainJob,
    byte MainJobLevel,
    byte SubJob,
    byte Zone,
    bool CanRename,
    bool EligibleForRaceChange,
    ushort Face = 0,
    byte Hair = 0,
    byte Size = 0,
    IReadOnlyList<ushort>? Equipment = null)
{
    public string RaceName => FfxiRace.Name(Race);
    public string JobName => FfxiJob.Abbreviation(MainJob);
    public string SubJobName => FfxiJob.Abbreviation(SubJob);

    /// <summary>"WAR40" or "WAR40/WHM20" - the usual shorthand.</summary>
    public string JobSummary => SubJob == 0
        ? $"{JobName}{MainJobLevel}"
        : $"{JobName}{MainJobLevel}/{SubJobName}";

    /// <summary>Colour for a generated avatar, keyed off race.</summary>
    public string AvatarColour => FfxiRace.Colour(Race);

    /// <summary>First letter of the name, for a generated avatar.</summary>
    public string Initial => Name.Length > 0 ? Name[..1].ToUpperInvariant() : "?";

    /// <summary>The zone the character logged out in, by name where we know it.</summary>
    public string ZoneName => FfxiZoneNames.Get(Zone) ?? $"zone {Zone}";

    /// <summary>Face and hair ids. Numbered per race, so only meaningful next to the race.</summary>
    public string FaceHair => $"face {Face} hair {Hair}";
}

/// <summary>
/// FFXI's eight playable race/sex combinations. The protocol calls this
/// `mon_no` and uses the same numbering for player models.
/// </summary>
public static class FfxiRace
{
    public static string Name(ushort race) => race switch
    {
        1 => "Hume M",
        2 => "Hume F",
        3 => "Elvaan M",
        4 => "Elvaan F",
        5 => "Tarutaru M",
        6 => "Tarutaru F",
        7 => "Mithra",
        8 => "Galka",
        _ => $"race {race}",
    };

    /// <summary>
    /// A stable colour per race, so a generated avatar is recognisable at a
    /// glance. Purely cosmetic - nothing in the protocol assigns colours.
    /// </summary>
    public static string Colour(ushort race) => race switch
    {
        1 => "#5C8AC6",
        2 => "#7BA7D9",
        3 => "#63A66B",
        4 => "#86C48D",
        5 => "#D6A94E",
        6 => "#E3C173",
        7 => "#C97BA8",
        8 => "#B06A4F",
        _ => "#8D8D8D",
    };
}

/// <summary>FFXI job ids, as they appear in the roster and job-info packets.</summary>
public static class FfxiJob
{
    private static readonly string[] Abbreviations =
    [
        "---", "WAR", "MNK", "WHM", "BLM", "RDM", "THF", "PLD", "DRK",
        "BST", "BRD", "RNG", "SAM", "NIN", "DRG", "SMN", "BLU", "COR",
        "PUP", "DNC", "SCH", "GEO", "RUN",
    ];

    public static string Abbreviation(byte job) =>
        job < Abbreviations.Length ? Abbreviations[job] : $"job{job}";
}
