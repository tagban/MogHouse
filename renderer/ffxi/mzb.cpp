#include "mzb.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace ffxi
{
namespace
{
/// Placement records come in two sizes. Which one a file uses is not stated,
/// but it can be worked out: the header word at 20 holds where the table ends,
/// so the size that lands exactly there is the right one.
constexpr size_t kPlacementSizeLong = 0x64;
constexpr size_t kPlacementSizeShort = 0x54;
constexpr size_t kPlacementsOffset = 32;
constexpr uint16_t kIndexMask = 0x3FFF;
constexpr uint8_t kFirstEncryptedVersion = 0x1B;

template <typename T> T read(const std::vector<uint8_t>& buffer, size_t offset);

/// How wide a placement record is in this file.
///
/// The header word at 20 is where the placement table ends, so the stride that
/// lands the last record exactly there is the one in use. Bastok Markets says
/// 102232, and 32 + 1022 * 0x64 is 102232.
///
/// Guessed wrong, every field after the first record reads from the middle of
/// its neighbour - which does not throw, it just produces a zone quietly built
/// out of noise.
inline size_t placementStride(const std::vector<uint8_t>& buffer, uint32_t count)
{
    if (buffer.size() >= 24 && count > 0)
    {
        uint32_t endsAt = 0;
        std::memcpy(&endsAt, buffer.data() + 20, sizeof(endsAt));
        if (kPlacementsOffset + static_cast<size_t>(count) * kPlacementSizeShort == endsAt)
        {
            return kPlacementSizeShort;
        }
    }
    return kPlacementSizeLong;
}

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
    const size_t kPlacementSize = placementStride(buffer, count);
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
    const size_t kPlacementSize = placementStride(buffer, placementCount);

    // The header is 32 bytes and only two of its fields are used - the
    // placement count at 4 and the mesh table at 8. Whatever else it points at
    // is where the geometry this cannot find must be: the fountain's water
    // model is in the DAT with no placement, and so are the sky sphere and the
    // collision proxies.
    // The decrypted buffer, so the regions the parser does not understand can
    // be picked apart outside it.
    if (const char* dumpTo = std::getenv("MOGHOUSE_MZB_DUMP"))
    {
        std::ofstream out{dumpTo, std::ios::binary};
        out.write(reinterpret_cast<const char*>(buffer.data()),
                  static_cast<std::streamsize>(buffer.size()));
        std::printf("wrote %zu decrypted bytes to %s\n", buffer.size(), dumpTo);
    }

    if (std::getenv("MOGHOUSE_MZB_HEADER"))
    {
        std::printf("MZB header, as 32-bit words:\n");
        for (size_t word = 0; word < 8; ++word)
        {
            const uint32_t value = read<uint32_t>(buffer, word * 4);
            std::printf("  [%2zu] 0x%08X  %11u%s\n", word * 4, value, value,
                        value != 0 && value < buffer.size() ? "   <- inside the chunk" : "");
        }
        std::printf("  chunk is %zu bytes; placements end at %zu\n", buffer.size(),
                    kPlacementsOffset + static_cast<size_t>(placementCount) * kPlacementSize);

        const size_t placementsEnd =
            kPlacementsOffset + static_cast<size_t>(placementCount) * kPlacementSize;
        const size_t spots[] = { placementsEnd, read<uint32_t>(buffer, 16),
                                 read<uint32_t>(buffer, 24) };
        const char* labels[] = { "after the placements", "word 16", "word 24" };
        for (size_t s = 0; s < 3; ++s)
        {
            std::printf("  %s, at %zu:\n", labels[s], spots[s]);
            for (size_t row = 0; row < 6; ++row)
            {
                const size_t at = spots[s] + row * 16;
                if (at + 16 > buffer.size()) break;
                std::printf("    %8zu  ", at);
                for (size_t b = 0; b < 16; ++b) std::printf("%02X ", buffer[at + b]);
                std::printf(" ");
                for (size_t b = 0; b < 16; ++b)
                {
                    const uint8_t c = buffer[at + b];
                    std::printf("%c", c >= 32 && c < 127 ? c : '.');
                }
                std::printf("\n");
            }
            std::printf("    floats: ");
            for (size_t f = 0; f < 8; ++f)
                std::printf("%.3f ", read<float>(buffer, spots[s] + f * 4));
            std::printf("\n    u32:    ");
            for (size_t f = 0; f < 8; ++f)
                std::printf("%u ", read<uint32_t>(buffer, spots[s] + f * 4));
            std::printf("\n");
        }
    }
    zone.placements.reserve(placementCount);
    for (uint32_t i = 0; i < placementCount; ++i)
    {
        const size_t base = kPlacementsOffset + i * kPlacementSize;
        Placement placement;
        // Names are space padded, not null terminated, and MMB trims the same
        // way - without this every lookup misses by trailing whitespace.
        const char* name = reinterpret_cast<const char*>(buffer.data() + base);
        placement.model.assign(name, ::strnlen(name, 16));
        while (!placement.model.empty() && placement.model.back() == ' ')
        {
            placement.model.pop_back();
        }
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

    // Instances reference a mesh by the offset of its table entry, so remember
    // where each one started.
    std::unordered_map<uint32_t, uint32_t> meshByOffset;
    for (uint32_t i = 0; i < meshCount; ++i)
    {
        size_t next = 0;
        meshByOffset[static_cast<uint32_t>(entry)] = static_cast<uint32_t>(zone.collision.size());
        zone.collision.push_back(readMesh(buffer, entry, next));
        entry = next;
    }

    // The meshes are model space and reused. Where each copy goes is held in a
    // spatial grid: a 2D array of offsets, each pointing at a list of
    // (placement, mesh) pairs. Without this every mesh sits at the origin.
    const uint32_t gridTable = read<uint32_t>(buffer, meshTable + 0x10);
    if (gridTable == 0)
    {
        return zone;
    }

    const uint8_t gridWidth = buffer[12];
    const uint8_t gridHeight = buffer[13];
    const uint8_t bucketWidth = buffer[14];
    const uint8_t bucketHeight = buffer[15];
    const int cellsAcross = gridWidth * bucketWidth / 4;
    const int cellsDown = gridHeight * bucketHeight / 4;

    for (int y = 0; y < cellsDown; ++y)
    {
        for (int x = 0; x < cellsAcross; ++x)
        {
            const size_t cell = gridTable + static_cast<size_t>(y * cellsAcross + x) * 4;
            const uint32_t listOffset = read<uint32_t>(buffer, cell);
            if (listOffset == 0)
            {
                continue;
            }

            // The low 14 bits are how many entries follow; the rest is a
            // position we do not need, since the transform carries it.
            const uint32_t count = read<uint32_t>(buffer, listOffset) & 0x3FFF;
            size_t pair = listOffset + 4;
            for (uint32_t i = 0; i < count; ++i, pair += 8)
            {
                const uint32_t placementOffset = read<uint32_t>(buffer, pair);
                const uint32_t meshOffset = read<uint32_t>(buffer, pair + 4);

                auto found = meshByOffset.find(meshOffset);
                if (found == meshByOffset.end())
                {
                    continue;
                }

                CollisionInstance instance{};
                instance.mesh = found->second;

                // Fixed point with the top six and bottom four bits carrying
                // something else: shift left to put the sign where it belongs,
                // then back plus four to drop the low bits.
                const int32_t packed = read<int32_t>(buffer, placementOffset + 164);
                instance.waterHeight = static_cast<float>((packed << 6) >> 10) / 1024.0f;
                // Straight copy, no transpose. Transposing puts elements 3, 7
                // and 11 - the constant (0,0,0,1) column - into the translation
                // slot, which reads as every instance sitting exactly on the
                // origin.
                for (int i = 0; i < 16; ++i)
                {
                    instance.transform[i] = read<float>(buffer, placementOffset + static_cast<size_t>(i) * sizeof(float));
                }
                zone.instances.push_back(instance);
            }
        }
    }

    return zone;
}
} // namespace ffxi
