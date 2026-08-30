#pragma once

// Per-zone lighting, sky and fog, varying with the time of day.
//
// Chunk type 0x2F. The chunk's four-character id is the time it applies to on
// FFXI's 1440-minute clock. See docs/lighting-format.md.

#include "dat.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ffxi
{
struct Colour
{
    float r{}, g{}, b{}, a{};
};

/// One lighting set. Entities and landscape are lit separately - characters and
/// scenery do not share a light.
struct LightingSet
{
    int minutes{}; ///< time of day this applies to, 0..1439

    Colour sunlight;
    Colour moonlight;
    Colour ambient;
    Colour fog;
    float minFog{};
    float maxFog{};
    float brightness{1.0f};

    Colour landscapeSunlight;
    Colour landscapeMoonlight;
    Colour landscapeAmbient;
    Colour landscapeFog;
    float landscapeMinFog{};
    float landscapeMaxFog{};
    float landscapeBrightness{1.0f};

    Colour fogColour;
    float fogOffset{};
    float maxFarClip{};

    std::array<Colour, 8> skyColours{};
    std::array<float, 8> skyAltitudes{};
};

/// Every lighting set a zone carries, in time order.
class Lighting
{
public:
    void add(const Chunk& chunk);

    bool empty() const { return sets_.empty(); }
    const std::vector<LightingSet>& sets() const { return sets_; }

    /// The lighting at a given time, interpolated between the two surrounding
    /// entries. Wraps across midnight.
    LightingSet at(int minutes) const;

private:
    std::vector<LightingSet> sets_;
};

/// Chunk type holding a lighting set.
inline constexpr uint8_t kChunkLighting = 0x2F;
} // namespace ffxi
