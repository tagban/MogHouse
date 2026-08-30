#include "mzb.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ffxi
{
namespace
{
constexpr size_t kPlacementSize = 0x64;
constexpr size_t kPlacementsOffset = 32;
constexpr uint16_t kIndexMask = 0x3FFF;
constexpr uint8_t kFirstEncryptedVersion = 0x1B;

template <typename T> T read(const std::vector<uint8_t>& buffer, size_t offset)
{
    if (offset + sizeof(T) > buffer.size())
    {
        throw std::runtime_error("MZB: read past end of chunk");
    }
    T value{};
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    return value;
}

/// Undoes the run-based obfuscation described in docs/mzb-format.md. Only runs
/// whose key happens to be odd are actually XORed, and the run length varies
/// with the key, so this cannot be done as a single pass over the buffer.
void decrypt(std::vector<uint8_t>& buffer, const KeyTable& keys)
{
    if (buffer.size() < 8 || buffer[3] < kFirstEncryptedVersion)
    {
        return;
    }

    const uint32_t length = read<uint32_t>(buffer, 0) & 0x00FFFFFF;
    if (length > buffer.size())
    {
        throw std::runtime_error("MZB: declared length exceeds the chunk");
    }

    uint32_t key = keys[buffer[7] ^ 0xFF];
    uint32_t counter = 0;
    uint32_t pos = 8;
    while (pos < length)
    {
        const uint32_t run = ((key >> 4) & 7) + 16;
        if ((key & 1) && pos + run < length)
        {
            for (uint32_t i = 0; i < run; ++i)
            {
                buffer[pos + i] ^= 0xFF;
            }
        }
        key += ++counter;
        pos += run;
    }

    // Model names get a second, separate pass.
    const uint32_t count = read<uint32_t>(buffer, 4) & 0x00FFFFFF;
    for (uint32_t i = 0; i < count; ++i)
    {
        const size_t base = kPlacementsOffset + i * kPlacementSize;
        if (base + 16 > buffer.size())
        {
            throw std::runtime_error("MZB: placement table runs past the chunk");
        }
        for (size_t j = 0; j < 16; ++j)
        {
            buffer[base + j] ^= 0x55;
        }
    }
}

CollisionMesh readMesh(const std::vector<uint8_t>& buffer, size_t entry, size_t& next)
{
    const auto vertexOffset = read<uint32_t>(buffer, entry + 0x00);
    const auto normalOffset = read<uint32_t>(buffer, entry + 0x04);
    const auto triangleOffset = read<uint32_t>(buffer, entry + 0x08);
    const auto triangleCount = read<uint16_t>(buffer, entry + 0x0C);

    if (normalOffset < vertexOffset || triangleOffset < normalOffset)
    {
        throw std::runtime_error("MZB: collision mesh offsets are out of order");
    }

    CollisionMesh mesh;
    mesh.flags = read<uint16_t>(buffer, entry + 0x0E);

    // Vertex and normal counts are implied by where the next block starts,
    // rather than being stored anywhere.
    const size_t vertexFloats = (normalOffset - vertexOffset) / sizeof(float);
    const size_t normalFloats = (triangleOffset - normalOffset) / sizeof(float);

    mesh.vertices.resize(vertexFloats);
    mesh.normals.resize(normalFloats);
    if (vertexFloats)
    {
        if (vertexOffset + vertexFloats * sizeof(float) > buffer.size())
        {
            throw std::runtime_error("MZB: vertex data runs past the chunk");
        }
        std::memcpy(mesh.vertices.data(), buffer.data() + vertexOffset, vertexFloats * sizeof(float));
    }
    if (normalFloats)
    {
        std::memcpy(mesh.normals.data(), buffer.data() + normalOffset, normalFloats * sizeof(float));
    }

    // Four uint16 per triangle; three are indices, and the top two bits of each
    // carry something we have not identified.
    mesh.indices.reserve(static_cast<size_t>(triangleCount) * 3);
    for (uint16_t i = 0; i < triangleCount; ++i)
    {
        const size_t base = triangleOffset + static_cast<size_t>(i) * 8;
        for (size_t corner = 0; corner < 3; ++corner)
        {
            mesh.indices.push_back(read<uint16_t>(buffer, base + corner * 2) & kIndexMask);
        }
    }

    next = triangleOffset + static_cast<size_t>(triangleCount) * 8;
    return mesh;
}
} // namespace

std::optional<KeyTable> KeyTable::load(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file || static_cast<size_t>(file.tellg()) != 256)
    {
        return std::nullopt;
    }

    KeyTable table;
    file.seekg(0);
    file.read(reinterpret_cast<char*>(table.bytes_.data()), 256);
    return table;
}

Zone parseMzb(const Chunk& chunk, const KeyTable& keys)
{
    std::vector<uint8_t> buffer(chunk.data.begin(), chunk.data.end());
    decrypt(buffer, keys);

    Zone zone;
    zone.id.assign(chunk.id, chunk.id + 4);
    zone.version = buffer.size() > 3 ? buffer[3] : 0;

    const uint32_t placementCount = read<uint32_t>(buffer, 4) & 0x00FFFFFF;
    zone.placements.reserve(placementCount);
    for (uint32_t i = 0; i < placementCount; ++i)
    {
        const size_t base = kPlacementsOffset + i * kPlacementSize;
        Placement placement;
        const char* name = reinterpret_cast<const char*>(buffer.data() + base);
        placement.model.assign(name, ::strnlen(name, 16));
        std::memcpy(placement.translate, buffer.data() + base + 16, sizeof(placement.translate));
        std::memcpy(placement.rotate, buffer.data() + base + 28, sizeof(placement.rotate));
        std::memcpy(placement.scale, buffer.data() + base + 40, sizeof(placement.scale));
        zone.placements.push_back(std::move(placement));
    }

    // 0 is the sentinel for "no collision at all", not a table at offset 0.
    // The ferry zones use it: the sea has no collision because the collision
    // belongs to the vessel, which is a separate MZB in the same DAT.
    const uint32_t meshTable = read<uint32_t>(buffer, 8);
    if (meshTable == 0)
    {
        return zone;
    }

    const uint32_t meshCount = read<uint32_t>(buffer, meshTable);
    size_t entry = read<uint32_t>(buffer, meshTable + 4);
    zone.collision.reserve(meshCount);
    for (uint32_t i = 0; i < meshCount; ++i)
    {
        size_t next = 0;
        zone.collision.push_back(readMesh(buffer, entry, next));
        entry = next;
    }

    return zone;
}
} // namespace ffxi
