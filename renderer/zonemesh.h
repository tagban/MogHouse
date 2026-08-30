#pragma once

// Turns MZB collision meshes into something a GPU can draw.

#include "ffxi/mmb.h"
#include "ffxi/mzb.h"
#include "math.h"

#include <cstdint>
#include <string>
#include <unordered_map>
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

/// Builds the visible world: every MZB placement resolved to its MMB model,
/// transformed into place.
///
/// Transforms are baked into the vertices rather than instanced. A zone is a
/// few million vertices that way, which is fine for a static buffer, and it
/// reuses the collision pipeline unchanged. Instancing is the obvious next step
/// but not a prerequisite for seeing whether the placement maths is right.
ZoneMesh buildPlacedMesh(const ffxi::Zone& zone, const std::unordered_map<std::string, ffxi::Model>& models,
                         size_t& placementsResolved, size_t& placementsMissing);
} // namespace pj
