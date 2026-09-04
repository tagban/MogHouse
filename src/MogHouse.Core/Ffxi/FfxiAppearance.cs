namespace MogHouse.Core.Ffxi;

/// <summary>
/// Turning a race and a face into the look string the renderer takes.
/// </summary>
public static class FfxiAppearance
{
    /// <summary>
    /// The equipment model id to use when the real one is not known.
    ///
    /// Zero, which is what the server itself sends for a slot with nothing in
    /// it - checked against a live look, where taking the body, hands and legs
    /// off turned all three to 0 while the feet stayed 8.
    ///
    /// It used to be 1, on the grounds that zero drew a character with no hips
    /// and no hands. One is a whole set of clothes, so a character whose gear
    /// was unknown was dressed in an outfit nobody chose - which reads as
    /// wearing the wrong armour, because that is exactly what it is.
    ///
    /// This is only ever seen where the gear is genuinely unknown: a character
    /// being previewed before it exists, and the moment between entering the
    /// world and the server saying what we look like. That second case used to
    /// last forever, because the client never read its own look; it now lasts
    /// until the first entity update.
    /// </summary>
    public const ushort UnknownGear = 0;

    /// <summary>
    /// "race,face,head,body,hands,legs,feet" with every unknown slot filled in.
    /// </summary>
    public static string LookString(ushort race, ushort face, int size = 1) =>
        $"{race},{face},{UnknownGear},{UnknownGear},{UnknownGear},{UnknownGear},{UnknownGear},{Math.Clamp(size, 0, 2)}";
}
