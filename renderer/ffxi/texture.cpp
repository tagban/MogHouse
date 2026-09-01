#include "texture.h"

#include <cstring>
#include <stdexcept>

namespace ffxi
{
namespace
{
// Header layout, verified against the retail files:
//   +0x00 u8       encoding flag
//   +0x01 char[16] name
//   +0x11 u32      unidentified
//   +0x15 i32      width
//   +0x19 i32      height
//   +0x1D u32[6]   unidentified
//   +0x35 u32      bytes per row
//   +0x39 char[4]  "3TXD" for the block-compressed encoding
//   +0x3D u32      payload size
//   +0x41 u32      block count
//   +0x45          pixel data
constexpr size_t kNameOffset = 0x01;
constexpr size_t kWidthOffset = 0x15;
constexpr size_t kHeightOffset = 0x19;
constexpr size_t kTypeOffset = 0x39;
constexpr size_t kSizeOffset = 0x3D;
constexpr size_t kBlockDataOffset = 0x45;

// The paletted encoding replaces the type/size fields with a 256-entry palette.
constexpr size_t kPaletteOffset = 0x39;
constexpr size_t kPaletteEntries = 256;
constexpr size_t kPalettedDataOffset = kPaletteOffset + kPaletteEntries * 4;

template <typename T> T read(const std::span<const uint8_t>& data, size_t offset)
{
    if (offset + sizeof(T) > data.size())
    {
        throw std::runtime_error("texture: read past end of chunk");
    }
    T value{};
    std::memcpy(&value, data.data() + offset, sizeof(T));
    return value;
}

std::string readName(const std::span<const uint8_t>& data, size_t offset)
{
    if (offset + 16 > data.size())
    {
        throw std::runtime_error("texture: name runs past end of chunk");
    }
    std::string name(reinterpret_cast<const char*>(data.data() + offset), 16);
    while (!name.empty() && (name.back() == ' ' || name.back() == 0))
    {
        name.pop_back();
    }
    return name;
}
} // namespace

Texture parseTexture(const Chunk& chunk)
{
    const std::span<const uint8_t>& data = chunk.data;
    if (data.size() < kBlockDataOffset)
    {
        throw std::runtime_error("texture: chunk too small to hold a header");
    }

    Texture texture;
    texture.name = readName(data, kNameOffset);
    texture.width = static_cast<uint32_t>(read<int32_t>(data, kWidthOffset));
    texture.height = static_cast<uint32_t>(read<int32_t>(data, kHeightOffset));

    if (texture.width == 0 || texture.height == 0 || texture.width > 8192 || texture.height > 8192)
    {
        throw std::runtime_error("texture: implausible dimensions");
    }

    switch (data[0])
    {
    case 0xA1:
    {
        // "3TXD" is DXT3 and "1TXD" is DXT1, stored the same way and
        // differing only in what a block holds: BC2 spends eight bytes on a
        // 4-bit alpha per texel before the colour, BC1 has no alpha at all
        // beyond a punch-through bit and so is half the size.
        //
        // Rejecting 1TXD left eleven zones with textures that would not decode
        // - the 190s especially - and a mesh with no texture is drawn white,
        // which reads as missing art rather than as an unread format.
        char type[5] = {};
        std::memcpy(type, data.data() + kTypeOffset, 4);
        if (type[0] != '3' && type[0] != '1')
        {
            throw std::runtime_error(std::string("texture: unexpected block type ") + type);
        }

        const bool dxt1 = type[0] == '1';

        const uint32_t size = read<uint32_t>(data, kSizeOffset);
        if (kBlockDataOffset + size > data.size())
        {
            throw std::runtime_error("texture: block data runs past end of chunk");
        }
        texture.format = dxt1 ? TextureFormat::Bc1 : TextureFormat::Bc2;
        texture.pixels.resize(size);
        std::memcpy(texture.pixels.data(), data.data() + kBlockDataOffset, size);

        if (dxt1)
        {
            // BC1 packs a 4x4 block into 8 bytes: two RGB565 endpoints and
            // 2-bit indices. Transparency exists only when the first endpoint
            // is not greater than the second, and then only for index 3.
            size_t clear = 0;
            size_t texels = 0;
            for (size_t block = 0; block + 8 <= texture.pixels.size(); block += 8)
            {
                uint16_t c0 = 0;
                uint16_t c1 = 0;
                std::memcpy(&c0, texture.pixels.data() + block, sizeof(c0));
                std::memcpy(&c1, texture.pixels.data() + block + 2, sizeof(c1));
                texels += 16;
                if (c0 > c1)
                {
                    continue;   // opaque block, no punch-through
                }

                for (size_t i = 0; i < 4; ++i)
                {
                    const uint8_t byte = texture.pixels[block + 4 + i];
                    for (int shift = 0; shift < 8; shift += 2)
                    {
                        clear += ((byte >> shift) & 0x3) == 0x3;
                    }
                }
            }

            texture.alphaZero = texels ? static_cast<float>(clear) / static_cast<float>(texels) : 0.0f;
            texture.blackWhereClear = 0.0f;
            break;
        }

        // BC2 packs a 4x4 block into 16 bytes: 8 of 4-bit alpha, then two
        // RGB565 endpoints and 2-bit indices between them.
        size_t zero = 0;
        size_t texels = 0;
        size_t clearBlocks = 0;
        size_t clearAndBlack = 0;
        for (size_t block = 0; block + 16 <= texture.pixels.size(); block += 16)
        {
            size_t blockZero = 0;
            for (size_t i = 0; i < 8; ++i)
            {
                const uint8_t byte = texture.pixels[block + i];
                blockZero += (byte & 0x0F) == 0;
                blockZero += (byte >> 4) == 0;
            }
            zero += blockZero;
            texels += 16;

            // Half the block transparent is enough to ask what colour it hides.
            if (blockZero >= 8)
            {
                ++clearBlocks;
                uint16_t c0 = 0;
                uint16_t c1 = 0;
                std::memcpy(&c0, texture.pixels.data() + block + 8, sizeof(c0));
                std::memcpy(&c1, texture.pixels.data() + block + 10, sizeof(c1));
                // Both endpoints near black in RGB565.
                if (c0 < 0x1082 && c1 < 0x1082)
                {
                    ++clearAndBlack;
                }
            }
        }
        texture.alphaZero = texels ? static_cast<float>(zero) / static_cast<float>(texels) : 0.0f;
        texture.blackWhereClear =
            clearBlocks ? static_cast<float>(clearAndBlack) / static_cast<float>(clearBlocks) : 0.0f;
        break;
    }
    case 0x81:
    case 0xB1:
    {
        // 8-bit indices into a 256-entry BGRA palette, stored bottom-up.
        //
        // 0x81 is the same thing in an older dress. It is what the early zones
        // use - zone 0's textures are every one of them 0x81 - and rejecting it
        // left that zone drawn entirely white, which reads as missing textures
        // rather than as an unread format.
        //
        // Checked before assuming: its palette starts where this one's does and
        // decodes to a stone wall, a green tree and a perfectly neutral grey
        // smoke, which is what those three ought to be.
        if (data.size() <= kPalettedDataOffset)
        {
            throw std::runtime_error("texture: no paletted data at all");
        }

        // Short data is read as far as it goes rather than refused.
        //
        // One texture per zone across the 190s carries less than its own
        // dimensions call for - `ji` in zone 190 declares 128x128 and brings
        // about half that, which is not a 4-bit encoding either, since even
        // 4-bit with a sixteen entry palette would not fit. It looks like
        // truncated data in the retail files.
        //
        // Refusing it cost the whole texture, and a mesh with no texture is
        // drawn white, so a single short image left visible seams across nine
        // zones. What is there decodes correctly; the remainder stays
        // transparent, which is a far smaller lie than a white wall.
        const size_t available = data.size() - kPalettedDataOffset;
        const size_t wanted = static_cast<size_t>(texture.width) * texture.height;

        texture.format = TextureFormat::Rgba8;
        texture.pixels.assign(wanted * 4, 0);

        const uint8_t* indices = data.data() + kPalettedDataOffset;
        for (uint32_t y = 0; y < texture.height; ++y)
        {
            // Rows run bottom to top in the file.
            const uint32_t target = texture.height - 1 - y;
            for (uint32_t x = 0; x < texture.width; ++x)
            {
                const size_t at = static_cast<size_t>(y) * texture.width + x;
                if (at >= available)
                {
                    continue;
                }

                const uint8_t index = indices[at];
                const uint32_t entry = read<uint32_t>(data, kPaletteOffset + static_cast<size_t>(index) * 4);
                std::memcpy(texture.pixels.data() + (static_cast<size_t>(target) * texture.width + x) * 4, &entry, 4);
            }
        }
        break;
    }
    default:
        throw std::runtime_error("texture: unknown encoding flag");
    }

    return texture;
}
} // namespace ffxi
