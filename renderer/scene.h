#pragma once

// A zone as unique geometry plus per-placement transforms.
//
// The earlier approach baked each placement's transform into its own copy of
// the vertices, which made one zone 658,000 triangles out of about 36,000 of
// actual geometry - the three most-placed models alone account for 3,033 copies.
// Here each mesh is uploaded once and drawn once per placement.

#include "ffxi/mmb.h"
#include "ffxi/mzb.h"
#include "ffxi/texture.h"
#include "linalg.h"
#include "zonemesh.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mh
{
/// One mesh, drawn once per placement of the model it belongs to.
struct InstancedDraw
{
    std::string texture;
    bool cutout{};
    uint32_t indexOffset{};
    uint32_t indexCount{};
    uint32_t instanceOffset{};
    uint32_t instanceCount{};
};

struct Scene
{
    std::vector<Vertex> vertices;   ///< model space, each mesh appearing once
    std::vector<uint32_t> indices;
    std::vector<float> instances;   ///< 16 floats per placement, column major
    std::vector<InstancedDraw> draws;

    /// Water surfaces, as flat quads. Not instanced - each cell's plane is its
    /// own rectangle - and not placed by the placement table: water comes from a
    /// height carried on each collision grid entry.
    std::vector<Vertex> waterVertices;
    std::vector<uint32_t> waterIndices;

    Vec3 boundsMin{};
    Vec3 boundsMax{};

    Vec3 centre() const { return (boundsMin + boundsMax) * 0.5f; }
    float radius() const;

    size_t triangles() const;      ///< unique geometry
    size_t drawnTriangles() const; ///< what the GPU actually processes
};

Scene buildScene(const ffxi::Zone& zone, const std::unordered_map<std::string, ffxi::Model>& models,
                 const std::unordered_map<std::string, ffxi::Texture>& textures, size_t& placementsResolved,
                 size_t& placementsMissing);
} // namespace mh
