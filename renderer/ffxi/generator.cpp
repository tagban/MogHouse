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
constexpr uint8_t kOpHidden = 0x27;
constexpr uint8_t kOpDistanceFade = 0x48;
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

        // Four streams, each running to the next stream's start (or the chunk's
        // end). The length byte carries flags in its top three bits - 0xa4 is
        // four words, 0xa1 one - and op 0x00 is a one-word nop, not an end:
        // read as an end, and with the flags taken as length, everything after
        // a lamp glow's op 0x12 was lost, including its curve and night flag.
        uint32_t starts[4] = {};
        for (int section = 0; section < 4; ++section)
        {
            readAt(data, kSectionTable - kHeader + section * 4, starts[section]);
        }
        for (int section = 0; section < 4; ++section)
        {
            const uint32_t start = starts[section];
            if (start < kHeader || start - kHeader >= data.size())
            {
                continue;
            }
            size_t end = data.size();
            for (int other = 0; other < 4; ++other)
            {
                if (starts[other] > start && starts[other] - kHeader < end)
                {
                    end = starts[other] - kHeader;
                }
            }
            size_t pos = start - kHeader;
            for (int guard = 0; guard < 200 && pos + 4 <= end; ++guard)
            {
                const uint8_t op = data[pos];
                const size_t words = data[pos + 1] & 0x1f;
                if (words == 0)
                {
                    break;
                }
                const size_t length = words * 4;
                if (pos + length > end)
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
                    if (length >= 12 && placement.textureAnimation.empty())
                    {
                        placement.textureAnimation = fourChars(data, payload + 4);
                    }
                    break;
                case kOpNightOnly:
                    placement.nightOnly = true;
                    break;
                case kOpHidden:
                    placement.hidden = true;
                    break;
                case kOpScroll:
                    if (length >= 8)
                    {
                        readAt(data, payload, placement.scroll);
                    }
                    break;
                case kOpDistanceFade:
                    if (length >= 20)
                    {
                        for (int i = 0; i < 4; ++i)
                        {
                            readAt(data, payload + i * 4, placement.fade[i]);
                        }
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

float IntensityCurve::at(float dayFraction) const
{
    if (keys.empty())
    {
        return 1.0f;
    }
    if (dayFraction <= keys.front().first)
    {
        return keys.front().second;
    }
    for (size_t i = 1; i < keys.size(); ++i)
    {
        if (dayFraction <= keys[i].first)
        {
            const float span = keys[i].first - keys[i - 1].first;
            const float t = span > 1e-6f ? (dayFraction - keys[i - 1].first) / span : 1.0f;
            return keys[i - 1].second + (keys[i].second - keys[i - 1].second) * t;
        }
    }
    return keys.back().second;
}

std::unordered_map<std::string, IntensityCurve> parseIntensityCurves(const DatFile& dat)
{
    std::unordered_map<std::string, IntensityCurve> out;
    for (const Chunk& chunk : dat.chunksOfType(0x19))
    {
        IntensityCurve curve;
        const std::span<const uint8_t> data = chunk.data;
        float lastTime = -1.0f;
        for (size_t pos = 0; pos + 8 <= data.size(); pos += 8)
        {
            float time = 0.0f, value = 0.0f;
            readAt(data, pos, time);
            readAt(data, pos + 4, value);
            // Time runs forward; a pair that goes back to zero is padding.
            if (time < lastTime || time < 0.0f || time > 1.0f)
            {
                break;
            }
            curve.keys.emplace_back(time, value);
            lastTime = time;
        }
        if (!curve.keys.empty())
        {
            out[fourChars(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(chunk.id), 4), 0)] =
                std::move(curve);
        }
    }
    return out;
}
} // namespace ffxi
