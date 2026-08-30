#pragma once

// Turns FFXI zone data into something a GPU can draw.

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
    float uv[2];
};

/// A run of indices sharing one texture. Geometry is grouped by material so
/// each can be drawn with its own binding - WebGPU has no bindless arrays, so
/// one draw per texture is the shape available to us.
struct Batch
{
    std::string texture; ///< empty means untextured, e.g. collision geometry
    uint32_t indexOffset{};
    uint32_t indexCount{};
};

struct ZoneMesh
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Batch> batches;
    Vec3 boundsMin{};
    Vec3 boundsMax{};

    Vec3 centre() const { return (boundsMin + boundsMax) * 0.5f; }
    float radius() const;
};

/// Collision geometry, as one untextured batch.
///
/// Normals are computed per face rather than read from the file. MZB stores
/// normals but there are fewer of them than either vertices or triangles, so
/// what indexes them is still unknown. Face normals need no such answer and
/// suit collision hulls anyway.
ZoneMesh buildZoneMesh(const ffxi::Zone& zone);

/// The visible world: every placement resolved to its model, transformed into
/// place, and grouped by texture.
///
/// Transforms are baked into the vertices rather than instanced. That costs a
/// copy of the geometry per placement, which is the obvious thing to fix next,
/// but it reuses one pipeline and made the placement maths verifiable first.
ZoneMesh buildPlacedMesh(const ffxi::Zone& zone, const std::unordered_map<std::string, ffxi::Model>& models,
                         size_t& placementsResolved, size_t& placementsMissing);
} // namespace pj
