namespace MogHouse.Core.Ffxi;

/// <summary>
/// Turning a race and a face into the look string the renderer takes.
/// </summary>
public static class FfxiAppearance
{
    /// <summary>
    /// What a character with nothing on is wearing: the starting clothes.
    ///
    /// Taken from the server's own answer rather than chosen. LandSandBoat's
    /// char_look defaults a new character to 8 in the body, hands, legs and
    /// feet and 0 in the head, and that is exactly what somebody who has just
    /// been created looks like.
    /// </summary>
    public const ushort StartingClothes = 8;

    /// <summary>
    /// Nothing worn at all, which the server sends for an empty slot.
    ///
    /// Checked against a live look: taking the body, hands and legs off turned
    /// all three to 0 while the feet stayed 8. Not what to draw when the gear
    /// is merely unknown, though - see below.
    /// </summary>
    public const ushort NothingWorn = 0;

    /// <summary>
    /// "race,face,head,body,hands,legs,feet" for a character whose gear is not
    /// known: the one being made on the creation screen, the one picked from
    /// the roster before the world has been entered.
    ///
    /// The starting clothes rather than nothing. Three tries at this:
    ///
    /// Model 1 in every slot dressed a character in an outfit nobody chose,
    /// which reads as wearing the wrong armour because that is what it is.
    /// Model 0 is honest - it is what the server sends for an empty slot - but
    /// the character creation screen then draws somebody with no hips and no
    /// legs, because 0 means "no mesh for this slot" and nothing else supplies
    /// one. The starting clothes are both: a real appearance, and the one the
    /// character will actually have when it exists.
    ///
    /// In the world this only shows for the moment between arriving and the
    /// server's first entity update, which now says what we really look like.
    /// </summary>
    public static string LookString(ushort race, ushort face, int size = 1) =>
        $"{race},{face},{NothingWorn},{StartingClothes},{StartingClothes},{StartingClothes}," +
        $"{StartingClothes},{Math.Clamp(size, 0, 2)}";
}
