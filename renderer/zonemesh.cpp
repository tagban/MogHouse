#include "zonemesh.h"

#include <algorithm>
#include <limits>
#include <map>
#include <utility>

namespace pj
{
namespace
{
/// How far apart to push each successive coplanar layer of a model. Small
/// enough to be invisible, large enough to survive depth buffer precision at
/// the distances a zone spans.
constexpr float kLayerSeparation = 0.004f;

/// Below this average |face normal .y| a mesh counts as standing up rather than
/// lying flat. Taken from the triangles rather than the stored normals, which
/// grass billboards deliberately point upward.
constexpr float kUprightThreshold = 0.5f;

struct Bounds
{
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 hi{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    void add(const Vec3& p)
    {
        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
    }
};

/// Builds a placement's transform. Rotations are radians, applied Y then X then
/// Z, to a scaled point, then translated.
Mat4 placementTransform(const ffxi::Placement& placement)
{
    const float sx = std::sin(placement.rotate[0]), cx = std::cos(placement.rotate[0]);
    const float sy = std::sin(placement.rotate[1]), cy = std::cos(placement.rotate[1]);
    const float sz = std::sin(placement.rotate[2]), cz = std::cos(placement.rotate[2]);

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

/// Applies a transform and flips Y. FFXI's Y axis points down; everything past
/// this point can assume Y is up.
Vec3 toWorld(const float* m, float x, float y, float z, bool translate)
{
    const float tx = translate ? m[12] : 0.0f;
    const float ty = translate ? m[13] : 0.0f;
    const float tz = translate ? m[14] : 0.0f;
    return Vec3{m[0] * x + m[4] * y + m[8] * z + tx,
                -(m[1] * x + m[5] * y + m[9] * z + ty),
                m[2] * x + m[6] * y + m[10] * z + tz};
}
} // namespace

float ZoneMesh::radius() const
{
    const Vec3 extent = boundsMax - boundsMin;
    return std::max({extent.x, extent.y, extent.z}) * 0.5f;
}

ZoneMesh buildZoneMesh(const ffxi::Zone& zone)
{
    ZoneMesh out;
    Bounds bounds;

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
            const uint16_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
            if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
            {
                continue;
            }

            const float* m = instance.transform;
            const Vec3 a = toWorld(m, mesh.vertices[ia * 3], mesh.vertices[ia * 3 + 1], mesh.vertices[ia * 3 + 2], true);
            const Vec3 b = toWorld(m, mesh.vertices[ib * 3], mesh.vertices[ib * 3 + 1], mesh.vertices[ib * 3 + 2], true);
            const Vec3 c = toWorld(m, mesh.vertices[ic * 3], mesh.vertices[ic * 3 + 1], mesh.vertices[ic * 3 + 2], true);
            const Vec3 normal = normalise(cross(b - a, c - a));

            for (const Vec3& position : {a, b, c})
            {
                out.indices.push_back(static_cast<uint32_t>(out.vertices.size()));
                out.vertices.push_back(Vertex{{position.x, position.y, position.z}, {normal.x, normal.y, normal.z}, {0.0f, 0.0f}});
                bounds.add(position);
            }
        }
    }

    out.boundsMin = bounds.lo;
    out.boundsMax = bounds.hi;
    if (!out.indices.empty())
    {
        out.batches.push_back(Batch{"", 0, static_cast<uint32_t>(out.indices.size()), 0});
    }
    return out;
}

ZoneMesh buildPlacedMesh(const ffxi::Zone& zone, const std::unordered_map<std::string, ffxi::Model>& models,
                         size_t& placementsResolved, size_t& placementsMissing)
{
    placementsResolved = 0;
    placementsMissing = 0;

    // Gathered per texture first, then concatenated, so each texture ends up as
    // one contiguous run of indices and one draw.
    struct Group
    {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };
    // Keyed by texture and blending together: one draw needs both a single
    // texture and a single alpha treatment.
    std::map<std::pair<std::string, bool>, Group> groups;
    Bounds bounds;

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
        const float* m = transform.m;

        uint32_t meshIndex = 0;
        for (const ffxi::ModelMesh& mesh : found->second.meshes)
        {
            // A mesh standing up is foliage; one lying flat is ground.
            //
            // Measured from the triangles, not from the stored normals. Grass
            // billboards carry normals pointing straight up - the usual trick so
            // they light like the ground they stand on rather than edge-on - so
            // by stored normal they look flat and get treated as terrain, which
            // leaves their black background undiscarded. The geometry does not
            // lie about which way a quad faces.
            float verticality = 0.0f;
            uint32_t faces = 0;
            for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
            {
                const uint16_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
                if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size())
                {
                    continue;
                }
                const float* pa = mesh.vertices[ia].position;
                const float* pb = mesh.vertices[ib].position;
                const float* pc = mesh.vertices[ic].position;
                const Vec3 face = normalise(cross(Vec3{pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2]},
                                                  Vec3{pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2]}));
                verticality += std::fabs(face.y);
                ++faces;
            }
            verticality = faces ? verticality / static_cast<float>(faces) : 1.0f;
            const bool cutout = verticality < kUprightThreshold;

            Group& group = groups[{mesh.texture, cutout}];
            const uint32_t base = static_cast<uint32_t>(group.vertices.size());

            // A model's meshes are layered and coplanar - a base surface with
            // overlays on top - and the game resolves them by draw order within
            // the model. Grouping by texture for the sampler destroys that
            // order, so each layer is nudged along its normal instead. Without
            // this the depth test rejects whichever layer loses the race and
            // whole tiles render as holes.
            const float layerOffset = static_cast<float>(meshIndex) * kLayerSeparation;

            for (const ffxi::ModelVertex& source : mesh.vertices)
            {
                const Vec3 placed = toWorld(m, source.position[0], source.position[1], source.position[2], true);
                // Normals rotate but do not translate.
                const Vec3 normal =
                    normalise(toWorld(m, source.normal[0], source.normal[1], source.normal[2], false));

                const Vec3 world{placed.x + normal.x * layerOffset, placed.y + normal.y * layerOffset,
                                 placed.z + normal.z * layerOffset};

                group.vertices.push_back(
                    Vertex{{world.x, world.y, world.z}, {normal.x, normal.y, normal.z}, {source.uv[0], source.uv[1]}});
                bounds.add(world);
            }

            for (uint16_t index : mesh.indices)
            {
                if (index < mesh.vertices.size())
                {
                    group.indices.push_back(base + index);
                }
            }
            ++meshIndex;
        }
    }

    ZoneMesh out;
    out.boundsMin = bounds.lo;
    out.boundsMax = bounds.hi;

    for (auto& [key, group] : groups)
    {
        if (group.indices.empty())
        {
            continue;
        }

        const uint32_t vertexBase = static_cast<uint32_t>(out.vertices.size());
        const uint32_t indexStart = static_cast<uint32_t>(out.indices.size());

        out.vertices.insert(out.vertices.end(), group.vertices.begin(), group.vertices.end());
        for (uint32_t index : group.indices)
        {
            out.indices.push_back(vertexBase + index);
        }

        out.batches.push_back(
            Batch{key.first, indexStart, static_cast<uint32_t>(out.indices.size()) - indexStart, key.second});
    }

    return out;
}
} // namespace pj
