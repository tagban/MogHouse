#pragma once

// Uploading FFXI textures to the GPU.

#include <cstdio>
#include "ffxi/texture.h"

#include <webgpu/webgpu_cpp.h>

#include <cstring>
#include <vector>

namespace mh
{
/// Uploads one texture. BC2 goes up untouched - it is a format the GPU reads
/// natively, which is the whole reason DXT3 assets are cheap to load.
inline wgpu::Texture uploadTexture(const wgpu::Device& device, const ffxi::Texture& source)
{
    const bool dxt1 = source.format == ffxi::TextureFormat::Bc1;
    const bool compressed = dxt1 || source.format == ffxi::TextureFormat::Bc2;

    // A block-compressed texture has to be a whole number of 4x4 blocks.
    //
    // Some models carry tiny placeholder textures - a 2x1 turned up on a stub
    // creature model - and asking for one as BC2 does not fail politely. The
    // texture comes back invalid, the bind group built from it is invalid, and
    // every command buffer that references it is refused from then on: the
    // window keeps its last frame and the renderer looks hung, several
    // thousand identical validation errors later. Returning nothing here
    // leaves the caller to fall back to the white texture, which costs one
    // untextured mesh instead of the whole device.
    if (compressed && (source.width % 4 != 0 || source.height % 4 != 0))
    {
        std::printf("skipping %ux%u compressed texture: not whole 4x4 blocks\n", source.width, source.height);
        return {};
    }

    wgpu::TextureDescriptor descriptor{};
    descriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    descriptor.dimension = wgpu::TextureDimension::e2D;
    descriptor.size = {source.width, source.height, 1};
    descriptor.format = dxt1    ? wgpu::TextureFormat::BC1RGBAUnorm
                        : compressed ? wgpu::TextureFormat::BC2RGBAUnorm
                                     : wgpu::TextureFormat::RGBA8Unorm;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;

    wgpu::Texture texture = device.CreateTexture(&descriptor);
    if (!texture)
    {
        return texture;
    }

    wgpu::TexelCopyTextureInfo destination{};
    destination.texture = texture;

    wgpu::TexelCopyBufferLayout layout{};
    // A block covers 4x4 pixels either way, so a row of blocks covers four rows
    // of pixels and there are width/4 blocks across - but a BC1 block is eight
    // bytes where a BC2 block is sixteen, because BC1 has no alpha channel to
    // store. Getting this wrong does not fail, it skews.
    const uint32_t blockBytes = dxt1 ? 8 : 16;
    layout.bytesPerRow = compressed ? (source.width / 4) * blockBytes : source.width * 4;
    layout.rowsPerImage = compressed ? source.height / 4 : source.height;

    device.GetQueue().WriteTexture(&destination, source.pixels.data(), source.pixels.size(), &layout, &descriptor.size);
    return texture;
}

/// A single white pixel, for geometry that has no texture - collision hulls,
/// and any mesh whose texture is missing from the DAT.
/// A single texel of a given colour, for meshes with no texture to bind.
inline wgpu::Texture createSolidTexture(const wgpu::Device& device, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    wgpu::TextureDescriptor descriptor{};
    descriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    descriptor.dimension = wgpu::TextureDimension::e2D;
    descriptor.size = {1, 1, 1};
    descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;

    wgpu::Texture texture = device.CreateTexture(&descriptor);
    const uint8_t texel[4] = {r, g, b, a};

    wgpu::TexelCopyTextureInfo destination{};
    destination.texture = texture;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    device.GetQueue().WriteTexture(&destination, texel, sizeof(texel), &layout, &descriptor.size);
    return texture;
}

inline wgpu::Texture createWhiteTexture(const wgpu::Device& device)
{
    wgpu::TextureDescriptor descriptor{};
    descriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    descriptor.dimension = wgpu::TextureDimension::e2D;
    descriptor.size = {1, 1, 1};
    descriptor.format = wgpu::TextureFormat::RGBA8Unorm;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;

    wgpu::Texture texture = device.CreateTexture(&descriptor);
    const uint8_t white[4] = {255, 255, 255, 255};

    wgpu::TexelCopyTextureInfo destination{};
    destination.texture = texture;
    wgpu::TexelCopyBufferLayout layout{};
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    device.GetQueue().WriteTexture(&destination, white, sizeof(white), &layout, &descriptor.size);
    return texture;
}
} // namespace mh
