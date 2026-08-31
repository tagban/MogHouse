#pragma once

// The glyph atlas the renderer draws all its text with.
//
// Built by tools/makefont.ps1 from a real typeface rather than taken from
// FFXI's own font DATs. The game's font is a 2002 bitmap and inheriting it
// means inheriting its size, its weight and its edges; this way the text can
// be as sharp as the window is.
//
// Two coverages per texel: red is the glyph interior, green is the outline
// around it. Keeping them apart is what lets one atlas serve every colour a
// name might need - a party member blue, a GM red, a monster yellow - with the
// outline staying black behind all of them.

#include <cstdint>
#include <string>
#include <vector>

namespace mh
{
/// A loaded atlas: the pixels, its shape, and how wide each glyph is.
struct TextFont
{
    std::vector<uint8_t> pixels;  ///< RGBA8, row major
    uint32_t width{};
    uint32_t height{};

    /// Glyphs are laid out in a fixed grid, so a character's cell is found by
    /// arithmetic rather than by a table of rectangles.
    uint32_t cell{};
    uint32_t columns{};

    /// The first character in the atlas, and how many follow it.
    uint32_t firstChar{};
    uint32_t charCount{};

    /// How far the pen moves after each glyph, in atlas pixels. Indexed from
    /// firstChar. Without this every glyph occupies a full cell and the text
    /// reads as a ransom note.
    std::vector<float> advance;

    bool empty() const { return pixels.empty(); }

    /// Where a character sits in the atlas, as a cell index, or the space
    /// glyph for anything outside the range.
    uint32_t indexOf(char raw) const
    {
        const auto code = static_cast<uint32_t>(static_cast<unsigned char>(raw));
        return code >= firstChar && code < firstChar + charCount ? code - firstChar : 0;
    }

    float advanceOf(char raw) const
    {
        const uint32_t index = indexOf(raw);
        return index < advance.size() ? advance[index] : static_cast<float>(cell);
    }
};

/// Reads the atlas beside the executable, or wherever MOGHOUSE_FONT points.
///
/// Returns an empty font if it is not there. Text is worth doing without
/// rather than failing to open a window over.
TextFont loadTextFont(const std::string& directory);
} // namespace mh
