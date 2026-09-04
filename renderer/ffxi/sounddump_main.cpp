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
//   ffxi-sounddump --wav <out-dir> <file.spw>...   so they can be listened to
//
// The last one exists because which sound belongs to which event is not
// written down anywhere that has been found - not in the headers, not in the
// animations, not in the index beside them - and is most likely compiled into
// the retail client. That cannot be derived, but it can be recognised: turn a
// folder into .wav and the noise a worm makes is obvious to anyone who has
// played the game.

#include "spw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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

/// A 44-byte RIFF header and the samples. Nothing here needs an encoder: the
/// decode is already PCM16, which is what a .wav holds.
bool writeWav(const std::filesystem::path& path, const ffxi::SpwSound& sound)
{
    std::ofstream out{path, std::ios::binary};
    if (!out)
    {
        return false;
    }

    const uint32_t dataBytes = static_cast<uint32_t>(sound.samples.size() * sizeof(int16_t));
    const uint16_t channels = static_cast<uint16_t>(sound.channels);
    const uint32_t byteRate = sound.sampleRate * channels * 2;
    const uint16_t blockAlign = static_cast<uint16_t>(channels * 2);

    auto u32 = [&out](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&out](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };

    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);                 // PCM
    u16(channels);
    u32(sound.sampleRate);
    u32(byteRate);
    u16(blockAlign);
    u16(16);                // bits per sample
    out.write("data", 4);
    u32(dataBytes);
    out.write(reinterpret_cast<const char*>(sound.samples.data()), dataBytes);
    return static_cast<bool>(out);
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: ffxi-sounddump <file.spw>...\n"
                    "       ffxi-sounddump --sweep <directory>\n"
                    "       ffxi-sounddump --wav <out-dir> <file.spw>...\n");
        return 1;
    }

    if (std::strcmp(argv[1], "--wav") == 0 && argc >= 4)
    {
        const std::filesystem::path out{argv[2]};
        std::error_code ignored;
        std::filesystem::create_directories(out, ignored);

        size_t written = 0;
        size_t refused = 0;
        for (int i = 3; i < argc; ++i)
        {
            const std::filesystem::path from{argv[i]};
            const std::optional<ffxi::SpwSound> sound = ffxi::loadSpw(from);
            if (!sound || !writeWav(out / (from.stem().string() + ".wav"), *sound))
            {
                ++refused;
                continue;
            }
            ++written;
        }
        std::printf("wrote %zu .wav, refused %zu, into %s\n", written, refused, out.string().c_str());
        return 0;
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
