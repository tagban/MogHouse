// Decodes sound effects and says whether the result is a waveform.
//
// The sound formats were read from the files rather than from documentation,
// so "it parsed" is not evidence of anything - a wrong reading parses happily
// and produces noise. This measures the same thing the music decoder was
// checked against: autocorrelation at one sample. Audio is strongly correlated
// with itself a sample later; white noise is not. Anything above about 0.9 is
// a waveform, and anything near zero means the bytes were read wrongly.
//
//   ffxi-sounddump <file.spw>...
//   ffxi-sounddump --sweep <directory>     every .spw under it, as a summary

#include "spw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
double autocorrelation(const std::vector<int16_t>& samples, int channels)
{
    if (samples.size() < static_cast<size_t>(channels) * 4)
    {
        return 0.0;
    }

    // One channel only. Interleaved stereo compared against itself a sample
    // later is comparing left with right, which measures nothing.
    double sum = 0.0;
    double energy = 0.0;
    for (size_t i = static_cast<size_t>(channels); i < samples.size(); i += static_cast<size_t>(channels))
    {
        const double now = samples[i];
        const double before = samples[i - static_cast<size_t>(channels)];
        sum += now * before;
        energy += before * before;
    }
    return energy > 0.0 ? sum / energy : 0.0;
}

void report(const std::filesystem::path& path)
{
    const std::optional<ffxi::SpwSound> sound = ffxi::loadSpw(path);
    if (!sound)
    {
        std::printf("%-28s could not be read\n", path.filename().string().c_str());
        return;
    }

    const double seconds = sound->sampleRate > 0
                               ? static_cast<double>(sound->frames()) / sound->sampleRate
                               : 0.0;
    int16_t loudest = 0;
    for (int16_t s : sound->samples)
    {
        loudest = std::max(loudest, static_cast<int16_t>(s < 0 ? -s : s));
    }

    std::printf("%-28s %5u Hz  %dch  %7zu frames  %5.2fs  peak %6d  r=%.3f%s\n",
                path.filename().string().c_str(), sound->sampleRate, sound->channels,
                sound->frames(), seconds, loudest,
                autocorrelation(sound->samples, sound->channels),
                sound->loopFrame ? "  loops" : "");
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: ffxi-sounddump <file.spw>...\n"
                    "       ffxi-sounddump --sweep <directory>\n");
        return 1;
    }

    if (std::strcmp(argv[1], "--sweep") == 0 && argc >= 3)
    {
        size_t read = 0;
        size_t refused = 0;
        size_t waveform = 0;
        double worst = 1.0;
        std::string worstName;

        for (const auto& entry : std::filesystem::recursive_directory_iterator{argv[2]})
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".spw")
            {
                continue;
            }

            const std::optional<ffxi::SpwSound> sound = ffxi::loadSpw(entry.path());
            if (!sound)
            {
                ++refused;
                continue;
            }
            ++read;

            const double r = autocorrelation(sound->samples, sound->channels);
            if (r > 0.9)
            {
                ++waveform;
            }
            else if (r < worst)
            {
                worst = r;
                worstName = entry.path().filename().string();
            }
        }

        std::printf("read %zu, refused %zu\n", read, refused);
        std::printf("of those read, %zu are a waveform at r>0.9 (%.1f%%)\n", waveform,
                    read ? 100.0 * static_cast<double>(waveform) / static_cast<double>(read) : 0.0);
        if (!worstName.empty())
        {
            std::printf("least correlated: %s at r=%.3f\n", worstName.c_str(), worst);
        }
        return 0;
    }

    for (int i = 1; i < argc; ++i)
    {
        report(argv[i]);
    }
    return 0;
}
