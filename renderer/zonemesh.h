#pragma once

// Turns MZB collision meshes into something a GPU can draw.

#include "ffxi/mzb.h"
#include "math.h"

#include <cstdint>
#include <vector>

namespace pj
{
struct Vertex
{
    float position[3];
    float normal[3];
};

struct ZoneMesh
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Vec3 boundsMin{};
    Vec3 boundsMax{};

    Vec3 centre() const { return (boundsMin + boundsMax) * 0.5f; }
    float radius() const;
};

/// Flattens every collision mesh in a zone into one buffer.
///
/// Normals are computed per face rather than taken from the file. MZB does
/// store normals, but there are fewer of them than either vertices or triangles
/// - 2,354 against 3,248 and 3,578 in one zone - so what they are indexed by is
/// still unknown. Face normals are correct, need no such answer, and give flat
/// shading that suits collision geometry anyway.
ZoneMesh buildZoneMesh(const ffxi::Zone& zone);
} // namespace pj
