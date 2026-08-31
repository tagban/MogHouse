#include "textfont.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace
{
/// Reads the whole file, or nothing.
std::vector<uint8_t> readAll(const std::filesystem::path& path)
{
    std::ifstream file{path, std::ios::binary};
    if (!file)
    {
        return {};
    }
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
} // namespace

mh::TextFont mh::loadTextFont(const std::string& directory)
{
    namespace fs = std::filesystem;

    const char* override = std::getenv("MOGHOUSE_FONT");
    const fs::path base = override ? fs::path{override} : fs::path{directory};

    TextFont font;

    std::ifstream metrics{base / "font.txt"};
    if (!metrics)
    {
        // Worth saying out loud: without the atlas every label, the compass
        // and the chat panel silently vanish, and the window looks like the
        // HUD was never written.
        std::printf("no font atlas at %s - the HUD will not draw. Set MOGHOUSE_FONT.\n", base.string().c_str());
        return font;
    }

    std::string word;
    while (metrics >> word)
    {
        if (word == "atlas")
        {
            metrics >> font.width >> font.height >> font.cell >> font.columns >> font.firstChar >> font.charCount;
            font.advance.assign(font.charCount, static_cast<float>(font.cell));
        }
        else if (word == "glyph")
        {
            uint32_t code = 0;
            float advance = 0.0f;
            metrics >> code >> advance;
            if (code >= font.firstChar && code - font.firstChar < font.advance.size())
            {
                font.advance[code - font.firstChar] = advance;
            }
        }
        else
        {
            // A comment line, or a key from a newer generator than this reader.
            std::string rest;
            std::getline(metrics, rest);
        }
    }

    if (font.width == 0 || font.height == 0)
    {
        return TextFont{};
    }

    // The generator writes BGRA, which is what Windows hands back from a
    // locked bitmap. The GPU is told RGBA, so the two channels that matter -
    // the glyph and its outline - are swapped into place here rather than in
    // every shader that samples them.
    std::vector<uint8_t> raw = readAll(base / "font.bin");
    const size_t expected = static_cast<size_t>(font.width) * font.height * 4;
    if (raw.size() < expected)
    {
        return TextFont{};
    }

    font.pixels.resize(expected);
    for (size_t texel = 0; texel < expected; texel += 4)
    {
        font.pixels[texel + 0] = raw[texel + 2]; // glyph, written to blue by GDI+
        font.pixels[texel + 1] = raw[texel + 1]; // outline
        font.pixels[texel + 2] = raw[texel + 0];
        font.pixels[texel + 3] = raw[texel + 3];
    }

    std::printf("font: %ux%u atlas, %u glyphs from %u, cell %u\n", font.width, font.height, font.charCount,
                font.firstChar, font.cell);
    return font;
}
