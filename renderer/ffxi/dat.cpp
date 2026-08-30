#include "dat.h"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace ffxi
{
namespace
{
constexpr size_t kHeaderSize = 16;

std::vector<uint8_t> readWholeFile(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary | std::ios::ate};
    if (!file)
    {
        throw std::runtime_error("could not open " + path.string());
    }

    const auto size = static_cast<size_t>(file.tellg());
    std::vector<uint8_t> buffer(size);
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(size));
    return buffer;
}
} // namespace

DatFile::DatFile(const std::filesystem::path& path) : buffer_(readWholeFile(path))
{
    size_t offset = 0;
    while (offset + kHeaderSize <= buffer_.size())
    {
        uint32_t packed = 0;
        std::memcpy(&packed, buffer_.data() + offset + 4, sizeof(packed));

        const auto type = static_cast<uint8_t>(packed & 0x7F);
        const size_t length = static_cast<size_t>((packed >> 7) & 0x7FFFF) * 16;

        // A chunk shorter than its own header would not advance the cursor.
        // Treat it as the end of anything we can trust rather than looping.
        if (length < kHeaderSize || offset + length > buffer_.size())
        {
            break;
        }

        Chunk chunk{};
        std::memcpy(chunk.id, buffer_.data() + offset, 4);
        chunk.type = type;
        chunk.data = std::span<const uint8_t>(buffer_.data() + offset + kHeaderSize, length - kHeaderSize);
        chunks_.push_back(chunk);

        offset += length;
    }
}

std::vector<Chunk> DatFile::chunksOfType(uint8_t type) const
{
    std::vector<Chunk> found;
    for (const Chunk& chunk : chunks_)
    {
        if (chunk.type == type)
        {
            found.push_back(chunk);
        }
    }
    return found;
}
} // namespace ffxi
