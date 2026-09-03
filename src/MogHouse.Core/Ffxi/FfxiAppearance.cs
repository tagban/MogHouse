namespace MogHouse.Core.Ffxi;

/// <summary>
/// Turning a race and a face into the look string the renderer takes.
/// </summary>
public static class FfxiAppearance
{
    /// <summary>
    /// The equipment model id to use when the real one is not known.
    ///
    /// Not zero. Zero is a real id pointing at a real file, and for several
    /// slots that file has nothing in it - a character built from zeroes comes
    /// out with no hips, no hands and a pair of boots floating where the legs
    /// should be. One is a whole set of clothes.
    ///
    /// This matters wherever gear is unknown rather than empty, which is our
    /// own character - the server never sends us the entity update it sends
    /// for everyone else - and any character being previewed before it exists.
    /// </summary>
    public const ushort UnknownGear = 1;

    /// <summary>
    /// "race,face,head,body,hands,legs,feet" with every unknown slot filled in.
    /// </summary>
    public static string LookString(ushort race, ushort face, int size = 1) =>
        $"{race},{face},{UnknownGear},{UnknownGear},{UnknownGear},{UnknownGear},{UnknownGear},{Math.Clamp(size, 0, 2)}";
}
