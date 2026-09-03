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
    Bc1,   ///< DXT1 blocks, uploadable untouched. Half the size of BC2: no
           ///< alpha channel, only a punch-through bit.
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

    /// How much of the texture sits at neither end of the alpha range.
    ///
    /// A mask is binary - a texel is leaf or it is nothing - so this is near
    /// zero. A blend texture uses the middle of the range for what it is for,
    /// so this is not. It is the question `blackWhereClear` was reaching for
    /// and could not ask of a format whose clear texels are not black.
    float alphaMidway{};

    /// Whether this texture's alpha is a mask to cut with rather than a
    /// factor to blend by - which decides whether a draw gets the alpha test.
    ///
    /// Two ways of being sure, because one is not enough. A texture that is
    /// black where it is clear is a mask: nobody paints the hidden part black
    /// unless it will never be seen. That is the original test, and it works
    /// for the block-compressed textures it was measured on.
    ///
    /// It says nothing about the paletted ones the early zones use, whose
    /// clear texels carry whatever colour the palette left there - Sel Phiner's
    /// `tree` is 37% fully transparent and none of it black, so it failed the
    /// test and every leaf drew its own card. Hence the second way: alpha that
    /// is binary is a mask, because a blend uses the middle of the range and a
    /// mask has no use for it. Terrain measures 0.39 to 1.00 midway; foliage
    /// measures 0.00.
    bool isCutoutMask() const
    {
        constexpr float kBlackWhereClear = 0.2f;
        constexpr float kSomeTransparency = 0.02f;
        constexpr float kEssentiallyBinary = 0.05f;
        return blackWhereClear > kBlackWhereClear ||
               (alphaZero > kSomeTransparency && alphaMidway < kEssentiallyBinary);
    }
};

/// Reads one texture chunk. Texture chunks are not obfuscated, unlike MZB and
/// MMB, so this needs no key tables.
///
/// Throws std::runtime_error on an unknown encoding or a truncated chunk.
Texture parseTexture(const Chunk& chunk);
} // namespace ffxi
