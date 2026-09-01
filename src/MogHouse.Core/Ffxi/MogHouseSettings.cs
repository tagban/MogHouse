using System.Text.Json;
using System.Text.Json.Serialization;

namespace MogHouse.Core.Ffxi;

/// <summary>
/// The preferences a player sets while playing, kept between sessions.
///
/// Beside the executable rather than in %AppData%, the same as the server
/// profiles: this is meant to run unpacked from a zip, with no installer and
/// nothing left behind on a machine it was tried on once.
///
/// Everything here has a working default, and a file that will not parse is
/// ignored rather than fatal. A client that refuses to start because a
/// preferences file is malformed has turned a cosmetic problem into a broken
/// install.
/// </summary>
public sealed class MogHouseSettings
{
    /// <summary>0 silent, 1 as recorded.</summary>
    [JsonPropertyName("musicVolume")]
    public float MusicVolume { get; set; } = 0.35f;

    /// <summary>Whether the minimap turns with the player or holds north up.</summary>
    [JsonPropertyName("radarTurnsWithPlayer")]
    public bool RadarTurnsWithPlayer { get; set; } = true;

    public static string FilePath =>
        Path.Combine(Path.GetDirectoryName(FfxiServerProfileStore.FilePath)!, "moghouse-settings.json");

    private static readonly JsonSerializerOptions JsonOptions = new() { WriteIndented = true };

    private static MogHouseSettings? _cache;

    /// <summary>What is on disk, or the defaults.</summary>
    public static MogHouseSettings Current => _cache ??= LoadFromDisk();

    /// <summary>
    /// Writes the current settings out. Failures are swallowed on purpose - a
    /// read-only folder is a reason not to remember the volume, not a reason
    /// to stop playing.
    /// </summary>
    public void Save()
    {
        _cache = this;
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, JsonOptions));
        }
        catch (Exception)
        {
        }
    }

    private static MogHouseSettings LoadFromDisk()
    {
        try
        {
            if (File.Exists(FilePath) &&
                JsonSerializer.Deserialize<MogHouseSettings>(File.ReadAllText(FilePath)) is { } loaded)
            {
                loaded.MusicVolume = Math.Clamp(loaded.MusicVolume, 0.0f, 1.0f);
                return loaded;
            }
        }
        catch (Exception)
        {
        }
        return new MogHouseSettings();
    }
}
