namespace MogHouse.Core.Ffxi;

/// <summary>
/// How an entity's appearance is described - look_t's `size` field, which is
/// a kind rather than a length.
/// </summary>
public enum FfxiLookKind : ushort
{
    /// <summary>A fixed model id. Most scenery and creature NPCs.</summary>
    Standard = 0,

    /// <summary>Race, face and equipment, exactly as a player character is.</summary>
    Equipped = 1,

    /// <summary>A door. Carries a door id or a name, not a model.</summary>
    Door = 2,

    /// <summary>Named transport rather than a model.</summary>
    Elevator = 3,

    /// <summary>Named transport rather than a model.</summary>
    Ship = 4,

    Unknown5 = 5,
    Automaton = 6,

    /// <summary>Race and equipment, like <see cref="Equipped"/>.</summary>
    Chocobo = 7,
}

/// <summary>
/// What an entity looks like, as the server describes it - the `look_t` at
/// offset 0x30 of an entity update.
///
/// FFXI has two quite different ways of saying what something looks like, and
/// which one applies is the first field. A shopkeeper is described the same way
/// a player is - a race and a set of equipment model ids - and can be built
/// from the same files. A crab, a door, a bookshelf is a single model id
/// pointing somewhere else entirely.
///
/// This is also where invisibility lives in practice: an auction counter is a
/// real entity with a real position that the game never draws.
/// </summary>
public sealed record FfxiEntityLook(FfxiLookKind Kind, ushort ModelId, byte Race, byte Face,
                                    ushort Head, ushort Body, ushort Hands, ushort Legs, ushort Feet,
                                    ushort Main, ushort Sub, ushort Ranged)
{
    /// <summary>
    /// Whether this is the race-and-equipment form, which the character loader
    /// already knows how to build.
    /// </summary>
    public bool IsEquipment => Kind is FfxiLookKind.Equipped or FfxiLookKind.Chocobo;

    /// <summary>
    /// Whether this names a single model rather than a set of pieces. The id
    /// alone is not enough to find the file - that mapping is its own problem.
    /// </summary>
    public bool IsFixedModel => Kind is FfxiLookKind.Standard or FfxiLookKind.Unknown5 or FfxiLookKind.Automaton;

    /// <summary>The seven numbers the character loader takes, in its order.</summary>
    public string ToLookString() =>
        $"{Race},{Face},{Head},{Body},{Hands},{Legs},{Feet}";
}
