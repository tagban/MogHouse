namespace MogHouse.Core.Ffxi;

/// <summary>
/// The weather a zone is under, as the server numbers it.
///
/// <para>
/// Taken from the server's own list rather than guessed - LandSandBoat's
/// <c>zoneutils::GetWeatherElement</c> enumerates all twenty in this order,
/// and casts the value to <c>uint16_t</c>, which is what fixes the width of
/// the field in both packets that carry it.
/// </para>
///
/// <para>
/// They come in pairs, a mild one and its severe form - rain and squall, snow
/// and blizzards - because the pair shares an element and the severe one is
/// what a weather-sensitive spell keys off. The sky does not have twenty
/// looks to match: a zone's DAT ships four, <c>suny</c>, <c>fine</c>,
/// <c>clod</c> and <c>mist</c>, so several of these necessarily share one.
/// Which shares which is the renderer's business and is not settled here.
/// </para>
/// </summary>
public enum FfxiWeather : ushort
{
    None = 0,
    Sunshine = 1,
    Clouds = 2,
    Fog = 3,
    HotSpell = 4,
    HeatWave = 5,
    Rain = 6,
    Squall = 7,
    DustStorm = 8,
    SandStorm = 9,
    Wind = 10,
    Gales = 11,
    Snow = 12,
    Blizzards = 13,
    Thunder = 14,
    Thunderstorms = 15,
    Auroras = 16,
    StellarGlare = 17,
    Gloom = 18,
    Darkness = 19,
}
