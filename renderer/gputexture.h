#pragma once

// Uploading FFXI textures to the GPU.

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
    const bool compressed = source.format == ffxi::TextureFormat::Bc2;

    wgpu::TextureDescriptor descriptor{};
    descriptor.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    descriptor.dimension = wgpu::TextureDimension::e2D;
    descriptor.size = {source.width, source.height, 1};
    descriptor.format = compressed ? wgpu::TextureFormat::BC2RGBAUnorm : wgpu::TextureFormat::RGBA8Unorm;
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
    // BC2 packs a 4x4 block into 16 bytes, so a row of blocks covers four rows
    // of pixels and there are width/4 blocks across.
    layout.bytesPerRow = compressed ? (source.width / 4) * 16 : source.width * 4;
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
