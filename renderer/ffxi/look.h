#pragma once

// What a player character is wearing, and which files that comes from.
//
// The layout was derived rather than transcribed: see tools/pcmodels.py, which
// scores every candidate race base against an index of every DAT holding a
// skinned mesh and requires the winner to be the file holding that race's
// skeleton. All eight races resolve.

#include "filetable.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ffxi
{
enum class Race : uint8_t
{
    HumeMale = 1,
    HumeFemale,
    ElvaanMale,
    ElvaanFemale,
    TarutaruMale,
    TarutaruFemale,
    Mithra,
    Galka,
};

enum class LookSlot : uint8_t
{
    Face,
    Head,
    Body,
    Hands,
    Legs,
    Feet,
    Count
};

/// What a character is wearing. Model ids, not item ids: the item table maps
/// one to the other and is a separate problem.
struct Look
{
    Race race{Race::HumeMale};
    std::array<uint16_t, static_cast<size_t>(LookSlot::Count)> model{};
};

/// The file id holding this race's skeleton and animation set.
size_t skeletonFileId(Race race);

/// The file id for one slot's model, or 0 if the race or slot is unknown.
///
/// Only the first block is resolved - model ids 0 to 255, which is the gear
/// the game shipped with. Later expansions added blocks elsewhere in the file
/// table that this does not yet reach.
size_t modelFileId(Race race, LookSlot slot, uint16_t modelId);

/// Every file a look needs, skipping slots that resolve to nothing.
std::vector<std::filesystem::path> lookFiles(const FileTable& table, const Look& look);

/// Parses "race,face,head,body,hands,legs,feet" - the shape a look arrives in
/// from the server. Returns false if it does not have seven numbers.
bool parseLook(const std::string& text, Look& look);

const char* raceName(Race race);
const char* slotName(LookSlot slot);
} // namespace ffxi
