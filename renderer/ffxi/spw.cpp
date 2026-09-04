#include "spw.h"

#include <algorithm>
#include <cstring>
#include <fstream>

namespace ffxi
{
namespace
{
// The four PlayStation ADPCM predictors, as in bgw.cpp - the same codec in a
// different container.
constexpr float kFilter0[5] = {0.0f, 0.9375f, 1.796875f, 1.53125f, 1.90625f};
constexpr float kFilter1[5] = {0.0f, 0.0f, -0.8125f, -0.859375f, -0.9375f};

constexpr size_t kHeaderBytes = 48;
constexpr size_t kSamplesPerBlock = 16;
constexpr uint32_t kNoLoop = 0xFFFFFFFFu;

template <typename T> T readAt(const uint8_t* from, size_t offset)
{
    T value{};
    std::memcpy(&value, from + offset, sizeof(T));
    return value;
}

/// One channel's running state. Each sample is a difference from the two
/// before it, so a channel cannot be decoded out of order.
struct Running
{
    float previous{};
    float beforeThat{};
};

void decodeBlock(const uint8_t* half, Running& state, int16_t* into, int stride)
{
    int predictor = half[0] >> 4;
    const int shift = half[0] & 0x0F;
    if (predictor > 4)
    {
        predictor = 0;
    }

    for (int byte = 0; byte < 8; ++byte)
    {
        // Low nibble first: the samples run in the order the nibbles are
        // written, not the order they read on screen.
        const int nibbles[2] = {half[1 + byte] & 0x0F, half[1 + byte] >> 4};
        for (int n = 0; n < 2; ++n)
        {
            const int signed4 = nibbles[n] > 7 ? nibbles[n] - 16 : nibbles[n];
            float sample = static_cast<float>(signed4 << (12 - shift)) +
                           kFilter0[predictor] * state.previous + kFilter1[predictor] * state.beforeThat;
            sample = std::clamp(sample, -32768.0f, 32767.0f);

            state.beforeThat = state.previous;
            state.previous = sample;
            into[(static_cast<size_t>(byte) * 2 + n) * stride] = static_cast<int16_t>(sample);
        }
    }
}
} // namespace

std::optional<SpwSound> loadSpw(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        return std::nullopt;
    }

    uint8_t header[kHeaderBytes]{};
    file.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!file || std::memcmp(header, "SeWave", 6) != 0)
    {
        return std::nullopt;
    }

    const uint32_t count = readAt<uint32_t>(header, 0x14);
    const uint32_t loop = readAt<uint32_t>(header, 0x18);
    const uint32_t dataOffset = readAt<uint32_t>(header, 0x24);
    const int channels = header[0x2A];

    // Split in two and summed, wrapping, exactly as the music does it. Nobody
    // knows why; it is not a checksum, because either half is free.
    const uint32_t rate = readAt<uint32_t>(header, 0x1C) + readAt<uint32_t>(header, 0x20);

    if (count == 0 || dataOffset < kHeaderBytes || channels < 1 || channels > 2)
    {
        return std::nullopt;
    }

    file.seekg(0, std::ios::end);
    const auto size = static_cast<uint64_t>(file.tellg());
    if (size <= dataOffset)
    {
        return std::nullopt;
    }
    const uint64_t payload = size - dataOffset;

    std::vector<uint8_t> data(static_cast<size_t>(payload));
    file.seekg(static_cast<std::streamoff>(dataOffset));
    file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(payload));
    if (!file)
    {
        return std::nullopt;
    }

    SpwSound sound;
    sound.channels = channels;
    sound.sampleRate = (rate >= 8000 && rate <= 192000) ? rate : 48000;

    // Which format this is, decided by division rather than by a flag. It
    // either divides exactly or the file is not what it looked like.
    const uint64_t adpcmBytes = static_cast<uint64_t>(count) * 9 * channels;
    const uint64_t pcmBytes = static_cast<uint64_t>(count) * 2 * channels;

    if (payload == adpcmBytes)
    {
        sound.samples.assign(static_cast<size_t>(count) * kSamplesPerBlock * channels, 0);
        Running running[2]{};
        for (uint32_t block = 0; block < count; ++block)
        {
            for (int channel = 0; channel < channels; ++channel)
            {
                // Stereo interleaves by block, not by sample: nine bytes of
                // left then nine of right.
                const uint8_t* half = data.data() + (static_cast<size_t>(block) * channels + channel) * 9;
                int16_t* into = sound.samples.data() +
                                static_cast<size_t>(block) * kSamplesPerBlock * channels + channel;
                decodeBlock(half, running[channel], into, channels);
            }
        }

        if (loop != kNoLoop && loop < count)
        {
            sound.loopFrame = loop * static_cast<uint32_t>(kSamplesPerBlock);
        }
    }
    else if (payload == pcmBytes)
    {
        // Already samples. The count is frames here rather than blocks, which
        // is the one place these two readings of the same field differ.
        sound.samples.assign(static_cast<size_t>(count) * channels, 0);
        std::memcpy(sound.samples.data(), data.data(), static_cast<size_t>(pcmBytes));
        if (loop != kNoLoop && loop < count)
        {
            sound.loopFrame = loop;
        }
    }
    else
    {
        // Neither reading fits. About a tenth of the library is like this and
        // is not yet understood - see docs/wiki/Audio-Formats.md. Refused
        // rather than played as noise.
        return std::nullopt;
    }

    return sound;
}
} // namespace ffxi
