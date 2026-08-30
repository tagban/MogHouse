#pragma once

#include "dat.h"

#include <cstdint>
#include <vector>

namespace ffxi
{
/// One joint. The transform is relative to the parent.
struct Bone
{
    uint8_t parent{}; ///< index into the bone list; the root points at itself
    uint8_t flags{};  ///< 0 or 1; meaning not yet established
    float rotation[4]{};    ///< quaternion, x y z w
    float translation[3]{};
};

/// A named slot on the skeleton that something else can be hung from -
/// equipment, weapon trails, spell effects. The index in the list is the
/// slot number the rest of the game refers to.
struct GeneratorPoint
{
    uint16_t bone{};
    float offset[3]{};
};

struct Skeleton
{
    std::vector<Bone> bones;
    std::vector<GeneratorPoint> generatorPoints;

    /// True if every parent is in range and the parent chain terminates.
    bool isWellFormed() const;
};

/// Parses an SK2 chunk. Throws if the chunk is too small or the counts are
/// implausible; a malformed skeleton is never returned half-built.
Skeleton parseSkeleton(const Chunk& chunk);

inline constexpr uint8_t kChunkSkeleton = 0x29;
inline constexpr uint8_t kChunkSkinnedMesh = 0x2A;
inline constexpr uint8_t kChunkAnimation = 0x2B;
} // namespace ffxi
