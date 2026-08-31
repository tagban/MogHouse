#pragma once

// A character: a skeleton, the skinned meshes hung on it, and the geometry
// that comes out once the two are combined.
//
// Skinning happens on the CPU. A zone is hundreds of thousands of triangles
// and has to be instanced on the GPU; a character is a few thousand, and there
// will rarely be more than a few dozen on screen. Doing it here means the
// result drops straight into the pipeline the zone already uses, and it keeps
// the bone maths somewhere it can be inspected.

#include "ffxi/mo2.h"
#include "ffxi/os2.h"
#include "ffxi/skeleton.h"
#include "ffxi/texture.h"
#include "linalg.h"
#include "zonemesh.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace mh
{
/// One equipment slot. FFXI stores a character as separate meshes sharing one
/// skeleton, so an outfit is a matter of swapping which OS2 goes in each slot
/// rather than rebuilding anything.
enum class Slot
{
    Face,
    Head,
    Body,
    Hands,
    Legs,
    Feet,
    Count
};

/// The chunk id a slot's mesh is stored under. Every race uses the same six.
const char* slotChunkId(Slot slot);
const char* slotName(Slot slot);

/// A bone in world space, kept as a rotation and a translation rather than one
/// matrix.
///
/// They cannot be composed. A vertex arrives already scaled by its weight, so
/// the rotation applies to it as-is while the translation has to be scaled by
/// that weight separately. Folding the translation into the matrix weights it
/// twice for one influence and not at all for the other, which pulls every
/// two-bone vertex - shoulders, hips, knees - away from the joint.
struct BonePose
{
    Mat4 rotation{Mat4::identity()};
    Vec3 translation{};
    Vec3 scale{1.0f, 1.0f, 1.0f};
};

/// The skeleton evaluated into one world pose per bone.
///
/// Animation is the same walk with the local rotations replaced, so nothing
/// downstream has to change to make this move.
std::vector<BonePose> bindPose(const ffxi::Skeleton& skeleton);

/// The same walk with one frame of an animation composed onto it.
///
/// The animation does not replace the rest pose, it turns from it: the local
/// rotation is the animated one applied to the bone's own, and the local
/// translation is the sum. Treating the animation as the whole local transform
/// collapses the skeleton, because most animated bones carry no translation of
/// their own and would lose the offset that holds the body together.
///
/// `frame` is in frames and wraps, so a caller can pass elapsed time divided
/// by the animation's frame length without worrying about the length.
std::vector<BonePose> animatedPose(const ffxi::Skeleton& skeleton, const ffxi::Animation& animation, float frame);

/// As above, with a second clip layered over the bones the first leaves alone.
/// FFXI keeps the legs and the upper body in separate clips - walking is only
/// the legs - so an overlay is what puts the arms and torso into a stride.
std::vector<BonePose> animatedPose(const ffxi::Skeleton& skeleton, const ffxi::Animation& animation, float frame,
                                   const ffxi::Animation* overlay, float overlayFrame);

/// Everything needed to draw one character.
struct Character
{
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Batch> batches;
    Vec3 boundsMin{};
    Vec3 boundsMax{};

    Vec3 centre() const { return (boundsMin + boundsMax) * 0.5f; }
    float height() const { return boundsMax.y - boundsMin.y; }
    size_t triangles() const { return indices.size() / 3; }
};

/// Skins the given meshes onto the pose and writes out drawable geometry.
///
/// Vertices are expanded per triangle corner rather than shared: UVs live in
/// the draw stream rather than on the vertex, so the same vertex legitimately
/// appears with different UVs and would have to be split anyway.
Character buildCharacter(const std::vector<BonePose>& pose, const std::vector<ffxi::SkinnedModel>& meshes,
                         const std::unordered_map<std::string, ffxi::Texture>& textures);

/// Rewrites an already-built character's vertices for a new pose, leaving the
/// indices and batches alone. Skinning is the only part of the work that
/// changes between frames.
void reskin(Character& character, const std::vector<BonePose>& pose, const std::vector<ffxi::SkinnedModel>& meshes);
} // namespace mh
