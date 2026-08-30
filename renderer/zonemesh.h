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

/// Flattens a zone's collision geometry into one buffer, placed in the world.
///
/// Meshes are model space and reused, so this walks the instances rather than
/// the meshes - drawing the meshes directly stacks every one of them on the
/// origin. FFXI's Y axis points down, so it is flipped here and everything
/// downstream can assume Y is up.
///
/// Normals are computed per face rather than taken from the file. MZB does
/// store normals, but there are fewer of them than either vertices or triangles
/// - 2,354 against 3,248 and 3,578 in one zone - so what they are indexed by is
/// still unknown. Face normals are correct, need no such answer, and give flat
/// shading that suits collision geometry anyway.
ZoneMesh buildZoneMesh(const ffxi::Zone& zone);
} // namespace pj
