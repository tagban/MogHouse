#include "mmb.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>

namespace ffxi
{
namespace
{
constexpr size_t kVertexStride = 36;  // position, normal, colour, uv
constexpr size_t kVertexStride2 = 48; // as above plus a displacement vector

template <typename T> T read(const std::vector<uint8_t>& buffer, size_t offset)
{
    if (offset + sizeof(T) > buffer.size())
    {
        throw std::runtime_error("MMB: read past end of chunk");
    }
    T value{};
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    return value;
}

std::string readName(const std::vector<uint8_t>& buffer, size_t offset, size_t size)
{
    if (offset + size > buffer.size())
    {
        throw std::runtime_error("MMB: name runs past end of chunk");
    }
    std::string name(reinterpret_cast<const char*>(buffer.data() + offset), size);
    // Names are space padded, and some carry trailing nulls as well.
    while (!name.empty() && (name.back() == ' ' || name.back() == 0))
    {
        name.pop_back();
    }
    return name;
}

// Undoes both obfuscation stages. See docs/mmb-format.md - the key advances
// twice per byte and the shift uses the key after the first advance.
void decrypt(std::vector<uint8_t>& buffer, const KeyTable& keys, const KeyTable& keys2)
{
    if (buffer.size() < 8)
    {
        return;
    }

    uint32_t length = read<uint32_t>(buffer, 0) & 0x00FFFFFF;
    if (length > buffer.size())
    {
        length = static_cast<uint32_t>(buffer.size());
    }

    if (buffer[3] >= 5)
    {
        uint32_t key = keys[buffer[5] ^ 0xF0];
        uint32_t count = 0;
        for (uint32_t pos = 8; pos < length; ++pos)
        {
            const uint32_t x = ((key & 0xFF) << 8) | (key & 0xFF);
            key += ++count;
            buffer[pos] ^= static_cast<uint8_t>(x >> (key & 7));
            key += ++count;
        }
    }

    if (buffer[6] == 0xFF && buffer[7] == 0xFF)
    {
        uint32_t key1 = static_cast<uint32_t>(buffer[5] ^ 0xF0);
        uint32_t key2 = keys2[key1];
        const uint32_t half = ((length - 8) & ~0xFu) / 2;

        for (uint32_t pos = 0; pos < half; pos += 8)
        {
            const size_t a = 8 + pos;
            const size_t b = 8 + half + pos;
            if (b + 8 > buffer.size())
            {
                break;
            }
            if (key2 & 1)
            {
                for (size_t i = 0; i < 8; ++i)
                {
                    std::swap(buffer[a + i], buffer[b + i]);
                }
            }
            key1 += 9;
            key2 += key1;
        }
    }
}
} // namespace

size_t Model::vertexCount() const
{
    size_t total = 0;
    for (const ModelMesh& mesh : meshes)
    {
        total += mesh.vertices.size();
    }
    return total;
}

size_t Model::triangleCount() const
{
    size_t total = 0;
    for (const ModelMesh& mesh : meshes)
    {
        total += mesh.indices.size() / 3;
    }
    return total;
}

Model parseMmb(const Chunk& chunk, const KeyTable& keys, const KeyTable& keys2)
{
    std::vector<uint8_t> buffer(chunk.data.begin(), chunk.data.end());
    decrypt(buffer, keys, keys2);

    Model model;
    // Byte 4 selects the vertex layout: 2 means the longer one, which carries an
    // extra displacement vector between position and normal.
    const uint8_t layout = buffer.size() > 4 ? buffer[4] : 0;
    const size_t stride = layout == 2 ? kVertexStride2 : kVertexStride;

    model.group = readName(buffer, 8, 8);
    model.name = readName(buffer, 16, 16);

    const int32_t pieces = read<int32_t>(buffer, 32);
    for (int axis = 0; axis < 3; ++axis)
    {
        model.boundsMin[axis] = read<float>(buffer, 36 + axis * 8);
        model.boundsMax[axis] = read<float>(buffer, 40 + axis * 8);
    }
    const uint32_t blockHeaderOffset = read<uint32_t>(buffer, 60);

    // Some models are exactly a header and nothing else - the hit_ ones, which
    // are collision proxies. That is empty, not broken.
    if (buffer.size() <= 64)
    {
        return model;
    }

    if (pieces < 0 || pieces > 4096)
    {
        throw std::runtime_error("MMB: implausible piece count");
    }

    // Either one block follows the header, or the header is followed by a table
    // of offsets pointing at them.
    std::vector<size_t> pieceOffsets;
    if (blockHeaderOffset == 0)
    {
        pieceOffsets.push_back(64);
    }
    else
    {
        size_t cursor = 64;
        while (cursor + 4 <= blockHeaderOffset && pieceOffsets.size() < static_cast<size_t>(pieces))
        {
            const uint32_t candidate = read<uint32_t>(buffer, cursor);
            if (candidate != 0)
            {
                pieceOffsets.push_back(candidate);
            }
            cursor += 4;
        }
        if (pieceOffsets.empty())
        {
            pieceOffsets.push_back(blockHeaderOffset);
        }
    }

    for (size_t offset : pieceOffsets)
    {
        const int32_t modelCount = read<int32_t>(buffer, offset);
        offset += 32; // count, this block's own bounds, and a face count

        if (modelCount < 0 || modelCount > 256)
        {
            continue;
        }

        // Carried across the meshes of one piece, not across pieces.
        std::string lastTexture;
        for (int32_t i = 0; i < modelCount; ++i)
        {
            if (offset + 20 > buffer.size())
            {
                break;
            }

            ModelMesh mesh;
            mesh.texture = readName(buffer, offset, 16);

            // A blank name means "keep the last one", not "no texture".
            //
            // FFXI sets the texture with a command that applies to every
            // primitive after it until the next one, so a mesh that does not
            // name its own inherits whatever was set before it. Read as an
            // empty name it binds the white fallback instead, which is how a
            // shop ends up with one blank white panel among its walls.
            if (mesh.texture.empty())
            {
                mesh.texture = lastTexture;
            }
            else
            {
                lastTexture = mesh.texture;
            }
            const uint16_t vertexCount = read<uint16_t>(buffer, offset + 16);
            mesh.blending = read<uint16_t>(buffer, offset + 18);
            offset += 20;

            if (offset + static_cast<size_t>(vertexCount) * stride > buffer.size())
            {
                break;
            }

            mesh.vertices.reserve(vertexCount);
            for (uint16_t v = 0; v < vertexCount; ++v)
            {
                const size_t base = offset + static_cast<size_t>(v) * stride;
                const size_t normalAt = base + (layout == 2 ? 24 : 12);

                ModelVertex vertex{};
                for (int axis = 0; axis < 3; ++axis)
                {
                    vertex.position[axis] = read<float>(buffer, base + axis * 4);
                    vertex.normal[axis] = read<float>(buffer, normalAt + axis * 4);
                }
                vertex.colour = read<uint32_t>(buffer, normalAt + 12);
                vertex.uv[0] = read<float>(buffer, normalAt + 16);
                vertex.uv[1] = read<float>(buffer, normalAt + 20);
                mesh.vertices.push_back(vertex);
            }
            offset += static_cast<size_t>(vertexCount) * stride;

            const uint16_t indexCount = read<uint16_t>(buffer, offset);
            offset += 4; // the count is two bytes but the field is four

            if (offset + static_cast<size_t>(indexCount) * 2 > buffer.size())
            {
                break;
            }

            // MMB-tagged chunks and the longer vertex layout store a plain
            // triangle list; everything else is a strip.
            const bool triangleList = (chunk.id[0] == 'M' && chunk.id[1] == 'M' && chunk.id[2] == 'B') ||
                                      (chunk.id[0] != 'M' && layout == 2);

            if (triangleList)
            {
                mesh.indices.reserve(indexCount);
                for (uint16_t k = 0; k < indexCount; ++k)
                {
                    mesh.indices.push_back(read<uint16_t>(buffer, offset + static_cast<size_t>(k) * 2));
                }
            }
            else if (indexCount >= 3)
            {
                for (uint16_t k = 0; k + 2 < indexCount; ++k)
                {
                    const uint16_t a = read<uint16_t>(buffer, offset + static_cast<size_t>(k) * 2);
                    const uint16_t b = read<uint16_t>(buffer, offset + static_cast<size_t>(k + 1) * 2);
                    const uint16_t c = read<uint16_t>(buffer, offset + static_cast<size_t>(k + 2) * 2);
                    // A strip jumps between pieces using degenerate triangles.
                    if (a == b || b == c)
                    {
                        continue;
                    }
                    // Every other triangle in a strip is wound the other way.
                    if (k % 2)
                    {
                        mesh.indices.push_back(b);
                        mesh.indices.push_back(a);
                        mesh.indices.push_back(c);
                    }
                    else
                    {
                        mesh.indices.push_back(a);
                        mesh.indices.push_back(b);
                        mesh.indices.push_back(c);
                    }
                }
            }
            offset += static_cast<size_t>(indexCount) * 2;
            // The index block is padded so the next mesh header starts on a
            // 4-byte boundary. Without this, any mesh following an odd index
            // count reads two bytes early and the rest of the model collapses -
            // which silently cost 1,238 of one zone's 4,918 placements.
            offset = (offset + 3) & ~static_cast<size_t>(3);

            model.meshes.push_back(std::move(mesh));
        }
    }

    return model;
}
} // namespace ffxi
