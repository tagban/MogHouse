#include "generator.h"

#include <cstring>

namespace ffxi
{
namespace
{
template <typename T>
bool readAt(std::span<const uint8_t> data, size_t offset, T& out)
{
    if (offset + sizeof(T) > data.size())
    {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

std::string fourChars(std::span<const uint8_t> data, size_t offset)
{
    std::string s;
    for (size_t i = 0; i < 4 && offset + i < data.size(); ++i)
    {
        const char c = static_cast<char>(data[offset + i]);
        if (c == 0)
        {
            break;
        }
        s.push_back(c);
    }
    while (!s.empty() && s.back() == ' ')
    {
        s.pop_back();
    }
    return s;
}

/// Offsets in the generator are from the start of the chunk, header included;
/// the Chunk's span starts after the sixteen-byte header.
constexpr size_t kHeader = 16;

// The generator is a fixed header followed by four opcode streams, each
// starting at an offset in a table at 0x80 (chunk-relative). An opcode is a
// byte, then its length in four-byte words including itself, then two bytes of
// padding, then the payload. Op 0x00 ends a stream. The ones read here:
//
//   0x01  len 12   +4 flags, +8 model id (four chars), +16 position x y z
//   0x09  len 4    rotation x y z, radians
//   0x0f  len 4    scale x y z
//   0x63  len 4    +4 texture animation id (four chars)
//
// Read from East Ronfaure's kawa (river) generators and Bastok Markets' sea
// and funs (fountain) ones; the rest of the opcodes - emission rate, lifetime,
// colour, the particle behaviours - are not needed to put the model where it
// goes.
constexpr size_t kSectionTable = 0x80;
constexpr uint8_t kOpModel = 0x01;
constexpr uint8_t kOpRotate = 0x09;
constexpr uint8_t kOpScale = 0x0f;
constexpr uint8_t kOpTexture = 0x63;
constexpr uint8_t kOpNightOnly = 0x0d;
constexpr uint8_t kOpScroll = 0x28;
} // namespace

std::vector<EffectPlacement> parseGenerators(const DatFile& dat)
{
    std::vector<EffectPlacement> out;
    // Walked in file order so the directory each generator sits in is known:
    // type 0x01 opens a directory, 0x00 closes it.
    std::vector<std::string> path;
    for (const Chunk& chunk : dat.chunks())
    {
        if (chunk.type == 0x00)
        {
            if (!path.empty())
            {
                path.pop_back();
            }
            continue;
        }
        if (chunk.type == 0x01)
        {
            path.push_back(fourChars(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(chunk.id), 4), 0));
            continue;
        }
        if (chunk.type != 0x05)
        {
            continue;
        }
        const std::span<const uint8_t> data = chunk.data;
        if (data.size() < kSectionTable + 16 - kHeader)
        {
            continue;
        }

        EffectPlacement placement;
        placement.generator = fourChars(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(chunk.id), 4), 0);
        for (size_t i = 0; i < path.size(); ++i)
        {
            placement.directory += (i ? "/" : "") + path[i];
        }
        bool hasModel = false;

        for (int section = 0; section < 4; ++section)
        {
            uint32_t start = 0;
            if (!readAt(data, kSectionTable - kHeader + section * 4, start) || start < kHeader)
            {
                continue;
            }
            size_t pos = start - kHeader;
            for (int guard = 0; guard < 100 && pos + 4 <= data.size(); ++guard)
            {
                const uint8_t op = data[pos];
                const uint8_t words = data[pos + 1];
                if (op == 0 || words == 0)
                {
                    break;
                }
                const size_t length = static_cast<size_t>(words) * 4;
                if (pos + length > data.size())
                {
                    break;
                }
                const size_t payload = pos + 4;
                switch (op)
                {
                case kOpModel:
                    if (length >= 32)
                    {
                        placement.modelId = fourChars(data, payload + 8);
                        hasModel = !placement.modelId.empty() && readAt(data, payload + 16, placement.translate[0]) &&
                                   readAt(data, payload + 20, placement.translate[1]) &&
                                   readAt(data, payload + 24, placement.translate[2]);
                    }
                    break;
                case kOpRotate:
                    if (length >= 16)
                    {
                        readAt(data, payload, placement.rotate[0]);
                        readAt(data, payload + 4, placement.rotate[1]);
                        readAt(data, payload + 8, placement.rotate[2]);
                    }
                    break;
                case kOpScale:
                    if (length >= 16)
                    {
                        readAt(data, payload, placement.scale[0]);
                        readAt(data, payload + 4, placement.scale[1]);
                        readAt(data, payload + 8, placement.scale[2]);
                    }
                    break;
                case kOpTexture:
                    if (length >= 12)
                    {
                        placement.textureAnimation = fourChars(data, payload + 4);
                    }
                    break;
                case kOpNightOnly:
                    placement.nightOnly = true;
                    break;
                case kOpScroll:
                    if (length >= 8)
                    {
                        readAt(data, payload, placement.scroll);
                    }
                    break;
                default:
                    break;
                }
                pos += length;
            }
        }

        if (hasModel)
        {
            out.push_back(std::move(placement));
        }
    }
    return out;
}
} // namespace ffxi
