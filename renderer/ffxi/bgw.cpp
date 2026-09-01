#include "bgw.h"

#include <algorithm>
#include <cstring>

namespace ffxi
{
namespace
{
/// The PlayStation's four ADPCM predictors, as sixty-fourths.
///
/// Not a guess: a file decoded with these comes out as a waveform, and the
/// shift and predictor nibbles in every block sit inside the range this table
/// is indexed by.
constexpr float kFilter0[5] = {0.0f, 60.0f / 64.0f, 115.0f / 64.0f, 98.0f / 64.0f, 122.0f / 64.0f};
constexpr float kFilter1[5] = {0.0f, 0.0f, -52.0f / 64.0f, -55.0f / 64.0f, -60.0f / 64.0f};

template <typename T> T readAt(const uint8_t* bytes, size_t offset)
{
    T value{};
    std::memcpy(&value, bytes + offset, sizeof(T));
    return value;
}
} // namespace

bool BgwStream::open(const std::filesystem::path& path)
{
    file_.open(path, std::ios::binary);
    if (!file_)
    {
        return false;
    }

    uint8_t header[48]{};
    file_.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!file_ || std::memcmp(header, "BGMStream", 9) != 0)
    {
        file_.close();
        return false;
    }

    track_ = readAt<uint32_t>(header, 0x14);
    blocks_ = readAt<uint32_t>(header, 0x18);
    loopBlock_ = readAt<uint32_t>(header, 0x1C);
    dataOffset_ = readAt<uint32_t>(header, 0x28);

    if (blocks_ == 0 || dataOffset_ < sizeof(header))
    {
        file_.close();
        return false;
    }

    // A loop past the end is no loop. Trusting it would seek beyond the file
    // and read silence for ever.
    if (loopBlock_ != kNoLoop && loopBlock_ >= blocks_)
    {
        loopBlock_ = kNoLoop;
    }

    nextBlock_ = 0;
    finished_ = false;
    pendingUsed_ = kSamplesPerBlock;
    channels_[0] = {};
    channels_[1] = {};
    return true;
}

bool BgwStream::decodeBlock()
{
    if (nextBlock_ >= blocks_)
    {
        if (loopBlock_ == kNoLoop)
        {
            finished_ = true;
            return false;
        }

        // Back to the loop point. The predictor history is deliberately kept:
        // the first block after a loop is a difference from the last two
        // samples before it, exactly as any other block is, and clearing it
        // puts a click in the seam.
        nextBlock_ = loopBlock_;
    }

    file_.seekg(dataOffset_ + static_cast<std::streamoff>(nextBlock_) * kBlockBytes);
    uint8_t block[kBlockBytes]{};
    file_.read(reinterpret_cast<char*>(block), sizeof(block));
    if (!file_)
    {
        finished_ = true;
        return false;
    }
    ++nextBlock_;

    for (int channel = 0; channel < kChannels; ++channel)
    {
        const uint8_t* half = block + channel * 9;
        int predictor = half[0] >> 4;
        const int shift = half[0] & 0x0F;
        if (predictor > 4)
        {
            predictor = 0;
        }

        Channel& state = channels_[channel];
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
                pending_[(byte * 2 + n) * kChannels + channel] = static_cast<int16_t>(sample);
            }
        }
    }

    pendingUsed_ = 0;
    return true;
}

size_t BgwStream::read(int16_t* out, size_t frames)
{
    size_t written = 0;
    while (written < frames)
    {
        if (pendingUsed_ >= kSamplesPerBlock)
        {
            if (!decodeBlock())
            {
                break;
            }
        }

        const size_t take = std::min(frames - written, kSamplesPerBlock - pendingUsed_);
        std::memcpy(out + written * kChannels, pending_ + pendingUsed_ * kChannels,
                    take * kChannels * sizeof(int16_t));
        pendingUsed_ += take;
        written += take;
    }
    return written;
}
} // namespace ffxi
