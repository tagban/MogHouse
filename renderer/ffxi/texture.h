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

    /// Fraction of texels with zero alpha.
    float alphaZero{};

    /// Of the blocks that are mostly transparent, the fraction whose colour is
    /// also black.
    ///
    /// This is what separates a cutout mask from a blend factor, and nothing in
    /// the format says which a texture is. An artist painting a cutout leaves
    /// the hidden area black because it will never be seen; a blend texture
    /// carries real colour throughout. Measured across a zone the split is
    /// absolute - cutouts land at 0.37 to 0.93, everything else at 0.00.
    ///
    /// Transparency alone does not work: grass is only 0.19 to 0.25 alpha-zero,
    /// less than rock at 0.51 or ground at 0.60.
    float blackWhereClear{};
};

/// Reads one texture chunk. Texture chunks are not obfuscated, unlike MZB and
/// MMB, so this needs no key tables.
///
/// Throws std::runtime_error on an unknown encoding or a truncated chunk.
Texture parseTexture(const Chunk& chunk);
} // namespace ffxi
