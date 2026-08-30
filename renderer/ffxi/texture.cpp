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
        char type[5] = {};
        std::memcpy(type, data.data() + kTypeOffset, 4);
        if (type[0] != '3')
        {
            throw std::runtime_error(std::string("texture: unexpected block type ") + type);
        }

        const uint32_t size = read<uint32_t>(data, kSizeOffset);
        if (kBlockDataOffset + size > data.size())
        {
            throw std::runtime_error("texture: block data runs past end of chunk");
        }
        texture.format = TextureFormat::Bc2;
        texture.pixels.resize(size);
        std::memcpy(texture.pixels.data(), data.data() + kBlockDataOffset, size);

        // BC2 packs a 4x4 block into 16 bytes: 8 of 4-bit alpha, then colour.
        size_t zero = 0;
        size_t texels = 0;
        for (size_t block = 0; block + 16 <= texture.pixels.size(); block += 16)
        {
            for (size_t i = 0; i < 8; ++i)
            {
                const uint8_t byte = texture.pixels[block + i];
                zero += (byte & 0x0F) == 0;
                zero += (byte >> 4) == 0;
                texels += 2;
            }
        }
        texture.alphaZero = texels ? static_cast<float>(zero) / static_cast<float>(texels) : 0.0f;
        break;
    }
    case 0xB1:
    {
        // 8-bit indices into a 256-entry BGRA palette, stored bottom-up.
        if (kPalettedDataOffset + static_cast<size_t>(texture.width) * texture.height > data.size())
        {
            throw std::runtime_error("texture: paletted data runs past end of chunk");
        }
        texture.format = TextureFormat::Rgba8;
        texture.pixels.resize(static_cast<size_t>(texture.width) * texture.height * 4);

        const uint8_t* indices = data.data() + kPalettedDataOffset;
        for (uint32_t y = 0; y < texture.height; ++y)
        {
            // Rows run bottom to top in the file.
            const uint32_t target = texture.height - 1 - y;
            for (uint32_t x = 0; x < texture.width; ++x)
            {
                const uint8_t index = indices[static_cast<size_t>(y) * texture.width + x];
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
