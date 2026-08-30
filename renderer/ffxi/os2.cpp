#include "os2.h"

#include <cstring>
#include <stdexcept>

namespace ffxi
{
namespace
{
// Every offset and size in the header counts 16-bit words from the start of
// the chunk. Reading them as byte offsets lands in the middle of the draw
// stream and looks almost plausible, which is worse than failing outright.
struct Header
{
    uint16_t flags{}; ///< bit 7 selects the bone table; the low bits gate normals
    uint16_t mirror{};
    uint32_t drawOffset{};
    uint16_t drawSize{};
    uint32_t boneRefOffset{};
    uint16_t boneRefCount{};
    uint32_t weightedCountOffset{};
    uint32_t weightOffset{};
    uint16_t weightCount{};
    uint32_t vertexOffset{};
    uint16_t vertexSize{};
};

template <typename T> T read(const std::span<const uint8_t>& data, size_t offset)
{
    if (offset + sizeof(T) > data.size())
    {
        throw std::runtime_error("OS2: read past the end of the chunk");
    }
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

Header readHeader(const std::span<const uint8_t>& data)
{
    Header header{};
    header.flags = read<uint16_t>(data, 2);
    header.mirror = read<uint16_t>(data, 4);
    header.drawOffset = read<uint32_t>(data, 6);
    header.drawSize = read<uint16_t>(data, 10);
    header.boneRefOffset = read<uint32_t>(data, 12);
    header.boneRefCount = read<uint16_t>(data, 16);
    header.weightedCountOffset = read<uint32_t>(data, 18);
    header.weightOffset = read<uint32_t>(data, 24);
    header.weightCount = read<uint16_t>(data, 28);
    header.vertexOffset = read<uint32_t>(data, 30);
    header.vertexSize = read<uint16_t>(data, 34);
    return header;
}

std::string trimmed(const std::span<const uint8_t>& data, size_t offset, size_t length)
{
    if (offset + length > data.size())
    {
        throw std::runtime_error("OS2: name runs past the end of the chunk");
    }
    std::string text(reinterpret_cast<const char*>(data.data() + offset), length);
    const size_t nul = text.find(char{0});
    if (nul != std::string::npos)
    {
        text.resize(nul);
    }
    while (!text.empty() && text.back() == ' ')
    {
        text.pop_back();
    }
    return text;
}
} // namespace

size_t SkinnedModel::triangleCount() const
{
    size_t total = 0;
    for (const SkinnedPart& part : parts)
    {
        total += part.corners.size() / 3;
    }
    return total;
}

SkinnedModel parseOs2(const Chunk& chunk)
{
    const std::span<const uint8_t>& data = chunk.data;
    const Header header = readHeader(data);

    SkinnedModel model;
    model.name = std::string(chunk.id, 4);
    model.mirrored = header.mirror > 0;

    // --- the draw stream ---------------------------------------------------
    // A small command language: state, material, then runs of geometry. Every
    // command carries its own length, so an unrecognised one has to stop the
    // walk rather than desynchronise everything after it.
    size_t cursor = static_cast<size_t>(header.drawOffset) * 2;
    const size_t drawEnd = cursor + static_cast<size_t>(header.drawSize) * 2;
    if (drawEnd > data.size())
    {
        throw std::runtime_error("OS2: draw stream runs past the end of the chunk");
    }

    auto corner = [&](size_t base, size_t which) {
        SkinCorner c;
        c.vertex = read<uint16_t>(data, base + which * 2);
        c.uv[0] = read<float>(data, base + 6 + which * 8);
        c.uv[1] = read<float>(data, base + 6 + which * 8 + 4);
        return c;
    };

    auto part = [&]() -> SkinnedPart& {
        if (model.parts.empty())
        {
            model.parts.push_back({});
        }
        return model.parts.back();
    };

    while (cursor + 2 <= drawEnd)
    {
        const uint16_t command = read<uint16_t>(data, cursor);
        cursor += 2;

        if (command == 0x8010) // draw state
        {
            model.parts.push_back({});
            model.parts.back().specularExponent = read<float>(data, cursor + 36);
            model.parts.back().specularIntensity = read<float>(data, cursor + 40);
            cursor += 44;
        }
        else if (command == 0x8000) // set material
        {
            part().texture = trimmed(data, cursor, 16);
            cursor += 16;
        }
        else if (command == 0x0054) // triangle list
        {
            const uint16_t count = read<uint16_t>(data, cursor);
            cursor += 2;
            for (uint16_t i = 0; i < count; ++i)
            {
                for (size_t v = 0; v < 3; ++v)
                {
                    part().corners.push_back(corner(cursor, v));
                }
                cursor += 30;
            }
        }
        else if (command == 0x5453) // triangle strip
        {
            const uint16_t count = read<uint16_t>(data, cursor);
            cursor += 2;
            if (count == 0)
            {
                continue;
            }

            // A strip opens with a whole triangle and then adds one corner at
            // a time, so the count is triangles rather than corners.
            SkinCorner previous[2] = {corner(cursor, 1), corner(cursor, 2)};
            for (size_t v = 0; v < 3; ++v)
            {
                part().corners.push_back(corner(cursor, v));
            }
            cursor += 30;

            for (uint16_t i = 1; i < count; ++i)
            {
                SkinCorner next;
                next.vertex = read<uint16_t>(data, cursor);
                next.uv[0] = read<float>(data, cursor + 2);
                next.uv[1] = read<float>(data, cursor + 6);
                cursor += 10;

                part().corners.push_back(previous[0]);
                part().corners.push_back(previous[1]);
                part().corners.push_back(next);
                previous[0] = previous[1];
                previous[1] = next;
            }
        }
        else if (command == 0x4353)
        {
            cursor += 8 + static_cast<size_t>(read<uint16_t>(data, cursor)) * 2;
        }
        else if (command == 0x0043)
        {
            cursor += static_cast<size_t>(read<uint16_t>(data, cursor)) * 10;
        }
        else if (command == 0xFFFF)
        {
            break;
        }
        else
        {
            throw std::runtime_error("OS2: unknown draw command");
        }
    }

    // --- the vertices ------------------------------------------------------
    std::vector<uint16_t> boneTable;
    boneTable.reserve(header.boneRefCount);
    for (uint16_t i = 0; i < header.boneRefCount; ++i)
    {
        boneTable.push_back(read<uint16_t>(data, static_cast<size_t>(header.boneRefOffset) * 2 + i * 2));
    }

    if (header.weightedCountOffset == 0)
    {
        throw std::runtime_error("OS2: no weighted vertex counts");
    }
    const size_t countsBase = static_cast<size_t>(header.weightedCountOffset) * 2;
    const uint16_t oneWeight = read<uint16_t>(data, countsBase);
    const uint16_t twoWeight = read<uint16_t>(data, countsBase + 2);

    const bool hasNormals = (header.flags & 0x7F) == 0;
    const bool useBoneTable = (header.flags & 0x80) != 0;

    size_t vertexCursor = static_cast<size_t>(header.vertexOffset) * 2;
    size_t boneCursor = static_cast<size_t>(header.weightOffset) * 2;

    // Bone references are packed two to a 16-bit word: seven bits each, then
    // two for the mirror axis.
    auto takeBones = [&](SkinInfluence& influence) {
        const uint16_t packed = read<uint16_t>(data, boneCursor);
        boneCursor += 2;
        const uint16_t first = packed & 0x7F;
        const uint16_t second = (packed >> 7) & 0x7F;
        influence.bone = static_cast<uint8_t>(useBoneTable && first < boneTable.size() ? boneTable[first] : first);
        influence.boneMirror =
            static_cast<uint8_t>(useBoneTable && second < boneTable.size() ? boneTable[second] : second);
        influence.mirrorAxis = static_cast<uint8_t>((packed >> 14) & 0x3);
    };

    model.vertices.reserve(static_cast<size_t>(oneWeight) + twoWeight);

    for (uint16_t i = 0; i < oneWeight; ++i)
    {
        SkinVertex vertex;
        vertex.influences = 1;
        for (int c = 0; c < 3; ++c)
        {
            vertex.influence[0].position[c] = read<float>(data, vertexCursor + c * 4);
        }
        vertexCursor += 12;
        if (hasNormals)
        {
            for (int c = 0; c < 3; ++c)
            {
                vertex.influence[0].normal[c] = read<float>(data, vertexCursor + c * 4);
            }
            vertexCursor += 12;
        }
        // The single-weight run still consumes bone words in pairs; only the
        // first of each is used.
        takeBones(vertex.influence[0]);
        boneCursor += 2;
        vertex.influence[0].weight = 1.0f;
        model.vertices.push_back(vertex);
    }

    for (uint16_t i = 0; i < twoWeight; ++i)
    {
        SkinVertex vertex;
        vertex.influences = 2;

        // The two influences are interleaved component by component - x0 x1
        // y0 y1 z0 z1 - rather than stored one after the other.
        for (int c = 0; c < 3; ++c)
        {
            vertex.influence[0].position[c] = read<float>(data, vertexCursor + c * 8);
            vertex.influence[1].position[c] = read<float>(data, vertexCursor + c * 8 + 4);
        }
        vertexCursor += 24;

        vertex.influence[0].weight = read<float>(data, vertexCursor);
        vertex.influence[1].weight = read<float>(data, vertexCursor + 4);
        vertexCursor += 8;

        if (hasNormals)
        {
            for (int c = 0; c < 3; ++c)
            {
                vertex.influence[0].normal[c] = read<float>(data, vertexCursor + c * 8);
                vertex.influence[1].normal[c] = read<float>(data, vertexCursor + c * 8 + 4);
            }
            vertexCursor += 24;
        }

        takeBones(vertex.influence[0]);
        takeBones(vertex.influence[1]);
        model.vertices.push_back(vertex);
    }

    const size_t vertexEnd = static_cast<size_t>(header.vertexOffset) * 2 + static_cast<size_t>(header.vertexSize) * 2;
    if (vertexCursor > vertexEnd)
    {
        throw std::runtime_error("OS2: vertex data overran the size in the header");
    }
    return model;
}
} // namespace ffxi
