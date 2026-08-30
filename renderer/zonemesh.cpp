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

    // FFXI's Y axis points down - ground level objects sit at negative Y, and a
    // zone's collision runs from about -6 up to +1.5. Flip it on the way in so
    // everything downstream can assume Y is up.
    auto toWorld = [](const ffxi::CollisionInstance& instance, const Vec3& local)
    {
        const float* m = instance.transform;
        const Vec3 placed{m[0] * local.x + m[4] * local.y + m[8] * local.z + m[12],
                          m[1] * local.x + m[5] * local.y + m[9] * local.z + m[13],
                          m[2] * local.x + m[6] * local.y + m[10] * local.z + m[14]};
        return Vec3{placed.x, -placed.y, placed.z};
    };

    // An instance says where a mesh goes. Without them every mesh lands on the
    // origin, folded through every other one.
    for (const ffxi::CollisionInstance& instance : zone.instances)
    {
        if (instance.mesh >= zone.collision.size())
        {
            continue;
        }
        const ffxi::CollisionMesh& mesh = zone.collision[instance.mesh];
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

            const Vec3 a = toWorld(instance, {mesh.vertices[ia * 3], mesh.vertices[ia * 3 + 1], mesh.vertices[ia * 3 + 2]});
            const Vec3 b = toWorld(instance, {mesh.vertices[ib * 3], mesh.vertices[ib * 3 + 1], mesh.vertices[ib * 3 + 2]});
            const Vec3 c = toWorld(instance, {mesh.vertices[ic * 3], mesh.vertices[ic * 3 + 1], mesh.vertices[ic * 3 + 2]});

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

namespace pj
{
namespace
{
/// Builds a placement's transform. Rotations are in radians; the order is a
/// first guess and is the thing to revisit if models come out twisted rather
/// than misplaced.
Mat4 placementTransform(const ffxi::Placement& placement)
{
    const float sx = std::sin(placement.rotate[0]), cx = std::cos(placement.rotate[0]);
    const float sy = std::sin(placement.rotate[1]), cy = std::cos(placement.rotate[1]);
    const float sz = std::sin(placement.rotate[2]), cz = std::cos(placement.rotate[2]);

    // Y then X then Z, applied to a scaled point, then translated.
    Mat4 m = Mat4::identity();
    m.m[0] = (cy * cz + sy * sx * sz) * placement.scale[0];
    m.m[1] = (cx * sz) * placement.scale[0];
    m.m[2] = (-sy * cz + cy * sx * sz) * placement.scale[0];

    m.m[4] = (-cy * sz + sy * sx * cz) * placement.scale[1];
    m.m[5] = (cx * cz) * placement.scale[1];
    m.m[6] = (sy * sz + cy * sx * cz) * placement.scale[1];

    m.m[8] = (sy * cx) * placement.scale[2];
    m.m[9] = -sx * placement.scale[2];
    m.m[10] = (cy * cx) * placement.scale[2];

    m.m[12] = placement.translate[0];
    m.m[13] = placement.translate[1];
    m.m[14] = placement.translate[2];
    return m;
}
} // namespace

ZoneMesh buildPlacedMesh(const ffxi::Zone& zone, const std::unordered_map<std::string, ffxi::Model>& models,
                         size_t& placementsResolved, size_t& placementsMissing)
{
    ZoneMesh out;
    placementsResolved = 0;
    placementsMissing = 0;
    out.boundsMin = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    out.boundsMax = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    for (const ffxi::Placement& placement : zone.placements)
    {
        auto found = models.find(placement.model);
        if (found == models.end())
        {
            ++placementsMissing;
            continue;
        }
        ++placementsResolved;

        const Mat4 transform = placementTransform(placement);
        const ffxi::Model& model = found->second;

        for (const ffxi::ModelMesh& mesh : model.meshes)
        {
            const uint32_t base = static_cast<uint32_t>(out.vertices.size());

            for (const ffxi::ModelVertex& source : mesh.vertices)
            {
                const float* m = transform.m;
                const float x = source.position[0], y = source.position[1], z = source.position[2];
                // FFXI's Y points down, so flip after placing, matching the
                // collision path.
                const Vec3 world{m[0] * x + m[4] * y + m[8] * z + m[12],
                                 -(m[1] * x + m[5] * y + m[9] * z + m[13]),
                                 m[2] * x + m[6] * y + m[10] * z + m[14]};

                const Vec3 normal = normalise({m[0] * source.normal[0] + m[4] * source.normal[1] + m[8] * source.normal[2],
                                               -(m[1] * source.normal[0] + m[5] * source.normal[1] + m[9] * source.normal[2]),
                                               m[2] * source.normal[0] + m[6] * source.normal[1] + m[10] * source.normal[2]});

                out.vertices.push_back(Vertex{{world.x, world.y, world.z}, {normal.x, normal.y, normal.z}});
                out.boundsMin = {std::min(out.boundsMin.x, world.x), std::min(out.boundsMin.y, world.y),
                                 std::min(out.boundsMin.z, world.z)};
                out.boundsMax = {std::max(out.boundsMax.x, world.x), std::max(out.boundsMax.y, world.y),
                                 std::max(out.boundsMax.z, world.z)};
            }

            for (uint16_t index : mesh.indices)
            {
                if (index < mesh.vertices.size())
                {
                    out.indices.push_back(base + index);
                }
            }
        }
    }

    return out;
}
} // namespace pj
