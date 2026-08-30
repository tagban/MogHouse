#pragma once

// MO2 - skeletal animation. Chunk type 0x2B. See docs/mo2-format.md.

#include "dat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ffxi
{
/// One bone's movement over the whole animation, already expanded to one
/// entry per frame.
///
/// The file stores each of the ten channels either as a constant or as a run
/// of floats in a shared pool, which saves space and makes nothing easier to
/// reason about downstream. Expanding on read costs a few kilobytes.
struct AnimationTrack
{
    uint32_t bone{};
    std::vector<float> rotation;    ///< four per frame, x y z w
    std::vector<float> translation; ///< three per frame
    std::vector<float> scale;       ///< three per frame
};

struct Animation
{
    std::string name;   ///< the chunk's four-character id
    uint16_t frames{};
    float speed{};      ///< multiplier on 30 frames a second
    std::vector<AnimationTrack> tracks;

    /// How long one frame lasts, in seconds.
    float frameSeconds() const;
};

/// Parses one MO2 chunk. Throws std::runtime_error if the channel indices
/// reach outside the chunk.
Animation parseMo2(const Chunk& chunk);
} // namespace ffxi
