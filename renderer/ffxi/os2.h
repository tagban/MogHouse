#pragma once

// OS2 - skinned meshes. Chunk type 0x2A. See docs/os2-format.md.

#include "dat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ffxi
{
/// One bone's contribution to a vertex.
///
/// The position is in that bone's own space, not the model's, so skinning is a
/// weighted sum of the bone transforms applied to their own copy of the point.
struct SkinInfluence
{
    float position[3]{};
    float normal[3]{};
    float weight{};
    uint8_t bone{};
    uint8_t boneMirror{};
    uint8_t mirrorAxis{};
};

struct SkinVertex
{
    SkinInfluence influence[2];
    uint8_t influences{1};
};

/// A triangle corner. UVs live in the draw stream rather than on the vertex,
/// so one vertex can appear with several different UVs and the corners have to
/// be kept whole.
struct SkinCorner
{
    uint16_t vertex{};
    float uv[2]{};
};

/// One run of triangles sharing a texture.
struct SkinnedPart
{
    std::string texture;
    float specularExponent{};
    float specularIntensity{};
    std::vector<SkinCorner> corners; ///< three per triangle, always a list
};

struct SkinnedModel
{
    std::string name; ///< the chunk's four-character id
    std::vector<SkinVertex> vertices;
    std::vector<SkinnedPart> parts;

    /// The mesh is half a body; the other half comes from reflecting each
    /// vertex through its mirror bone. Not applied here.
    bool mirrored{};

    size_t triangleCount() const;
};

/// Parses an OS2 chunk. Throws std::runtime_error if the header's offsets do
/// not stay inside the chunk or the draw stream contains a command we cannot
/// size.
SkinnedModel parseOs2(const Chunk& chunk);
} // namespace ffxi
