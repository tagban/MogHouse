#pragma once

// How much of a zone's footprint actually has geometry over it.
//
// The question this answers: when a zone renders with gaps, is the terrain
// itself missing, or is it there and the gaps are water and effects? Guessing
// at that has been wrong repeatedly; this measures it.

#include "zonemesh.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace mh
{
struct Coverage
{
    float anyGeometry{};  ///< fraction of cells with any triangle over them
    float groundLike{};   ///< fraction with a roughly horizontal triangle
    uint32_t cells{};
};

/// Projects triangles onto the ground plane and rasterises them into a grid.
///
/// Two numbers, because they answer different things. "Any geometry" includes
/// tree canopies and walls, so it overstates ground cover. "Ground-like" counts
/// only near-horizontal faces, which is much closer to walkable surface.
inline Coverage measureCoverage(const ZoneMesh& mesh, uint32_t resolution = 256)
{
    Coverage result;
    result.cells = resolution * resolution;
    if (mesh.indices.empty())
    {
        return result;
    }

    const float spanX = mesh.boundsMax.x - mesh.boundsMin.x;
    const float spanZ = mesh.boundsMax.z - mesh.boundsMin.z;
    if (spanX <= 0.0f || spanZ <= 0.0f)
    {
        return result;
    }

    std::vector<uint8_t> any(result.cells, 0);
    std::vector<uint8_t> ground(result.cells, 0);

    auto cellX = [&](float x) { return (x - mesh.boundsMin.x) / spanX * static_cast<float>(resolution); };
    auto cellZ = [&](float z) { return (z - mesh.boundsMin.z) / spanZ * static_cast<float>(resolution); };

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
    {
        const Vertex& va = mesh.vertices[mesh.indices[i]];
        const Vertex& vb = mesh.vertices[mesh.indices[i + 1]];
        const Vertex& vc = mesh.vertices[mesh.indices[i + 2]];

        const float ax = cellX(va.position[0]), az = cellZ(va.position[2]);
        const float bx = cellX(vb.position[0]), bz = cellZ(vb.position[2]);
        const float cx = cellX(vc.position[0]), cz = cellZ(vc.position[2]);

        // A face is ground-like when its normal points mostly up or down.
        const float ny = (va.normal[1] + vb.normal[1] + vc.normal[1]) / 3.0f;
        const bool horizontal = ny > 0.7f || ny < -0.7f;

        int minX = static_cast<int>(std::floor(std::min({ax, bx, cx})));
        int maxX = static_cast<int>(std::ceil(std::max({ax, bx, cx})));
        int minZ = static_cast<int>(std::floor(std::min({az, bz, cz})));
        int maxZ = static_cast<int>(std::ceil(std::max({az, bz, cz})));
        minX = std::max(minX, 0);
        minZ = std::max(minZ, 0);
        maxX = std::min(maxX, static_cast<int>(resolution) - 1);
        maxZ = std::min(maxZ, static_cast<int>(resolution) - 1);

        const float area = (bx - ax) * (cz - az) - (cx - ax) * (bz - az);
        if (std::fabs(area) < 1e-9f)
        {
            continue; // edge-on from above, covers nothing
        }

        for (int z = minZ; z <= maxZ; ++z)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                const float px = static_cast<float>(x) + 0.5f;
                const float pz = static_cast<float>(z) + 0.5f;
                const float w0 = ((bx - ax) * (pz - az) - (px - ax) * (bz - az)) / area;
                const float w1 = ((px - ax) * (cz - az) - (cx - ax) * (pz - az)) / area;
                if (w0 < 0.0f || w1 < 0.0f || w0 + w1 > 1.0f)
                {
                    continue;
                }
                const size_t cell = static_cast<size_t>(z) * resolution + static_cast<size_t>(x);
                any[cell] = 1;
                if (horizontal)
                {
                    ground[cell] = 1;
                }
            }
        }
    }

    uint32_t anyCount = 0;
    uint32_t groundCount = 0;
    for (size_t i = 0; i < any.size(); ++i)
    {
        anyCount += any[i];
        groundCount += ground[i];
    }
    result.anyGeometry = static_cast<float>(anyCount) / static_cast<float>(result.cells);
    result.groundLike = static_cast<float>(groundCount) / static_cast<float>(result.cells);
    return result;
}

/// Prints where one mesh covers ground the other does not.
///
/// The question is whether the difference is a coherent region - a missing
/// terrain layer - or scattered cells, which would mean the two simply
/// tessellate differently. A picture answers that; a percentage does not.
inline void printCoverageDiff(const ZoneMesh& reference, const ZoneMesh& subject, uint32_t resolution = 64)
{
    auto rasterise = [&](const ZoneMesh& mesh, const Vec3& lo, const Vec3& hi)
    {
        std::vector<uint8_t> grid(static_cast<size_t>(resolution) * resolution, 0);
        const float spanX = hi.x - lo.x;
        const float spanZ = hi.z - lo.z;
        if (spanX <= 0.0f || spanZ <= 0.0f)
        {
            return grid;
        }
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            for (int corner = 0; corner < 3; ++corner)
            {
                const Vertex& v = mesh.vertices[mesh.indices[i + corner]];
                const int x = static_cast<int>((v.position[0] - lo.x) / spanX * static_cast<float>(resolution));
                const int z = static_cast<int>((v.position[2] - lo.z) / spanZ * static_cast<float>(resolution));
                if (x >= 0 && z >= 0 && x < static_cast<int>(resolution) && z < static_cast<int>(resolution))
                {
                    grid[static_cast<size_t>(z) * resolution + static_cast<size_t>(x)] = 1;
                }
            }
        }
        return grid;
    };

    // Both rasterised over the same extent, or the maps do not line up.
    const Vec3 lo{std::min(reference.boundsMin.x, subject.boundsMin.x), 0.0f,
                  std::min(reference.boundsMin.z, subject.boundsMin.z)};
    const Vec3 hi{std::max(reference.boundsMax.x, subject.boundsMax.x), 0.0f,
                  std::max(reference.boundsMax.z, subject.boundsMax.z)};

    const std::vector<uint8_t> a = rasterise(reference, lo, hi);
    const std::vector<uint8_t> b = rasterise(subject, lo, hi);

    uint32_t both = 0, onlyReference = 0, onlySubject = 0;
    std::printf("  map: # both, R collision only, M models only, . neither\n");
    for (uint32_t z = 0; z < resolution; ++z)
    {
        std::printf("  ");
        for (uint32_t x = 0; x < resolution; ++x)
        {
            const size_t cell = static_cast<size_t>(z) * resolution + x;
            const bool inA = a[cell] != 0;
            const bool inB = b[cell] != 0;
            char c = '.';
            if (inA && inB) { c = '#'; ++both; }
            else if (inA) { c = 'R'; ++onlyReference; }
            else if (inB) { c = 'M'; ++onlySubject; }
            std::printf("%c", c);
        }
        std::printf("\n");
    }
    std::printf("  both %u, collision only %u, models only %u\n", both, onlyReference, onlySubject);
}
} // namespace mh
