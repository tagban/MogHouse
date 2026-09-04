#pragma once

// What sounds a DAT says it uses.
//
// A model, a zone or an effect declares its own sounds as 0x3D chunks, and the
// chunk's four-character name says what each one is for. So there is no table
// mapping creatures to sounds anywhere in the game, and there does not need to
// be one here: the creature is asked.
//
//   idl1 idl2      standing about        atk1-atk4  attacking
//   dam1-dam4      taking a hit          swy1-swy3  swaying
//   ded1-ded3      dying                 sdam skaz shit  shared weapon sounds
//
// Anything outside that vocabulary is worth looking at. The worm's two extras
// are 17024 and 17025 - coming out of the ground and going back under - and
// finding them is what made the burrower list a query instead of a guess.
//
// See docs/wiki/Audio-Formats.md.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ffxi
{
/// One sound a DAT declares.
struct SoundRef
{
    /// The chunk's own four-character name - what the sound is for.
    std::string name;

    /// The sound's number, which is also its path.
    uint32_t id{};

    /// The directory chunks enclosing it, joined with '/' - the same string a
    /// generator records, and deliberately so. A sound sharing a directory
    /// with generators is ambience the generators give a position to:
    /// `f_ro/mode/ligh/taki` holds one sound and fifty-six placements of it,
    /// and `taki` is Japanese for waterfall.
    std::string directory;

    /// `se{id/1000:03d}/se{id:06d}.spw`, relative to `sound/win`.
    std::filesystem::path file() const;
};

/// Every 0x3D reference in a DAT, in the order they appear.
std::vector<SoundRef> soundReferences(const std::filesystem::path& datPath);

/// The same, from bytes already in hand.
std::vector<SoundRef> soundReferences(const std::vector<uint8_t>& dat);

/// The sounds every creature declares, so anything else is worth a look.
bool isStandardCreatureSound(const std::string& name);
} // namespace ffxi
