#pragma once

// What the people in a zone say.
//
// Every zone has a dialogue file at `6420 + zone` in the file table, holding
// its entire script as numbered entries - greetings, shop patter, quest text,
// and the menus that go with them. An event says which entry to show; this
// says what the entry is.
//
// The format is a table of offsets followed by the text, both obfuscated by
// XOR and neither compressed. See docs/wiki/Dialogue.md.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ffxi
{
/// One entry: what is said, and what can be said back.
struct DialogueEntry
{
    /// The text, with its line breaks kept as '\n'.
    std::string text;

    /// The choices offered under it, in order. Empty when it is only speech.
    std::vector<std::string> options;
};

/// Every entry in one zone's dialogue file, in order, or empty if it will not
/// read. Entries are addressed by position - that is what an event refers to.
std::vector<DialogueEntry> readDialogue(const std::filesystem::path& path);

/// The file id holding a zone's dialogue.
inline uint32_t dialogueFileId(uint32_t zone) { return 6420 + zone; }
} // namespace ffxi
