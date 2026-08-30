#pragma once

// Reading the FFXI DAT container. Written from the format as documented in
// docs/mzb-format.md, which was established by reading the retail files.

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace ffxi
{
// Chunk type codes. Only the ones we actually use are named.
inline constexpr uint8_t kChunkEnd = 0x00;
inline constexpr uint8_t kChunkDirectory = 0x01;
inline constexpr uint8_t kChunkMzb = 0x1C;
inline constexpr uint8_t kChunkSk2 = 0x29;
inline constexpr uint8_t kChunkMmb = 0x2E;

struct Chunk
{
    char id[4];
    uint8_t type;
    /// The chunk's payload, header excluded.
    std::span<const uint8_t> data;
};

/// A whole .DAT held in memory, with its chunks located.
///
/// Chunks form a tree, but for the uses here a flat list is enough - the tree
/// structure only matters for chunk kinds we do not read yet.
class DatFile
{
public:
    /// Throws std::runtime_error if the file cannot be read.
    explicit DatFile(const std::filesystem::path& path);

    const std::vector<Chunk>& chunks() const { return chunks_; }

    /// Every chunk of a given type. Some DATs hold more than one MZB - the
    /// ferry zones carry both the world and the vessel - so this returns all of
    /// them rather than the first.
    std::vector<Chunk> chunksOfType(uint8_t type) const;

private:
    std::vector<uint8_t> buffer_;
    std::vector<Chunk> chunks_;
};
} // namespace ffxi
