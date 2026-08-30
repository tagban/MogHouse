#include "skeleton.h"

#include <cstring>
#include <stdexcept>

namespace ffxi
{
namespace
{
// Everything here is 2-byte packed, so no field sits on its natural
// alignment and each one has to be read by offset.
constexpr size_t kBoneCountOffset = 2;
constexpr size_t kBonesOffset = 4;
constexpr size_t kBoneSize = 30;
constexpr size_t kPointSize = 26;

template <typename T> T read(const std::span<const uint8_t>& data, size_t offset)
{
    if (offset + sizeof(T) > data.size())
    {
        throw std::runtime_error("SK2: read past the end of the chunk");
    }
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}
} // namespace

bool Skeleton::isWellFormed() const
{
    if (bones.empty())
    {
        return false;
    }

    for (size_t i = 0; i < bones.size(); ++i)
    {
        if (bones[i].parent >= bones.size())
        {
            return false;
        }

        // Walk to the root. A well-formed skeleton reaches a self-parent in
        // fewer steps than there are bones; anything longer is a cycle.
        size_t walk = i;
        size_t steps = 0;
        while (bones[walk].parent != walk)
        {
            walk = bones[walk].parent;
            if (++steps > bones.size())
            {
                return false;
            }
        }
    }
    return true;
}

Skeleton parseSkeleton(const Chunk& chunk)
{
    const uint16_t boneCount = read<uint16_t>(chunk.data, kBoneCountOffset);
    if (boneCount == 0 || boneCount > 512)
    {
        throw std::runtime_error("SK2: implausible bone count");
    }

    Skeleton skeleton;
    skeleton.bones.reserve(boneCount);
    for (uint16_t i = 0; i < boneCount; ++i)
    {
        const size_t base = kBonesOffset + static_cast<size_t>(i) * kBoneSize;

        Bone bone;
        // The parent is a single byte. Reading it as a uint16 - which is how
        // the field is usually written down - folds the following flag byte
        // into the index and sends half the bones out of range.
        bone.parent = read<uint8_t>(chunk.data, base);
        bone.flags = read<uint8_t>(chunk.data, base + 1);
        for (int c = 0; c < 4; ++c)
        {
            bone.rotation[c] = read<float>(chunk.data, base + 2 + c * 4);
        }
        for (int c = 0; c < 3; ++c)
        {
            bone.translation[c] = read<float>(chunk.data, base + 18 + c * 4);
        }
        skeleton.bones.push_back(bone);
    }

    // The attachment points follow the bones, behind their own count.
    const size_t pointsBase = kBonesOffset + static_cast<size_t>(boneCount) * kBoneSize;
    const uint16_t pointCount = read<uint16_t>(chunk.data, pointsBase);
    if (pointCount > 0 && pointCount <= 1024 &&
        pointsBase + 4 + static_cast<size_t>(pointCount) * kPointSize <= chunk.data.size())
    {
        skeleton.generatorPoints.reserve(pointCount);
        for (uint16_t i = 0; i < pointCount; ++i)
        {
            const size_t base = pointsBase + 4 + static_cast<size_t>(i) * kPointSize;

            GeneratorPoint point;
            point.bone = read<uint16_t>(chunk.data, base);
            // Twelve bytes of as-yet unidentified floats sit between the bone
            // index and the offset.
            for (int c = 0; c < 3; ++c)
            {
                point.offset[c] = read<float>(chunk.data, base + 14 + c * 4);
            }
            skeleton.generatorPoints.push_back(point);
        }
    }

    if (!skeleton.isWellFormed())
    {
        throw std::runtime_error("SK2: bone hierarchy is not a tree");
    }
    return skeleton;
}
} // namespace ffxi
