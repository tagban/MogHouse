#include "scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace pj
{
namespace
{
/// Successive coplanar layers of a model are pushed apart along their normal,
/// since the game resolves them by draw order and batching destroys that.
constexpr float kLayerSeparation = 0.004f;

/// A cutout texture is black where it is transparent; a blend texture carries
/// colour throughout. Cutouts measure 0.41 upward, everything else 0.00.
constexpr float kCutoutSignal = 0.2f;

/// Builds a placement's transform, with FFXI's downward Y folded in so
/// everything downstream can assume Y is up.
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

    // Negating the second row flips Y after the rest of the transform, which is
    // what the per-vertex path used to do by hand.
    for (int column = 0; column < 4; ++column)
    {
        m.m[column * 4 + 1] = -m.m[column * 4 + 1];
    }
    return m;
}

Vec3 transformPoint(const float* m, const Vec3& p)
{
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
}
} // namespace

float Scene::radius() const
{
    const Vec3 extent = boundsMax - boundsMin;
    return std::max({extent.x, extent.y, extent.z}) * 0.5f;
}

size_t Scene::triangles() const
{
    return indices.size() / 3;
}

size_t Scene::drawnTriangles() const
{
    size_t total = 0;
    for (const InstancedDraw& draw : draws)
    {
        total += static_cast<size_t>(draw.indexCount / 3) * draw.instanceCount;
    }
    return total;
}

Scene buildScene(const ffxi::Zone& zone, const std::unordered_map<std::string, ffxi::Model>& models,
                 const std::unordered_map<std::string, ffxi::Texture>& textures, size_t& placementsResolved,
                 size_t& placementsMissing)
{
    placementsResolved = 0;
    placementsMissing = 0;

    // Every placement of each model, so a model's geometry can be uploaded once
    // and drawn against the whole list.
    std::map<std::string, std::vector<Mat4>> byModel;
    for (const ffxi::Placement& placement : zone.placements)
    {
        if (models.find(placement.model) == models.end())
        {
            ++placementsMissing;
            continue;
        }
        ++placementsResolved;
        byModel[placement.model].push_back(placementTransform(placement));
    }

    Scene scene;
    Vec3 lo{std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    Vec3 hi{std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest()};

    for (const auto& [name, transforms] : byModel)
    {
        const ffxi::Model& model = models.find(name)->second;

        const uint32_t instanceOffset = static_cast<uint32_t>(scene.instances.size() / 16);
        for (const Mat4& transform : transforms)
        {
            scene.instances.insert(scene.instances.end(), transform.m, transform.m + 16);
        }

        uint32_t meshIndex = 0;
        for (const ffxi::ModelMesh& mesh : model.meshes)
        {
            if (mesh.indices.empty() || mesh.vertices.empty())
            {
                ++meshIndex;
                continue;
            }

            auto texture = textures.find(mesh.texture);
            const bool cutout = texture != textures.end() && texture->second.blackWhereClear > kCutoutSignal;
            const float layerOffset = static_cast<float>(meshIndex) * kLayerSeparation;

            const uint32_t vertexBase = static_cast<uint32_t>(scene.vertices.size());
            const uint32_t indexStart = static_cast<uint32_t>(scene.indices.size());

            for (const ffxi::ModelVertex& source : mesh.vertices)
            {
                Vertex vertex{};
                for (int axis = 0; axis < 3; ++axis)
                {
                    vertex.position[axis] = source.position[axis] + source.normal[axis] * layerOffset;
                    vertex.normal[axis] = source.normal[axis];
                }
                vertex.uv[0] = source.uv[0];
                vertex.uv[1] = source.uv[1];
                scene.vertices.push_back(vertex);
            }

            for (uint16_t index : mesh.indices)
            {
                if (index < mesh.vertices.size())
                {
                    scene.indices.push_back(vertexBase + index);
                }
            }

            scene.draws.push_back(InstancedDraw{mesh.texture, cutout, indexStart,
                                                static_cast<uint32_t>(scene.indices.size()) - indexStart,
                                                instanceOffset, static_cast<uint32_t>(transforms.size())});

            // Bounds have to account for where the instances put things, so
            // each mesh's corners are run through every transform.
            for (const Mat4& transform : transforms)
            {
                for (const ffxi::ModelVertex& source : mesh.vertices)
                {
                    const Vec3 world = transformPoint(
                        transform.m, {source.position[0], source.position[1], source.position[2]});
                    lo = {std::min(lo.x, world.x), std::min(lo.y, world.y), std::min(lo.z, world.z)};
                    hi = {std::max(hi.x, world.x), std::max(hi.y, world.y), std::max(hi.z, world.z)};
                }
            }
            ++meshIndex;
        }
    }

    scene.boundsMin = lo;
    scene.boundsMax = hi;
    return scene;
}
} // namespace pj
