#include "scene.h"

#include <cstdlib>
#include <string>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace mh
{
namespace
{
/// Successive coplanar layers of a model are pushed apart along their normal,
/// since the game resolves them by draw order and batching destroys that.
constexpr float kLayerSeparation = 0.004f;

/// A cutout texture is black where it is transparent; a blend texture carries
/// colour throughout. Cutouts measure 0.41 upward, everything else 0.00.
constexpr float kCutoutSignal = 0.2f;

/// Enough fully transparent texels to mean a texture is meant to be cut out
/// rather than to have a stray edge texel or two.
constexpr float kAnyTransparency = 0.005f;

/// The bit in a mesh header's blending field that asks for alpha blending.
constexpr uint16_t kBlendFlag = 0x8000;

/// Builds a placement's transform, with the turn out of FFXI's frame folded
/// in so everything downstream can assume Y is up.
///
/// The rotation composes as T . Rz.Ry.Rx . S. The order matters and is not
/// guessable: an earlier composition here agreed with this one exactly
/// whenever the Z rotation was zero, and differed otherwise, so it placed most
/// of a zone correctly and scattered the handful of models that are turned
/// about Z - a few buildings out by a constant while everything around them
/// was within 0.07 of where the server says it is.
///
/// Measured over 5199 points where collision reports a floor, asking whether
/// anything is drawn within 0.6 of it (ffxi-visiblecheck):
///
///   T . Rz.Ry.Rx . S   99.0%,  33 drawn but out of place
///   the earlier order  93.8%, 306 drawn but out of place
///
/// The order is the one vekien/xi-model-viewer uses, which the user pointed
/// at after this had been chased through three wrong explanations.
Mat4 placementTransform(const ffxi::Placement& placement)
{
    const float sx = std::sin(placement.rotate[0]), cx = std::cos(placement.rotate[0]);
    const float sy = std::sin(placement.rotate[1]), cy = std::cos(placement.rotate[1]);
    const float sz = std::sin(placement.rotate[2]), cz = std::cos(placement.rotate[2]);

    Mat4 m = Mat4::identity();
    m.m[0] = (cy * cz) * placement.scale[0];
    m.m[1] = (cy * sz) * placement.scale[0];
    m.m[2] = (-sy) * placement.scale[0];

    m.m[4] = (sx * sy * cz - cx * sz) * placement.scale[1];
    m.m[5] = (sx * sy * sz + cx * cz) * placement.scale[1];
    m.m[6] = (sx * cy) * placement.scale[1];

    m.m[8] = (cx * sy * cz + sx * sz) * placement.scale[2];
    m.m[9] = (cx * sy * sz - sx * cz) * placement.scale[2];
    m.m[10] = (cx * cy) * placement.scale[2];

    m.m[12] = placement.translate[0];
    m.m[13] = placement.translate[1];
    m.m[14] = placement.translate[2];

    // Negating the second and third rows applies the half turn about X after
    // the rest of the transform - the same conversion toWorld does per vertex,
    // see renderer/zonemesh.cpp.
    //
    // MOGHOUSE_PLACEMENT_LEGACY negates only the Y row, the way this did
    // before the world became a rotation rather than a reflection. Kept so the
    // two can be scored against collision - which agrees with the server - and
    // the argument settled with a number instead of another opinion.
    static const bool legacy = std::getenv("MOGHOUSE_PLACEMENT_LEGACY") != nullptr;
    for (int column = 0; column < 4; ++column)
    {
        m.m[column * 4 + 1] = -m.m[column * 4 + 1];
        if (!legacy)
        {
            m.m[column * 4 + 2] = -m.m[column * 4 + 2];
        }
    }
    return m;
}

/// Whether a model is one of the meshes FFXI uses for water surfaces.
///
/// Water is ordinary placed geometry with a recognisable name, not something
/// derived from the MZB's per-cell height - the naming convention is the one
/// vekien/xi-model-viewer matches on, with "suimen" being the Japanese for
/// water surface.
bool isWaterModel(const std::string& name)
{
    static const char* kExact[] = {"water", "water2", "lowsea", "2lowsea", "lowcol", "suimen", "tamadai"};
    std::string lower;
    lower.reserve(name.size());
    for (char c : name)
    {
        lower.push_back(c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c);
    }
    for (const char* one : kExact)
    {
        if (lower == one)
        {
            return true;
        }
    }
    return lower.rfind("sea", 0) == 0 || lower.rfind("water", 0) == 0 || lower.rfind("ocean", 0) == 0 ||
           lower.find("suimen") != std::string::npos;
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
    // MOGHOUSE_SKIP_WATER_MODELS leaves out the meshes FFXI names as water -
    // "water", "water2", "suimen", "lowsea" and friends - which is how you
    // find out whether a suspicious flat expanse is one of them or a hole.
    const bool skipWaterModels = std::getenv("MOGHOUSE_SKIP_WATER_MODELS") != nullptr;

    std::map<std::string, std::vector<Mat4>> byModel;
    for (const ffxi::Placement& placement : zone.placements)
    {
        if (skipWaterModels && isWaterModel(placement.model))
        {
            continue;
        }
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
        const bool water = isWaterModel(name);

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
            // Alpha tested if the texture has fully transparent texels at all,
            // not only if those texels also compressed to black.
            //
            // blackWhereClear measures one particular DXT tell, and it is a
            // good one for foliage. A cloth awning whose clear area is not
            // black scores below it, gets drawn opaque, and its transparent
            // texels come out as black blobs on the cloth.
            const bool cutout = texture != textures.end() &&
                                (texture->second.blackWhereClear > kCutoutSignal ||
                                 texture->second.alphaZero > kAnyTransparency);
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
                vertex.colour = source.colour;
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

            scene.draws.push_back(InstancedDraw{mesh.texture, cutout, (mesh.blending & kBlendFlag) != 0, water,
                                                indexStart,
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

    // Water. Each collision grid entry carries the height of the water over its
    // cell, and lotus notes in passing that the surface should be a flat plane
    // rather than a translated copy of the collision mesh - so that is what this
    // builds: one quad per cell, spanning where that cell's geometry reaches.
    //
    // Set MOGHOUSE_NO_WATER to leave it out. The water word is read exactly as
    // lotus-engine reads it - same offset, same shifts - and lotus carries a
    // TODO saying its own water handling is wrong, so this is not settled
    // enough to be sure a strange looking zone is the geometry's fault. Being
    // able to take the water away answers that in one look.
    // Water from the collision grid's per-cell height, on top of the named
    // water meshes rather than instead of them. The two are different things:
    // "water" and "water2" are the big bodies the zone is built around, and
    // this is the shallow standing water that sits on a floor - the pools
    // around the Bastok Markets fountain are 0.20 deep on floors at 11.1,
    // 12.0, 12.9 and 13.6, each its own little surface.
    //
    // This was off for a while, on the reasoning that a water height tracking
    // each cell's own floor could not be right because water does not climb
    // stairs. That was the wrong picture: it is not one body climbing, it is
    // many separate pools, and turning it off took the fountain with it.
    const bool skipWater = std::getenv("MOGHOUSE_NO_WATER") != nullptr;
    for (const ffxi::CollisionInstance& instance : zone.instances)
    {
        if (skipWater || instance.waterHeight == 0.0f || instance.mesh >= zone.collision.size())
        {
            continue;
        }
        const ffxi::CollisionMesh& mesh = zone.collision[instance.mesh];
        if (mesh.vertices.size() < 3)
        {
            continue;
        }

        // The cell's own triangles, flattened to the water height, rather than
        // a bounding rectangle around them. A rectangle spills past the bed and
        // leaves translucent squares lying on the grass.
        const float y = -instance.waterHeight;
        const size_t vertexCount = mesh.vertexCount();
        const uint32_t base = static_cast<uint32_t>(scene.waterVertices.size());

        for (size_t v = 0; v < vertexCount; ++v)
        {
            // transformPoint applies the instance matrix and nothing else, so
            // this is still in FFXI's frame and has to be turned the same way
            // the geometry is - see renderer/zonemesh.cpp. The vertical is
            // already the water height rather than a transformed one, so only
            // the depth axis is left to flip.
            //
            // Everything else reaches the world through toWorld and got this
            // for free. This path did not, and the result was a sheet of water
            // sitting where the market floor should be: mirrored about z while
            // the ground it was meant to fill stayed put.
            const Vec3 raw = transformPoint(instance.transform,
                                            {mesh.vertices[v * 3], mesh.vertices[v * 3 + 1], mesh.vertices[v * 3 + 2]});
            const Vec3 world{raw.x, y, -raw.z};

            Vertex vertex{};
            vertex.position[0] = world.x;
            vertex.position[1] = world.y;
            vertex.position[2] = world.z;
            vertex.normal[1] = 1.0f;
            // World-space UVs, so the texture is continuous across cells rather
            // than restarting at every seam.
            vertex.uv[0] = world.x * 0.06f;
            vertex.uv[1] = world.z * 0.06f;
            scene.waterVertices.push_back(vertex);

            lo = {std::min(lo.x, world.x), std::min(lo.y, y), std::min(lo.z, world.z)};
            hi = {std::max(hi.x, world.x), std::max(hi.y, y), std::max(hi.z, world.z)};
        }

        for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            const uint16_t ia = mesh.indices[i], ib = mesh.indices[i + 1], ic = mesh.indices[i + 2];
            if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount)
            {
                continue;
            }
            scene.waterIndices.push_back(base + ia);
            scene.waterIndices.push_back(base + ib);
            scene.waterIndices.push_back(base + ic);
        }
    }

    scene.boundsMin = lo;
    scene.boundsMax = hi;
    return scene;
}
} // namespace mh
