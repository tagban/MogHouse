namespace MogHouse.Core.Ffxi;

/// <summary>
/// Where a track number's audio lives on disk.
///
/// The server never names a piece of music; it sends a number per slot, in the
/// zone login reply and again in GP_SERV_COMMAND_MUSIC whenever the situation
/// changes. The number is the file: track 151 is music151.bgw, which is
/// Windurst Waters, and 212 is what plays on a chocobo.
///
/// Which directory holds it is not fixed. The install spreads them across
/// sound, sound2, sound3 and so on as expansions were added - 151 is in the
/// first and 212 in the second - so finding one means looking through them
/// rather than computing a path.
/// </summary>
public static class FfxiMusicFile
{
    /// <summary>How many sound directories to look through. Installs vary.</summary>
    private const int SoundDirectories = 10;

    /// <summary>
    /// The .bgw for a track number, or null if this install does not have it.
    /// </summary>
    public static string? Resolve(string installRoot, int track)
    {
        if (track <= 0)
        {
            return null;
        }

        string name = $"music{track:D3}.bgw";
        for (int i = 0; i <= SoundDirectories; i++)
        {
            string directory = i == 0 ? "sound" : $"sound{i + 1}";
            string path = Path.Combine(installRoot, directory, "win", "music", "data", name);
            if (File.Exists(path))
            {
                return path;
            }
        }

        return null;
    }
}
