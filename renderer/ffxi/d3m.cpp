#include "d3m.h"

#include <algorithm>
#include <cstring>

namespace ffxi
{
namespace
{
constexpr size_t kHeader = 16;          // the chunk header the span excludes
constexpr size_t kTriangles = 0x16 - kHeader;
constexpr size_t kTexture = 0x1e - kHeader;
constexpr size_t kVertices = 0x2e - kHeader;
constexpr size_t kVertexSize = 36;

template <typename T>
T readAt(std::span<const uint8_t> data, size_t offset)
{
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}
} // namespace

std::optional<Model> parseD3m(const Chunk& chunk)
{
    const std::span<const uint8_t> data = chunk.data;
    if (data.size() < kVertices)
    {
        return std::nullopt;
    }
    const uint16_t triangles = readAt<uint16_t>(data, kTriangles);
    const size_t vertexCount = static_cast<size_t>(triangles) * 3;
    if (triangles == 0 || kVertices + vertexCount * kVertexSize > data.size())
    {
        return std::nullopt;
    }

    Model model;
    model.name.assign(chunk.id, 4);
    while (!model.name.empty() && (model.name.back() == ' ' || model.name.back() == 0))
    {
        model.name.pop_back();
    }

    ModelMesh mesh;
    mesh.texture.assign(reinterpret_cast<const char*>(data.data() + kTexture), 16);
    while (!mesh.texture.empty() && (mesh.texture.back() == ' ' || mesh.texture.back() == 0))
    {
        mesh.texture.pop_back();
    }
    // Flames add to what is behind them; the flag is what the zone meshes
    // use for the same thing, and the effect pass reads it.
    mesh.blending = 0x8000;
    mesh.vertices.reserve(vertexCount);
    mesh.indices.reserve(vertexCount);

    for (int axis = 0; axis < 3; ++axis)
    {
        model.boundsMin[axis] = 1e9f;
        model.boundsMax[axis] = -1e9f;
    }
    for (size_t v = 0; v < vertexCount; ++v)
    {
        const size_t at = kVertices + v * kVertexSize;
        ModelVertex vertex{};
        for (int axis = 0; axis < 3; ++axis)
        {
            vertex.position[axis] = readAt<float>(data, at + axis * 4);
            vertex.normal[axis] = readAt<float>(data, at + 12 + axis * 4);
            model.boundsMin[axis] = std::min(model.boundsMin[axis], vertex.position[axis]);
            model.boundsMax[axis] = std::max(model.boundsMax[axis], vertex.position[axis]);
        }
        vertex.colour = readAt<uint32_t>(data, at + 24);
        vertex.uv[0] = readAt<float>(data, at + 28);
        vertex.uv[1] = readAt<float>(data, at + 32);
        mesh.vertices.push_back(vertex);
        mesh.indices.push_back(static_cast<uint16_t>(v));
    }
    model.meshes.push_back(std::move(mesh));
    return model;
}
} // namespace ffxi
