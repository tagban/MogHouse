#include "look.h"

#include <cstdio>

namespace ffxi
{
namespace
{
/// The first file id of each race's block. The skeleton sits at the base and
/// everything else is measured from it.
///
/// Tarutaru share one block: the two sexes are the same skeleton with
/// different models, which is why the same number appears twice.
constexpr size_t kRaceBase[] = {
    0,     // no race 0
    7072,  // hume male
    10248, // hume female
    13424, // elvaan male
    16600, // elvaan female
    19776, // tarutaru male
    19776, // tarutaru female
    23176, // mithra
    26352, // galka
};

struct SlotWindow
{
    size_t offset;
    uint16_t count;
};

/// Offset from the race base and how many model ids each window holds, in the
/// order the slots appear in the file table.
constexpr SlotWindow kSlotWindow[] = {
    {8, 32},     // the character's own head: face and hair
    {40, 256},   // headgear
    {296, 256},  // body
    {552, 256},  // hands
    {808, 256},  // legs
    {1064, 256}, // feet
};
} // namespace

size_t skeletonFileId(Race race)
{
    const auto index = static_cast<size_t>(race);
    return index < std::size(kRaceBase) ? kRaceBase[index] : 0;
}

size_t modelFileId(Race race, LookSlot slot, uint16_t modelId)
{
    const size_t base = skeletonFileId(race);
    const auto slotIndex = static_cast<size_t>(slot);
    if (base == 0 || slotIndex >= std::size(kSlotWindow))
    {
        return 0;
    }

    const SlotWindow& window = kSlotWindow[slotIndex];
    if (modelId >= window.count)
    {
        return 0;
    }
    return base + window.offset + modelId;
}

std::vector<std::filesystem::path> lookFiles(const FileTable& table, const Look& look)
{
    std::vector<std::filesystem::path> paths;
    for (size_t i = 0; i < static_cast<size_t>(LookSlot::Count); ++i)
    {
        const size_t fileId = modelFileId(look.race, static_cast<LookSlot>(i), look.model[i]);
        if (fileId == 0)
        {
            continue;
        }
        if (auto path = table.path(fileId))
        {
            paths.push_back(*path);
        }
    }
    return paths;
}

bool parseLook(const std::string& text, Look& look)
{
    unsigned values[7] = {};
    if (std::sscanf(text.c_str(), "%u,%u,%u,%u,%u,%u,%u", &values[0], &values[1], &values[2], &values[3], &values[4],
                    &values[5], &values[6]) != 7)
    {
        return false;
    }
    if (values[0] < 1 || values[0] > 8)
    {
        return false;
    }

    look.race = static_cast<Race>(values[0]);
    for (size_t i = 0; i < static_cast<size_t>(LookSlot::Count); ++i)
    {
        look.model[i] = static_cast<uint16_t>(values[i + 1]);
    }
    return true;
}

const char* raceName(Race race)
{
    switch (race)
    {
    case Race::HumeMale: return "hume male";
    case Race::HumeFemale: return "hume female";
    case Race::ElvaanMale: return "elvaan male";
    case Race::ElvaanFemale: return "elvaan female";
    case Race::TarutaruMale: return "tarutaru male";
    case Race::TarutaruFemale: return "tarutaru female";
    case Race::Mithra: return "mithra";
    case Race::Galka: return "galka";
    default: return "?";
    }
}

const char* slotName(LookSlot slot)
{
    switch (slot)
    {
    case LookSlot::Face: return "face";
    case LookSlot::Head: return "head";
    case LookSlot::Body: return "body";
    case LookSlot::Hands: return "hands";
    case LookSlot::Legs: return "legs";
    case LookSlot::Feet: return "feet";
    default: return "?";
    }
}
} // namespace ffxi
