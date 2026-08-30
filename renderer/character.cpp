#include "character.h"

#include <algorithm>
#include <cmath>

namespace pj
{
namespace
{
/// Column-major rotation from a quaternion. No translation: see BonePose.
Mat4 rotationOf(const float q[4])
{
    const float x = q[0], y = q[1], z = q[2], w = q[3];

    Mat4 out = Mat4::identity();
    out.m[0] = 1.0f - 2.0f * (y * y + z * z);
    out.m[1] = 2.0f * (x * y + z * w);
    out.m[2] = 2.0f * (x * z - y * w);

    out.m[4] = 2.0f * (x * y - z * w);
    out.m[5] = 1.0f - 2.0f * (x * x + z * z);
    out.m[6] = 2.0f * (y * z + x * w);

    out.m[8] = 2.0f * (x * z + y * w);
    out.m[9] = 2.0f * (y * z - x * w);
    out.m[10] = 1.0f - 2.0f * (x * x + y * y);
    return out;
}

Vec3 rotate(const Mat4& m, const Vec3& v)
{
    return {m.m[0] * v.x + m.m[4] * v.y + m.m[8] * v.z, m.m[1] * v.x + m.m[5] * v.y + m.m[9] * v.z,
            m.m[2] * v.x + m.m[6] * v.y + m.m[10] * v.z};
}

/// Reflection through one axis, applied in the bone's own space before the
/// bone rotates it. Axis 0 means the vertex sits on the centre line and is
/// shared between the two halves.
Vec3 mirrored(const Vec3& v, uint8_t axis)
{
    switch (axis)
    {
    case 1: return {-v.x, v.y, v.z};
    case 2: return {v.x, -v.y, v.z};
    case 3: return {v.x, v.y, -v.z};
    default: return v;
    }
}

/// The same rule the zone uses: a texture whose transparent blocks are also
/// black is a cutout mask rather than a blend factor.
constexpr float kCutoutSignal = 0.2f;

struct Skinned
{
    Vec3 position;
    Vec3 normal;
};

/// One vertex through the skin.
///
/// The position stored against each influence already carries that
/// influence's weight, so the rotated position is summed as-is and only the
/// bone's translation is scaled.
Skinned skin(const ffxi::SkinVertex& vertex, const std::vector<BonePose>& pose, bool useMirror)
{
    Skinned out{{}, {}};
    for (uint8_t i = 0; i < vertex.influences; ++i)
    {
        const ffxi::SkinInfluence& influence = vertex.influence[i];
        const uint8_t bone = useMirror ? influence.boneMirror : influence.bone;
        const uint8_t axis = useMirror ? influence.mirrorAxis : 0;
        if (bone >= pose.size())
        {
            continue;
        }

        const BonePose& b = pose[bone];
        const Vec3 p{influence.position[0], influence.position[1], influence.position[2]};
        const Vec3 n{influence.normal[0], influence.normal[1], influence.normal[2]};

        out.position = out.position + rotate(b.rotation, mirrored(p, axis)) + b.translation * influence.weight;
        out.normal = out.normal + rotate(b.rotation, mirrored(n, axis));
    }
    return out;
}
} // namespace

const char* slotChunkId(Slot slot)
{
    switch (slot)
    {
    case Slot::Face: return "hh_1";
    case Slot::Head: return "hh_m";
    case Slot::Body: return "hh_b";
    case Slot::Hands: return "hh_h";
    case Slot::Legs: return "hh_l";
    case Slot::Feet: return "hh_f";
    default: return "";
    }
}

const char* slotName(Slot slot)
{
    switch (slot)
    {
    case Slot::Face: return "face";
    case Slot::Head: return "head";
    case Slot::Body: return "body";
    case Slot::Hands: return "hands";
    case Slot::Legs: return "legs";
    case Slot::Feet: return "feet";
    default: return "?";
    }
}

std::vector<BonePose> bindPose(const ffxi::Skeleton& skeleton)
{
    std::vector<BonePose> pose(skeleton.bones.size());

    // Parents always come before their children in the file, so a single pass
    // in order resolves the whole chain. parseSkeleton has already refused
    // anything that is not a tree, so this cannot loop.
    for (size_t i = 0; i < skeleton.bones.size(); ++i)
    {
        const ffxi::Bone& bone = skeleton.bones[i];
        const Mat4 local = rotationOf(bone.rotation);
        const Vec3 offset{bone.translation[0], bone.translation[1], bone.translation[2]};

        if (bone.parent == i)
        {
            pose[i].rotation = local;
            pose[i].translation = offset;
        }
        else
        {
            const BonePose& parent = pose[bone.parent];
            pose[i].rotation = parent.rotation * local;
            pose[i].translation = parent.translation + rotate(parent.rotation, offset);
        }
    }
    return pose;
}

Character buildCharacter(const std::vector<BonePose>& pose, const std::vector<ffxi::SkinnedModel>& meshes,
                         const std::unordered_map<std::string, ffxi::Texture>& textures)
{
    Character character;
    bool any = false;

    // One batch per texture across every mesh, so a six-piece outfit sharing a
    // texture sheet does not become six draws.
    std::unordered_map<std::string, std::vector<uint32_t>> byTexture;

    auto grow = [&](const Vec3& p) {
        if (!any)
        {
            character.boundsMin = p;
            character.boundsMax = p;
            any = true;
            return;
        }
        character.boundsMin = {std::min(character.boundsMin.x, p.x), std::min(character.boundsMin.y, p.y),
                               std::min(character.boundsMin.z, p.z)};
        character.boundsMax = {std::max(character.boundsMax.x, p.x), std::max(character.boundsMax.y, p.y),
                               std::max(character.boundsMax.z, p.z)};
    };

    for (const ffxi::SkinnedModel& mesh : meshes)
    {
        // A mirrored mesh is half a body: the second pass reflects every
        // vertex onto its opposite bone. Nothing marks which half is stored,
        // because both come from the same triangles.
        const int passes = mesh.mirrored ? 2 : 1;

        for (const ffxi::SkinnedPart& part : mesh.parts)
        {
            if (part.corners.empty())
            {
                continue;
            }
            std::vector<uint32_t>& into = byTexture[part.texture];

            for (int pass = 0; pass < passes; ++pass)
            {
                for (const ffxi::SkinCorner& corner : part.corners)
                {
                    if (corner.vertex >= mesh.vertices.size())
                    {
                        continue;
                    }

                    const Skinned s = skin(mesh.vertices[corner.vertex], pose, pass == 1);

                    Vertex vertex{};
                    // FFXI points Y down, and the zone is flipped to match, so
                    // a character built without this stands on its head in an
                    // otherwise correct world.
                    vertex.position[0] = s.position.x;
                    vertex.position[1] = -s.position.y;
                    vertex.position[2] = s.position.z;

                    const Vec3 unit = normalise(Vec3{s.normal.x, -s.normal.y, s.normal.z});
                    vertex.normal[0] = unit.x;
                    vertex.normal[1] = unit.y;
                    vertex.normal[2] = unit.z;

                    vertex.uv[0] = corner.uv[0];
                    vertex.uv[1] = corner.uv[1];

                    into.push_back(static_cast<uint32_t>(character.vertices.size()));
                    character.vertices.push_back(vertex);
                    grow({vertex.position[0], vertex.position[1], vertex.position[2]});
                }
            }
        }
    }

    for (auto& [texture, indices] : byTexture)
    {
        Batch batch;
        batch.texture = texture;
        batch.indexOffset = static_cast<uint32_t>(character.indices.size());
        batch.indexCount = static_cast<uint32_t>(indices.size());

        const auto found = textures.find(texture);
        batch.cutout = found != textures.end() && found->second.blackWhereClear > kCutoutSignal;

        character.indices.insert(character.indices.end(), indices.begin(), indices.end());
        character.batches.push_back(batch);
    }

    return character;
}
} // namespace pj
