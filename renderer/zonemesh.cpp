#include "zonemesh.h"

#include <algorithm>
#include <limits>

namespace pj
{
float ZoneMesh::radius() const
{
    const Vec3 extent = boundsMax - boundsMin;
    return std::max({extent.x, extent.y, extent.z}) * 0.5f;
}

ZoneMesh buildZoneMesh(const ffxi::Zone& zone)
{
    ZoneMesh out;
    out.boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    out.boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    for (const ffxi::CollisionMesh& mesh : zone.collision)
    {
        const size_t vertexCount = mesh.vertexCount();
        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const uint16_t ia = mesh.indices[i];
            const uint16_t ib = mesh.indices[i + 1];
            const uint16_t ic = mesh.indices[i + 2];
            if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
            {
                continue;
            }

            const Vec3 a{mesh.vertices[ia * 3], mesh.vertices[ia * 3 + 1], mesh.vertices[ia * 3 + 2]};
            const Vec3 b{mesh.vertices[ib * 3], mesh.vertices[ib * 3 + 1], mesh.vertices[ib * 3 + 2]};
            const Vec3 c{mesh.vertices[ic * 3], mesh.vertices[ic * 3 + 1], mesh.vertices[ic * 3 + 2]};

            const Vec3 normal = normalise(cross(b - a, c - a));

            for (const Vec3& position : {a, b, c})
            {
                out.indices.push_back(static_cast<uint32_t>(out.vertices.size()));
                out.vertices.push_back(Vertex{{position.x, position.y, position.z}, {normal.x, normal.y, normal.z}});

                out.boundsMin = {std::min(out.boundsMin.x, position.x), std::min(out.boundsMin.y, position.y),
                                 std::min(out.boundsMin.z, position.z)};
                out.boundsMax = {std::max(out.boundsMax.x, position.x), std::max(out.boundsMax.y, position.y),
                                 std::max(out.boundsMax.z, position.z)};
            }
        }
    }

    return out;
}
} // namespace pj
