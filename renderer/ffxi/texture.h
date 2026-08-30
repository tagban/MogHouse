#pragma once

// DXT3 - FFXI textures. Chunk type 0x20, despite the name covering more than
// one encoding. See docs/dxt3-format.md.

#include "dat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ffxi
{
enum class TextureFormat
{
    Bc2,   ///< DXT3 blocks, uploadable to the GPU untouched
    Rgba8, ///< expanded here from an 8-bit paletted image
};

struct Texture
{
    std::string name; ///< 16 bytes, matches a mesh header's texture field
    uint32_t width{};
    uint32_t height{};
    TextureFormat format{TextureFormat::Bc2};
    std::vector<uint8_t> pixels;

    /// Fraction of texels with zero alpha. Ground textures sit around 0.5 and
    /// their alpha is a blend factor; foliage is far higher and genuinely wants
    /// cutting out. Measured here because it is a property of the artwork, and
    /// nothing in the headers says which a texture is.
    float alphaZero{};
};

/// Reads one texture chunk. Texture chunks are not obfuscated, unlike MZB and
/// MMB, so this needs no key tables.
///
/// Throws std::runtime_error on an unknown encoding or a truncated chunk.
Texture parseTexture(const Chunk& chunk);
} // namespace ffxi
