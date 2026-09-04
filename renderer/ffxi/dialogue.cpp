#include "dialogue.h"

#include <cstring>
#include <fstream>

namespace ffxi
{
namespace
{
/// The whole file is obfuscated with this, offsets and text alike - the
/// offsets a dword at a time, the text a byte at a time. Not encryption: one
/// constant, no key, and the same for every zone.
constexpr uint32_t kOffsetMask = 0x80808080u;
constexpr uint8_t kTextMask = 0x80u;

/// Where the offset table starts. The first dword is something else - it is
/// not a count, and the count is worked out from the first offset instead,
/// since the table runs right up to the text it points at.
constexpr size_t kTableStart = 4;

/// In the decoded text.
constexpr char kNewline = 0x07;    ///< a line break within one entry
constexpr char kOptions = 0x0B;    ///< everything after this is a menu
constexpr char kEnd = 0x7F;        ///< the entry stops here, whatever follows

uint32_t readDword(const std::vector<uint8_t>& data, size_t at)
{
    uint32_t value = 0;
    std::memcpy(&value, data.data() + at, sizeof(value));
    return value;
}
} // namespace

std::vector<DialogueEntry> readDialogue(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        return {};
    }
    std::vector<uint8_t> data{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    if (data.size() < kTableStart + 8)
    {
        return {};
    }

    // The first offset is where the text begins, so it is also the length of
    // the table - and dividing by four gives how many entries there are. The
    // file never says outright.
    const uint32_t firstOffset = readDword(data, kTableStart) ^ kOffsetMask;
    if (firstOffset < 4 || firstOffset % 4 != 0 || kTableStart + firstOffset > data.size())
    {
        return {};
    }
    const size_t count = firstOffset / 4;

    std::vector<DialogueEntry> entries;
    entries.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const size_t at = kTableStart + i * 4;
        const uint32_t start = readDword(data, at) ^ kOffsetMask;
        const uint32_t stop =
            i + 1 < count ? (readDword(data, at + 4) ^ kOffsetMask) : static_cast<uint32_t>(data.size() - kTableStart);
        if (start > stop || kTableStart + stop > data.size())
        {
            entries.push_back({});
            continue;
        }

        DialogueEntry entry;
        std::string* into = &entry.text;
        for (size_t b = kTableStart + start; b < kTableStart + stop; ++b)
        {
            const char decoded = static_cast<char>(data[b] ^ kTextMask);
            if (decoded == kEnd || decoded == '\0')
            {
                break;
            }
            if (decoded == kOptions)
            {
                // The rest is a menu, one choice per line break.
                entry.options.emplace_back();
                into = &entry.options.back();
                continue;
            }
            if (decoded == kNewline)
            {
                if (into == &entry.text)
                {
                    entry.text.push_back('\n');
                }
                else
                {
                    entry.options.emplace_back();
                    into = &entry.options.back();
                }
                continue;
            }
            into->push_back(decoded);
        }

        // A trailing break leaves an empty choice behind it.
        while (!entry.options.empty() && entry.options.back().empty())
        {
            entry.options.pop_back();
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}
} // namespace ffxi
