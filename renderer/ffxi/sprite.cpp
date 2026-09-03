#include "sprite.h"

#include <cstring>

namespace ffxi
{
namespace
{
constexpr size_t kHeader = 16; // the chunk header the span excludes
constexpr size_t kCount = 0x12 - kHeader;
constexpr size_t kTexture = 0x18 - kHeader;
constexpr size_t kFrames = 0x28 - kHeader;
constexpr size_t kFrameSize = 148;
constexpr size_t kVertexSize = 24;

template <typename T>
T readAt(std::span<const uint8_t> data, size_t offset)
{
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}
} // namespace

std::optional<SpriteAnimation> parseSprite(const Chunk& chunk)
{
    const std::span<const uint8_t> data = chunk.data;
    if (data.size() < kFrames)
    {
        return std::nullopt;
    }
    const size_t count = data[kCount];
    if (count == 0 || kFrames + count * kFrameSize > data.size() + 8)
    {
        return std::nullopt;
    }

    SpriteAnimation animation;
    animation.name.assign(chunk.id, 4);
    while (!animation.name.empty() && (animation.name.back() == ' ' || animation.name.back() == 0))
    {
        animation.name.pop_back();
    }
    animation.texture.assign(reinterpret_cast<const char*>(data.data() + kTexture), 16);
    while (!animation.texture.empty() && (animation.texture.back() == ' ' || animation.texture.back() == 0))
    {
        animation.texture.pop_back();
    }

    for (size_t f = 0; f < count; ++f)
    {
        const size_t at = kFrames + f * kFrameSize + 4;
        if (at + 6 * kVertexSize > data.size())
        {
            break;
        }
        SpriteFrame frame{};
        for (size_t v = 0; v < 6; ++v)
        {
            const size_t vat = at + v * kVertexSize;
            SpriteVertex& vertex = frame.vertices[v];
            for (int axis = 0; axis < 3; ++axis)
            {
                vertex.position[axis] = readAt<float>(data, vat + axis * 4);
            }
            vertex.colour = readAt<uint32_t>(data, vat + 12);
            vertex.uv[0] = readAt<float>(data, vat + 16);
            vertex.uv[1] = readAt<float>(data, vat + 20);
        }
        animation.frames.push_back(frame);
    }
    if (animation.frames.empty())
    {
        return std::nullopt;
    }
    return animation;
}
} // namespace ffxi
