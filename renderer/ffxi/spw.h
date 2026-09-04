#pragma once

// FFXI's sound effects.
//
// A .spw is the same container as a .bgw with every field four bytes earlier -
// its magic is "SeWave" where the music's is "BGMStream" - and usually the
// same Sony ADPCM inside. See docs/wiki/Audio-Formats.md for the header and
// how it was read.
//
// Decoded whole rather than streamed, which is the difference that matters
// here. Music is eight megabytes and plays once; an effect is a second of
// noise that may play forty times in a fight, and reading it off disk each
// time to save a hundred kilobytes is the wrong trade.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace ffxi
{
/// One sound effect, decoded.
struct SpwSound
{
    /// Interleaved when there are two channels.
    std::vector<int16_t> samples;

    uint32_t sampleRate{48000};
    int channels{1};

    /// Where to go back to, in frames, or absent when it plays once. Twenty-two
    /// of six hundred effects loop - a torch, a fountain, a waterfall.
    std::optional<uint32_t> loopFrame;

    /// Frames, not samples: a stereo sound has half as many as it has samples.
    size_t frames() const { return channels > 0 ? samples.size() / static_cast<size_t>(channels) : 0; }
};

/// Reads and decodes one .spw, or nothing if it is not one this understands.
///
/// The format is worked out by division rather than read from a flag: the
/// payload divided by the header's count is 9 or 18 for ADPCM and 2 or 4 for
/// PCM16, and anything else is a file this does not know. That is
/// self-checking in a way a flag is not - the byte at +0x29 looks like a codec
/// field and is not one.
std::optional<SpwSound> loadSpw(const std::filesystem::path& path);
} // namespace ffxi
