#pragma once

// FFXI's music files.
//
// A .bgw is a 48-byte header and then Sony ADPCM, the same four-coefficient
// scheme the PlayStation used, in 18-byte stereo blocks: nine bytes a channel,
// one of which is the predictor and shift, so sixteen samples a channel a
// block. 44100 Hz.
//
// The block size is what gives the format away. Every file's payload is
// exactly `count * 18` bytes for the count in its header, the two nine-byte
// halves have first bytes that track each other the way correlated stereo
// does, and splitting those bytes as PS-ADPCM does - predictor in the high
// nibble, shift in the low - puts every one inside the valid range. Decoded on
// that assumption the result has an autocorrelation of 0.995 at one sample,
// which is a waveform rather than noise.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

namespace ffxi
{
/// One music file, decoded on demand rather than all at once.
///
/// A zone theme is around eight megabytes of ADPCM and thirty of samples;
/// holding the samples costs more than reading the file as it plays, and the
/// audio callback wants a few thousand at a time regardless.
class BgwStream
{
public:
    static constexpr uint32_t kSampleRate = 44100;
    static constexpr int kChannels = 2;

    /// Opens a .bgw, or returns false if it is not one.
    bool open(const std::filesystem::path& path);

    /// Fills `out` with interleaved stereo samples, looping if the file says
    /// to and stopping if it does not. Returns how many frames were written.
    size_t read(int16_t* out, size_t frames);

    bool loops() const { return loopBlock_ != kNoLoop; }
    bool finished() const { return finished_; }
    uint32_t track() const { return track_; }

private:
    static constexpr uint32_t kNoLoop = 0xFFFFFFFFu;
    static constexpr size_t kBlockBytes = 18;
    static constexpr size_t kSamplesPerBlock = 16;

    /// One channel's running state. ADPCM is a difference from the two samples
    /// before it, so decoding cannot start anywhere but the beginning - which
    /// is why looping seeks to a block boundary and keeps the history.
    struct Channel
    {
        float previous{};
        float beforeThat{};
    };

    bool decodeBlock();

    std::ifstream file_;
    uint32_t track_{};
    uint32_t blocks_{};
    uint32_t loopBlock_{kNoLoop};
    uint32_t dataOffset_{};
    uint32_t nextBlock_{};
    bool finished_{};

    Channel channels_[kChannels]{};
    int16_t pending_[kSamplesPerBlock * kChannels]{};
    size_t pendingUsed_{kSamplesPerBlock};
};
} // namespace ffxi
