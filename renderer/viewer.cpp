// MogHouse's renderer. Opens a window on WebGPU - Metal on macOS, D3D12 on
// Windows, Vulkan on Linux - and draws FFXI zone geometry read from the retail
// DATs.
//
// Given a DAT it draws that zone's collision geometry. Given nothing it only
// clears, so the graphics path can still be checked on a machine with no game
// installed.

#include "ffxi/dat.h"
#include "ffxi/filetable.h"
#include "ffxi/look.h"
#include "ffxi/mmb.h"
#include "ffxi/lighting.h"
#include "ffxi/mo2.h"
#include "ffxi/mzb.h"
#include "ffxi/os2.h"
#include "ffxi/skeleton.h"
#include "ffxi/texture.h"
#include "gputexture.h"
#include "camera.h"
#include "character.h"
#include "collision.h"
#include "viewer.h"
#include "coverage.h"
#include "linalg.h"
#include "radar_shader.h"
#include "scene.h"
#include "surface.h"
#include "sky_shader.h"
#include "water_shader.h"
#include "zone_shader.h"
#include "zonemesh.h"

#include <SDL3/SDL.h>
#include <webgpu/webgpu_cpp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <string>

namespace
{
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
constexpr wgpu::TextureFormat kDepthFormat = wgpu::TextureFormat::Depth24Plus;

struct Uniforms
{
    float viewProjection[16];
    float lightDirection[4];
    float ambient[4];
    float sunlight[4];
    float fogColour[4];
    float fogRange[4];
    float eye[4];
};

/// Matches RadarUniforms in radar_shader.h.
struct RadarUniforms
{
    float placement[4];
    float mapExtent[4];
    float viewer[4];
    float counts[4];
    float entities[mh::kRadarMaxEntities][4];
};

struct SkyUniforms
{
    float forward[4];
    float right[4];
    float up[4];
    float skyColours[8][4];
    float skyAltitudes[8][4];
    float fogColour[4];
};

const char* backendName(wgpu::BackendType backend)
{
    switch (backend)
    {
    case wgpu::BackendType::D3D12: return "D3D12";
    case wgpu::BackendType::Metal: return "Metal";
    case wgpu::BackendType::Vulkan: return "Vulkan";
    case wgpu::BackendType::OpenGL: return "OpenGL";
    case wgpu::BackendType::OpenGLES: return "OpenGLES";
    default: return "other";
    }
}

// Loads the largest MZB in a DAT. Largest because the ferry DATs hold both a
// world and a vessel, and the world is the interesting one.
/// Reads one character out of a DAT: its skeleton, every mesh hung on it, and
/// the textures those meshes name.
///
/// A DAT like ROM/3/6.DAT holds a whole NPC. Player characters are assembled
/// from several files instead - one per equipment slot - which is the same
/// work with more inputs, so this takes a list.
/// A character kept in a form that can still be posed. The geometry is what
/// gets drawn; the skeleton, meshes and animations are what it is rebuilt from
/// every frame.
struct LoadedCharacter
{
    ffxi::Skeleton skeleton;
    std::vector<ffxi::SkinnedModel> meshes;
    std::map<std::string, ffxi::Animation> animations;
    mh::Character geometry;
};

std::optional<LoadedCharacter> loadCharacter(const std::vector<std::string>& datPaths,
                                             std::unordered_map<std::string, ffxi::Texture>& textures)
{
    LoadedCharacter loaded;
    ffxi::Skeleton& skeleton = loaded.skeleton;
    bool haveSkeleton = false;
    std::vector<ffxi::SkinnedModel>& meshes = loaded.meshes;

    for (const std::string& datPath : datPaths)
    {
        ffxi::DatFile dat{std::filesystem::path{datPath}};

        // The first skeleton found wins. Everything after it has to be hung on
        // the same bones, so a second one would be a different character.
        if (!haveSkeleton)
        {
            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkeleton))
            {
                try
                {
                    skeleton = ffxi::parseSkeleton(chunk);
                    haveSkeleton = true;
                    break;
                }
                catch (const std::exception& e)
                {
                    std::printf("skeleton %.4s: %s\n", chunk.id, e.what());
                }
            }
        }

        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkTexture))
        {
            try
            {
                ffxi::Texture texture = ffxi::parseTexture(chunk);
                textures.insert_or_assign(texture.name, std::move(texture));
            }
            catch (const std::exception&)
            {
            }
        }

        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkSkinnedMesh))
        {
            try
            {
                ffxi::SkinnedModel model = ffxi::parseOs2(chunk);
                if (!model.parts.empty())
                {
                    meshes.push_back(std::move(model));
                }
            }
            catch (const std::exception& e)
            {
                std::printf("skinned %.4s: %s\n", chunk.id, e.what());
            }
        }

        // Animations. The race's own file carries most of them; a piece of
        // equipment can bring its own, which is why they are collected from
        // every file rather than only the first.
        for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkAnimation))
        {
            try
            {
                ffxi::Animation animation = ffxi::parseMo2(chunk);
                loaded.animations.insert_or_assign(animation.name, std::move(animation));
            }
            catch (const std::exception&)
            {
                // Eye and mouth tracks share the chunk type and are not poses.
            }
        }
    }

    if (!haveSkeleton || meshes.empty())
    {
        std::printf("no character in those files\n");
        return std::nullopt;
    }

    loaded.geometry = mh::buildCharacter(mh::bindPose(skeleton), meshes, textures);
    std::printf("character: %zu bones, %zu meshes, %zu triangles, %.2f tall, %zu animations\n",
                skeleton.bones.size(), meshes.size(), loaded.geometry.triangles(), loaded.geometry.height(),
                loaded.animations.size());
    return loaded;
}

std::optional<mh::Scene> loadZone(const char* datPath, const char* keyPath, const char* key2Path, std::string& zoneId,
                                     std::unordered_map<std::string, ffxi::Texture>& textures, ffxi::Lighting& lighting,
                                     mh::Collision& collision)
{
    auto keys = ffxi::KeyTable::load(keyPath);
    if (!keys)
    {
        std::printf("could not read a 256-byte key table from %s\n", keyPath);
        return std::nullopt;
    }

    ffxi::DatFile dat{std::filesystem::path{datPath}};

    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkLighting))
    {
        lighting.add(chunk);
    }

    // Textures are not obfuscated, so they need no keys.
    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkTexture))
    {
        try
        {
            ffxi::Texture texture = ffxi::parseTexture(chunk);
            std::string key = texture.name;
            textures.emplace(std::move(key), std::move(texture));
        }
        catch (const std::exception&)
        {
        }
    }

    // Every model in the DAT, keyed by the name a placement refers to it by.
    std::unordered_map<std::string, ffxi::Model> models;
    size_t modelsFailed = 0;
    if (key2Path)
    {
        if (auto keys2 = ffxi::KeyTable::load(key2Path))
        {
            for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMmb))
            {
                try
                {
                    ffxi::Model model = ffxi::parseMmb(chunk, *keys, *keys2);
                    std::string key = model.name;
                    models.emplace(std::move(key), std::move(model));
                }
                catch (const std::exception&)
                {
                    ++modelsFailed;
                }
            }
        }
    }

    std::optional<mh::Scene> best;
    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMzb))
    {
        ffxi::Zone zone = ffxi::parseMzb(chunk, *keys);

        // Placed models are the visible world; collision geometry is the
        // fallback when the model key table is not available.
        mh::Scene mesh;
        if (!models.empty())
        {
            size_t resolved = 0;
            size_t missing = 0;
            mesh = mh::buildScene(zone, models, textures, resolved, missing);
            if (!mesh.vertices.empty())
            {
                std::printf("  %zu models (%zu unreadable), %zu placements drawn, %zu with no model\n", models.size(),
                            modelsFailed, resolved, missing);
                std::printf("  %zu unique triangles, %zu drawn - instancing saves %.1fx\n", mesh.triangles(),
                            mesh.drawnTriangles(),
                            mesh.triangles() ? static_cast<double>(mesh.drawnTriangles()) / static_cast<double>(mesh.triangles()) : 0.0);
            }
        }
        // Collision geometry is deliberately not drawn. Its meshes carry a
        // flags field with only two values across all 5,921 of them, so it
        // references no material - it is genuine collision, not terrain, and
        // drawing it just covers the world in untextured white.
        if (!best || mesh.vertices.size() > best->vertices.size())
        {
            best = std::move(mesh);
            zoneId = zone.id;
            // The same chunk that produced the visible world produces the
            // ground to stand on, so they cannot disagree.
            collision = mh::Collision{zone};
        }
    }
    return best;
}

wgpu::Buffer createBuffer(const wgpu::Device& device, const void* data, size_t size, wgpu::BufferUsage usage)
{
    wgpu::BufferDescriptor descriptor{.usage = usage | wgpu::BufferUsage::CopyDst, .size = size};
    wgpu::Buffer buffer = device.CreateBuffer(&descriptor);
    if (!buffer)
    {
        std::printf("failed to create a %zu byte buffer\n", size);
        return buffer;
    }

    // Written in chunks rather than one call: a single multi-megabyte
    // WriteBuffer stages the whole thing at once, and it is cheap to avoid.
    constexpr size_t kChunk = 4u * 1024u * 1024u;
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (size_t written = 0; written < size; written += kChunk)
    {
        const size_t amount = std::min(kChunk, size - written);
        device.GetQueue().WriteBuffer(buffer, written, bytes + written, amount);
    }
    return buffer;
}
/// Writes a 24-bit BMP. Uncompressed and about as simple as an image format
/// gets, which matters because the alternative is pulling in a PNG encoder to
/// look at one frame.
bool writeBmp(const char* path, const uint8_t* bgra, uint32_t width, uint32_t height, uint32_t bytesPerRow)
{
    std::FILE* file = std::fopen(path, "wb");
    if (!file)
    {
        return false;
    }

    const uint32_t rowBytes = width * 3;
    const uint32_t padding = (4 - (rowBytes % 4)) % 4;
    const uint32_t imageBytes = (rowBytes + padding) * height;
    const uint32_t fileBytes = 54 + imageBytes;

    uint8_t header[54] = {};
    header[0] = 0x42;
    header[1] = 0x4D;
    std::memcpy(header + 2, &fileBytes, 4);
    const uint32_t pixelOffset = 54;
    std::memcpy(header + 10, &pixelOffset, 4);
    const uint32_t infoSize = 40;
    std::memcpy(header + 14, &infoSize, 4);
    std::memcpy(header + 18, &width, 4);
    std::memcpy(header + 22, &height, 4);
    const uint16_t planes = 1;
    const uint16_t bits = 24;
    std::memcpy(header + 26, &planes, 2);
    std::memcpy(header + 28, &bits, 2);
    std::memcpy(header + 34, &imageBytes, 4);
    std::fwrite(header, 1, sizeof(header), file);

    // BMP rows run bottom to top.
    std::vector<uint8_t> row(rowBytes + padding, 0);
    for (uint32_t y = 0; y < height; ++y)
    {
        const uint8_t* source = bgra + static_cast<size_t>(height - 1 - y) * bytesPerRow;
        for (uint32_t x = 0; x < width; ++x)
        {
            row[x * 3 + 0] = source[x * 4 + 0];
            row[x * 3 + 1] = source[x * 4 + 1];
            row[x * 3 + 2] = source[x * 4 + 2];
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
    return true;
}
} // namespace

int mh::runViewer(const ViewerOptions& options)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string zoneId;
    std::optional<mh::Scene> zone;
    mh::Collision collision;
    std::unordered_map<std::string, ffxi::Texture> textures;
    ffxi::Lighting lighting;
    if (!options.zonePath.empty())
    {
        const char* keyPath = options.keyTablePath.empty() ? nullptr : options.keyTablePath.c_str();
        if (!keyPath)
        {
            std::printf("set MOGHOUSE_FFXI_KEYTABLE to the 256-byte MZB key table to load a zone\n");
            return 2;
        }
        zone = loadZone(options.zonePath.c_str(), keyPath,
                        options.keyTable2Path.empty() ? nullptr : options.keyTable2Path.c_str(), zoneId, textures,
                        lighting, collision);
        if (!zone)
        {
            return 1;
        }
        // The character is loaded after the zone so it can share the texture
        // map: a PC in a town wears textures the zone never mentions, and a
        // zone texture the character happens to name should not be read twice.
        std::printf("collision: %zu triangles\n", collision.triangleCount());
        std::printf("zone %s: %zu triangles\n", zoneId.c_str(), zone->indices.size() / 3);
        std::printf("  bounds x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f\n", zone->boundsMin.x, zone->boundsMax.x,
                    zone->boundsMin.y, zone->boundsMax.y, zone->boundsMin.z, zone->boundsMax.z);
    }
    else
    {
        std::printf("no DAT given - clearing only. Pass a zone DAT to draw one.\n");
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("MogHouse renderer", kWidth, kHeight,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window)
    {
        std::printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    // Waiting on a future with a non-zero timeout is opt-in. Without this every
    // WaitAny fails with "Timeout waits are either not enabled or not
    // supported", which reads like a driver limitation and is not one.
    static constexpr wgpu::InstanceFeatureName kInstanceFeatures[] = {wgpu::InstanceFeatureName::TimedWaitAny};
    wgpu::InstanceDescriptor instanceDescriptor{.requiredFeatureCount = std::size(kInstanceFeatures),
                                                .requiredFeatures = kInstanceFeatures};
    wgpu::Instance instance = wgpu::CreateInstance(&instanceDescriptor);
    if (!instance)
    {
        std::printf("could not create a WebGPU instance\n");
        return 1;
    }

    wgpu::Surface surface = mh::CreateSurface(instance, window);
    if (!surface)
    {
        std::printf("could not create a surface for this window\n");
        return 1;
    }

    wgpu::Adapter adapter;
    wgpu::RequestAdapterOptions adapterOptions{.compatibleSurface = surface};
    instance.WaitAny(instance.RequestAdapter(&adapterOptions, wgpu::CallbackMode::WaitAnyOnly,
                                             [&](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView message)
                                             {
                                                 if (status != wgpu::RequestAdapterStatus::Success)
                                                 {
                                                     std::printf("no adapter: %.*s\n", static_cast<int>(message.length), message.data);
                                                     return;
                                                 }
                                                 adapter = std::move(result);
                                             }),
                     UINT64_MAX);
    if (!adapter)
    {
        return 1;
    }

    wgpu::AdapterInfo info{};
    adapter.GetInfo(&info);
    std::printf("adapter: %.*s (%s)\n", static_cast<int>(info.device.length), info.device.data, backendName(info.backendType));

    // BC2 is the format FFXI's DXT3 textures already are, but a WebGPU device
    // will not accept it unless the feature is asked for up front.
    const bool supportsBc = adapter.HasFeature(wgpu::FeatureName::TextureCompressionBC);
    if (!supportsBc)
    {
        std::printf("this adapter has no BC texture support - compressed textures will be skipped\n");
    }
    const wgpu::FeatureName requiredFeatures[] = {wgpu::FeatureName::TextureCompressionBC};

    wgpu::DeviceDescriptor deviceDescriptor{};
    if (supportsBc)
    {
        deviceDescriptor.requiredFeatureCount = 1;
        deviceDescriptor.requiredFeatures = requiredFeatures;
    }
    deviceDescriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message)
                                                { std::printf("webgpu error: %.*s\n", static_cast<int>(message.length), message.data); });

    wgpu::Device device;
    instance.WaitAny(adapter.RequestDevice(&deviceDescriptor, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView message)
                                           {
                                               if (status != wgpu::RequestDeviceStatus::Success)
                                               {
                                                   std::printf("no device: %.*s\n", static_cast<int>(message.length), message.data);
                                                   return;
                                               }
                                               device = std::move(result);
                                           }),
                     UINT64_MAX);
    if (!device)
    {
        return 1;
    }

    wgpu::Queue queue = device.GetQueue();

    // The sky is drawn before anything else, at the far plane, with no depth
    // writes - it is a backdrop rather than a surface.
    wgpu::ShaderSourceWGSL skyWgsl;
    skyWgsl.code = mh::kSkyShader;
    wgpu::ShaderModuleDescriptor skyModuleDescriptor{.nextInChain = &skyWgsl};
    wgpu::ShaderModule skyModule = device.CreateShaderModule(&skyModuleDescriptor);

    wgpu::BufferDescriptor skyUniformDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                .size = sizeof(SkyUniforms)};
    wgpu::Buffer skyUniformBuffer = device.CreateBuffer(&skyUniformDescriptor);

    wgpu::SurfaceCapabilities capabilities{};
    surface.GetCapabilities(adapter, &capabilities);
    const wgpu::TextureFormat surfaceFormat = capabilities.formats[0];

    int pixelWidth = 0;
    int pixelHeight = 0;
    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);

    int pointWidth = 0;
    int pointHeight = 0;
    SDL_GetWindowSize(window, &pointWidth, &pointHeight);
    std::printf("window: %dx%d points, %dx%d pixels\n", pointWidth, pointHeight, pixelWidth, pixelHeight);

    wgpu::ColorTargetState skyTarget{.format = surfaceFormat};
    wgpu::FragmentState skyFragment{
        .module = skyModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &skyTarget};
    // Depth is compared but never written, so geometry drawn afterwards always
    // wins and the sky fills only what is left.
    wgpu::DepthStencilState skyDepth{.format = kDepthFormat,
                                     .depthWriteEnabled = wgpu::OptionalBool::False,
                                     .depthCompare = wgpu::CompareFunction::Always};
    wgpu::RenderPipelineDescriptor skyPipelineDescriptor{
        .vertex = {.module = skyModule, .entryPoint = "vertexMain"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
        .depthStencil = &skyDepth,
        .fragment = &skyFragment};
    wgpu::RenderPipeline skyPipeline = device.CreateRenderPipeline(&skyPipelineDescriptor);

    wgpu::BindGroupEntry skyEntry{.binding = 0, .buffer = skyUniformBuffer, .size = sizeof(SkyUniforms)};
    wgpu::BindGroupDescriptor skyBindGroupDescriptor{
        .layout = skyPipeline.GetBindGroupLayout(0), .entryCount = 1, .entries = &skyEntry};
    wgpu::BindGroup skyBindGroup = device.CreateBindGroup(&skyBindGroupDescriptor);

    wgpu::Texture depthTexture;
    auto configure = [&](uint32_t width, uint32_t height)
    {
        wgpu::SurfaceConfiguration configuration{.device = device,
                                                 .format = surfaceFormat,
                                                 // CopySrc so a frame can be read back; see writeBmp.
                                                 .usage = wgpu::TextureUsage::RenderAttachment |
                                                          wgpu::TextureUsage::CopySrc,
                                                 .width = width,
                                                 .height = height,
                                                 .presentMode = wgpu::PresentMode::Fifo};
        surface.Configure(&configuration);

        wgpu::TextureDescriptor depthDescriptor{.usage = wgpu::TextureUsage::RenderAttachment,
                                                .dimension = wgpu::TextureDimension::e2D,
                                                .size = {width, height, 1},
                                                .format = kDepthFormat,
                                                .mipLevelCount = 1,
                                                .sampleCount = 1};
        depthTexture = device.CreateTexture(&depthDescriptor);
    };
    configure(static_cast<uint32_t>(pixelWidth), static_cast<uint32_t>(pixelHeight));

    wgpu::Buffer vertexBuffer;
    wgpu::Buffer indexBuffer;
    wgpu::Buffer instanceBuffer;
    wgpu::Buffer waterVertexBuffer;
    wgpu::Buffer waterIndexBuffer;
    wgpu::RenderPipeline waterPipeline;
    wgpu::BindGroup waterBindGroup;
    uint32_t waterIndexCount = 0;
    wgpu::Buffer uniformBuffer;
    wgpu::RenderPipeline pipeline;
    wgpu::RenderPipeline cutoutPipeline;
    wgpu::BindGroupLayout zoneBindGroupLayout;
    wgpu::Sampler sampler;
    wgpu::Texture whiteTexture;
    std::vector<wgpu::Texture> batchTextures;
    std::vector<wgpu::BindGroup> batchBindGroups;
    uint32_t indexCount = 0;

    if (zone && !zone->indices.empty())
    {
        vertexBuffer = createBuffer(device, zone->vertices.data(), zone->vertices.size() * sizeof(mh::Vertex),
                                    wgpu::BufferUsage::Vertex);
        indexBuffer = createBuffer(device, zone->indices.data(), zone->indices.size() * sizeof(uint32_t),
                                   wgpu::BufferUsage::Index);
        instanceBuffer = createBuffer(device, zone->instances.data(), zone->instances.size() * sizeof(float),
                                      wgpu::BufferUsage::Vertex);
        indexCount = static_cast<uint32_t>(zone->indices.size());
        std::printf("buffers created\n");

        wgpu::BufferDescriptor uniformDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                 .size = sizeof(Uniforms)};
        uniformBuffer = device.CreateBuffer(&uniformDescriptor);

        wgpu::ShaderSourceWGSL wgsl;
        wgsl.code = mh::kZoneShader;
        wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl};
        wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);

        wgpu::VertexAttribute attributes[3] = {
            {.format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0},
            {.format = wgpu::VertexFormat::Float32x3, .offset = 3 * sizeof(float), .shaderLocation = 1},
            {.format = wgpu::VertexFormat::Float32x2, .offset = 6 * sizeof(float), .shaderLocation = 2}};
        wgpu::VertexAttribute instanceAttributes[4] = {
            {.format = wgpu::VertexFormat::Float32x4, .offset = 0, .shaderLocation = 3},
            {.format = wgpu::VertexFormat::Float32x4, .offset = 4 * sizeof(float), .shaderLocation = 4},
            {.format = wgpu::VertexFormat::Float32x4, .offset = 8 * sizeof(float), .shaderLocation = 5},
            {.format = wgpu::VertexFormat::Float32x4, .offset = 12 * sizeof(float), .shaderLocation = 6}};

        wgpu::VertexBufferLayout vertexLayout{.stepMode = wgpu::VertexStepMode::Vertex,
                                              .arrayStride = sizeof(mh::Vertex),
                                              .attributeCount = 3,
                                              .attributes = attributes};
        // Stepping per instance rather than per vertex is the whole trick: one
        // copy of the geometry, one matrix per placement.
        wgpu::VertexBufferLayout instanceLayout{.stepMode = wgpu::VertexStepMode::Instance,
                                                .arrayStride = 16 * sizeof(float),
                                                .attributeCount = 4,
                                                .attributes = instanceAttributes};
        wgpu::VertexBufferLayout bufferLayouts[2] = {vertexLayout, instanceLayout};

        // An explicit layout shared by both pipelines. Letting each derive its
        // own default layout makes bind groups built for one incompatible with
        // the other, which fails at draw time rather than at creation.
        wgpu::BindGroupLayoutEntry layoutEntries[3] = {};
        layoutEntries[0].binding = 0;
        layoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        layoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        layoutEntries[1].binding = 1;
        layoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        layoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        layoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        layoutEntries[2].binding = 2;
        layoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        layoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor bindGroupLayoutDescriptor{.entryCount = 3, .entries = layoutEntries};
        zoneBindGroupLayout = device.CreateBindGroupLayout(&bindGroupLayoutDescriptor);

        wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                .bindGroupLayouts = &zoneBindGroupLayout};
        wgpu::PipelineLayout sharedLayout = device.CreatePipelineLayout(&pipelineLayoutDescriptor);

        wgpu::ColorTargetState colorTarget{.format = surfaceFormat};
        wgpu::FragmentState fragment{.module = module, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &colorTarget};
        wgpu::DepthStencilState depthStencil{.format = kDepthFormat,
                                             .depthWriteEnabled = wgpu::OptionalBool::True,
                                             .depthCompare = wgpu::CompareFunction::Less};

        wgpu::RenderPipelineDescriptor pipelineDescriptor{
            .layout = sharedLayout,
            .vertex = {.module = module, .entryPoint = "vertexMain", .bufferCount = 2, .buffers = bufferLayouts},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &depthStencil,
            .fragment = &fragment};
        pipeline = device.CreateRenderPipeline(&pipelineDescriptor);

        // Same pipeline, alpha-cutout fragment shader. Which one a batch uses
        // comes from its mesh header rather than from one global choice.
        wgpu::FragmentState cutoutFragment{
            .module = module, .entryPoint = "fragmentCutout", .targetCount = 1, .targets = &colorTarget};
        wgpu::RenderPipelineDescriptor cutoutDescriptor = pipelineDescriptor;
        cutoutDescriptor.fragment = &cutoutFragment;
        cutoutPipeline = device.CreateRenderPipeline(&cutoutDescriptor);

        // WebGPU has no bindless arrays, so each texture needs its own bind
        // group and its own draw. Fine at a zone's few dozen textures; this is
        // the thing that will need atlasing or caching at a larger scale.
        wgpu::SamplerDescriptor samplerDescriptor{};
        samplerDescriptor.addressModeU = wgpu::AddressMode::Repeat;
        samplerDescriptor.addressModeV = wgpu::AddressMode::Repeat;
        samplerDescriptor.magFilter = wgpu::FilterMode::Linear;
        samplerDescriptor.minFilter = wgpu::FilterMode::Linear;
        sampler = device.CreateSampler(&samplerDescriptor);

        whiteTexture = mh::createWhiteTexture(device);
        const wgpu::TextureView whiteView = whiteTexture.CreateView();

        if (!zone->waterIndices.empty())
        {
            waterVertexBuffer = createBuffer(device, zone->waterVertices.data(),
                                             zone->waterVertices.size() * sizeof(mh::Vertex), wgpu::BufferUsage::Vertex);
            waterIndexBuffer = createBuffer(device, zone->waterIndices.data(),
                                            zone->waterIndices.size() * sizeof(uint32_t), wgpu::BufferUsage::Index);
            waterIndexCount = static_cast<uint32_t>(zone->waterIndices.size());

            wgpu::ShaderSourceWGSL waterWgsl;
            waterWgsl.code = mh::kWaterShader;
            wgpu::ShaderModuleDescriptor waterModuleDescriptor{.nextInChain = &waterWgsl};
            wgpu::ShaderModule waterModule = device.CreateShaderModule(&waterModuleDescriptor);

            // Blended, and writing no depth: water is a surface you see through,
            // and letting it write depth hides whatever is under it.
            wgpu::BlendState waterBlend{};
            waterBlend.color.operation = wgpu::BlendOperation::Add;
            waterBlend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
            waterBlend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
            waterBlend.alpha.operation = wgpu::BlendOperation::Add;
            waterBlend.alpha.srcFactor = wgpu::BlendFactor::One;
            waterBlend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;

            wgpu::ColorTargetState waterTarget{.format = surfaceFormat, .blend = &waterBlend};
            wgpu::FragmentState waterFragment{
                .module = waterModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &waterTarget};
            wgpu::DepthStencilState waterDepth{.format = kDepthFormat,
                                               .depthWriteEnabled = wgpu::OptionalBool::False,
                                               .depthCompare = wgpu::CompareFunction::Less};

            // FFXI's own water texture, scrolled, rather than an invented
            // colour. The models nothing places - kw01 for rivers, ike for
            // ponds, umi1 for sea - all carry one of these.
            wgpu::TextureView waterView = whiteTexture.CreateView();
            for (const char* candidate : {"effect  kaw1", "effect  ike1", "effect  ike2", "effect  umna", "effect  nami"})
            {
                auto found = textures.find(candidate);
                if (found != textures.end())
                {
                    wgpu::Texture gpu = mh::uploadTexture(device, found->second);
                    if (gpu)
                    {
                        batchTextures.push_back(gpu);
                        waterView = batchTextures.back().CreateView();
                        std::printf("water texture: %s\n", candidate);
                        break;
                    }
                }
            }

            wgpu::BindGroupLayoutEntry waterLayoutEntries[3] = {};
            waterLayoutEntries[0].binding = 0;
            waterLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
            waterLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
            waterLayoutEntries[1].binding = 1;
            waterLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
            waterLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
            waterLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
            waterLayoutEntries[2].binding = 2;
            waterLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
            waterLayoutEntries[2].sampler.type = wgpu::SamplerBindingType::Filtering;
            wgpu::BindGroupLayoutDescriptor waterBglDescriptor{.entryCount = 3, .entries = waterLayoutEntries};
            wgpu::BindGroupLayout waterBgl = device.CreateBindGroupLayout(&waterBglDescriptor);
            wgpu::PipelineLayoutDescriptor waterPlDescriptor{.bindGroupLayoutCount = 1, .bindGroupLayouts = &waterBgl};
            wgpu::PipelineLayout waterPl = device.CreatePipelineLayout(&waterPlDescriptor);

            wgpu::RenderPipelineDescriptor waterPipelineDescriptor{
                .layout = waterPl,
                .vertex = {.module = waterModule, .entryPoint = "vertexMain", .bufferCount = 1, .buffers = &vertexLayout},
                .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
                .depthStencil = &waterDepth,
                .fragment = &waterFragment};
            waterPipeline = device.CreateRenderPipeline(&waterPipelineDescriptor);

            wgpu::BindGroupEntry waterEntries[3] = {};
            waterEntries[0].binding = 0;
            waterEntries[0].buffer = uniformBuffer;
            waterEntries[0].size = sizeof(Uniforms);
            waterEntries[1].binding = 1;
            waterEntries[1].textureView = waterView;
            waterEntries[2].binding = 2;
            waterEntries[2].sampler = sampler;
            wgpu::BindGroupDescriptor waterBgDescriptor{.layout = waterBgl, .entryCount = 3, .entries = waterEntries};
            waterBindGroup = device.CreateBindGroup(&waterBgDescriptor);

            std::printf("water: %zu quads\n", zone->waterIndices.size() / 6);
        }

        std::unordered_map<std::string, wgpu::TextureView> uploadedViews;
        size_t uploaded = 0;
        size_t untextured = 0;
        for (const mh::InstancedDraw& batch : zone->draws)
        {
            // Cached by name: instancing produces one draw per mesh, and many
            // meshes share a texture. Uploading per draw meant 397 GPU textures
            // for 46 distinct images.
            wgpu::TextureView view = whiteView;
            if (!batch.texture.empty())
            {
                auto cached = uploadedViews.find(batch.texture);
                if (cached != uploadedViews.end())
                {
                    view = cached->second;
                }
                else
                {
                    auto found = textures.find(batch.texture);
                    if (found != textures.end())
                    {
                        wgpu::Texture gpu = mh::uploadTexture(device, found->second);
                        if (gpu)
                        {
                            batchTextures.push_back(gpu);
                            view = batchTextures.back().CreateView();
                            uploadedViews.emplace(batch.texture, view);
                            ++uploaded;
                        }
                    }
                    else
                    {
                        ++untextured;
                    }
                }
            }

            wgpu::BindGroupEntry entries[3] = {};
            entries[0].binding = 0;
            entries[0].buffer = uniformBuffer;
            entries[0].size = sizeof(Uniforms);
            entries[1].binding = 1;
            entries[1].textureView = view;
            entries[2].binding = 2;
            entries[2].sampler = sampler;

            wgpu::BindGroupDescriptor bindGroupDescriptor{
                .layout = zoneBindGroupLayout, .entryCount = 3, .entries = entries};
            batchBindGroups.push_back(device.CreateBindGroup(&bindGroupDescriptor));
        }
        std::printf("%zu draws, %zu textures uploaded, %zu with no texture in this DAT\n", zone->draws.size(),
                    uploaded, untextured);
    }

    // --- the map ------------------------------------------------------------
    // Baked once, straight down, through the pipeline that draws the zone - so
    // the radar shows the world as it actually looks rather than a schematic
    // redrawn from the same data.
    wgpu::Texture mapTexture;
    if (zone && pipeline && !zone->draws.empty())
    {
        constexpr uint32_t kMapSize = 2048;

        wgpu::TextureDescriptor mapDescriptor{
            .usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                     wgpu::TextureUsage::CopySrc,
            .dimension = wgpu::TextureDimension::e2D,
            .size = {kMapSize, kMapSize, 1},
            .format = surfaceFormat,
            .mipLevelCount = 1,
            .sampleCount = 1};
        mapTexture = device.CreateTexture(&mapDescriptor);

        wgpu::TextureDescriptor mapDepthDescriptor{.usage = wgpu::TextureUsage::RenderAttachment,
                                                   .dimension = wgpu::TextureDimension::e2D,
                                                   .size = {kMapSize, kMapSize, 1},
                                                   .format = kDepthFormat,
                                                   .mipLevelCount = 1,
                                                   .sampleCount = 1};
        wgpu::Texture mapDepth = device.CreateTexture(&mapDepthDescriptor);

        // A square covering the zone, so the map keeps the world aspect and a
        // step north is the same number of pixels as a step east.
        const mh::Vec3 lo = zone->boundsMin;
        const mh::Vec3 hi = zone->boundsMax;
        const float half = std::max(hi.x - lo.x, hi.z - lo.z) * 0.5f;
        const mh::Vec3 middle = zone->centre();

        Uniforms mapUniforms{};
        // Straight down, with +z up the screen so north is at the top.
        //
        // The left and right of the projection are swapped on purpose. Looking
        // straight down with north up, this look-at puts the world -x axis to
        // screen right - the map comes out mirrored east to west, which is
        // invisible on terrain this irregular and would send a player walking
        // east sliding west across the radar. Swapping the projection mirrors
        // it back. Measured, not reasoned: with both axes as they were, only
        // 80% of walkable texels landed on drawn terrain, against 100% once
        // corrected.
        const mh::Mat4 mapView = mh::lookAt({middle.x, hi.y + 100.0f, middle.z}, middle, {0.0f, 0.0f, 1.0f});
        const mh::Mat4 mapProjection = mh::orthographic(half, -half, -half, half, 1.0f, (hi.y - lo.y) + 400.0f);
        const mh::Mat4 mapViewProjection = mapProjection * mapView;
        std::memcpy(mapUniforms.viewProjection, mapViewProjection.m, sizeof(mapUniforms.viewProjection));

        // Flat and unfogged. A map lit from an angle reads as terrain in
        // shadow rather than as ground, and fog would erase the far half.
        const mh::Vec3 down = mh::normalise(mh::Vec3{0.0f, 1.0f, 0.0f});
        mapUniforms.lightDirection[0] = down.x;
        mapUniforms.lightDirection[1] = down.y;
        mapUniforms.lightDirection[2] = down.z;
        mapUniforms.ambient[0] = mapUniforms.ambient[1] = mapUniforms.ambient[2] = 0.75f;
        mapUniforms.sunlight[0] = mapUniforms.sunlight[1] = mapUniforms.sunlight[2] = 0.35f;
        mapUniforms.fogRange[0] = 1e9f;
        mapUniforms.fogRange[1] = 1e9f;
        queue.WriteBuffer(uniformBuffer, 0, &mapUniforms, sizeof(mapUniforms));

        wgpu::RenderPassColorAttachment mapColour{.view = mapTexture.CreateView(),
                                                  .loadOp = wgpu::LoadOp::Clear,
                                                  .storeOp = wgpu::StoreOp::Store,
                                                  .clearValue = {0.0f, 0.0f, 0.0f, 0.0f}};
        wgpu::RenderPassDepthStencilAttachment mapDepthAttachment{.view = mapDepth.CreateView(),
                                                                  .depthLoadOp = wgpu::LoadOp::Clear,
                                                                  .depthStoreOp = wgpu::StoreOp::Store,
                                                                  .depthClearValue = 1.0f};
        wgpu::RenderPassDescriptor mapPassDescriptor{.colorAttachmentCount = 1,
                                                     .colorAttachments = &mapColour,
                                                     .depthStencilAttachment = &mapDepthAttachment};

        wgpu::CommandEncoder mapEncoder = device.CreateCommandEncoder();
        wgpu::RenderPassEncoder mapPass = mapEncoder.BeginRenderPass(&mapPassDescriptor);
        mapPass.SetVertexBuffer(0, vertexBuffer);
        mapPass.SetVertexBuffer(1, instanceBuffer);
        mapPass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
        for (size_t i = 0; i < zone->draws.size() && i < batchBindGroups.size(); ++i)
        {
            const mh::InstancedDraw& draw = zone->draws[i];
            mapPass.SetPipeline(draw.cutout ? cutoutPipeline : pipeline);
            mapPass.SetBindGroup(0, batchBindGroups[i]);
            mapPass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
        }
        if (waterIndexCount && waterPipeline)
        {
            mapPass.SetPipeline(waterPipeline);
            mapPass.SetBindGroup(0, waterBindGroup);
            mapPass.SetVertexBuffer(0, waterVertexBuffer);
            mapPass.SetIndexBuffer(waterIndexBuffer, wgpu::IndexFormat::Uint32);
            mapPass.DrawIndexed(waterIndexCount);
        }
        mapPass.End();
        wgpu::CommandBuffer mapCommands = mapEncoder.Finish();
        queue.Submit(1, &mapCommands);

        std::printf("map: baked %ux%u covering %.0f units, centred on %.0f %.0f\n", kMapSize, kMapSize, half * 2.0f,
                    middle.x, middle.z);

        // mapPath writes the bake out so it can be looked at directly.
        if (options.mapPath)
        {
            const uint32_t bytesPerRow = (kMapSize * 4 + 255) / 256 * 256;
            wgpu::BufferDescriptor readDescriptor{.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead,
                                                  .size = static_cast<uint64_t>(bytesPerRow) * kMapSize};
            wgpu::Buffer readback = device.CreateBuffer(&readDescriptor);

            wgpu::CommandEncoder copyEncoder = device.CreateCommandEncoder();
            wgpu::TexelCopyTextureInfo source{.texture = mapTexture};
            wgpu::TexelCopyBufferInfo destination{
                .layout = {.bytesPerRow = bytesPerRow, .rowsPerImage = kMapSize}, .buffer = readback};
            const wgpu::Extent3D extent{kMapSize, kMapSize, 1};
            copyEncoder.CopyTextureToBuffer(&source, &destination, &extent);
            wgpu::CommandBuffer copyCommands = copyEncoder.Finish();
            queue.Submit(1, &copyCommands);

            // The mask over the same square, so the two can be checked against
            // each other. They are only useful together, and an offset here
            // would put every radar dot the same distance off the terrain it
            // is meant to sit on.
            {
                const std::vector<uint8_t> mask = collision.rasteriseWalkable(kMapSize, middle, half);
                const std::string maskPath = *options.mapPath + ".mask.pgm";
                if (std::FILE* file = std::fopen(maskPath.c_str(), "wb"))
                {
                    std::fprintf(file, "P5\n%u %u\n255\n", kMapSize, kMapSize);
                    std::fwrite(mask.data(), 1, mask.size(), file);
                    std::fclose(file);
                    std::printf("wrote %s\n", maskPath.c_str());
                }
            }

            bool mapped = false;
            wgpu::Future future = readback.MapAsync(wgpu::MapMode::Read, 0, wgpu::kWholeMapSize,
                                                    wgpu::CallbackMode::AllowProcessEvents,
                                                    [&](wgpu::MapAsyncStatus status, wgpu::StringView)
                                                    { mapped = status == wgpu::MapAsyncStatus::Success; });
            instance.WaitAny(future, UINT64_MAX);
            if (mapped)
            {
                const auto* pixels = static_cast<const uint8_t*>(readback.GetConstMappedRange());
                if (pixels && writeBmp(options.mapPath->c_str(), pixels, kMapSize, kMapSize, bytesPerRow))
                {
                    std::printf("wrote %s\n", options.mapPath->c_str());
                }
                readback.Unmap();
            }
        }
    }

    // --- the radar -----------------------------------------------------------
    wgpu::Texture maskTexture;
    wgpu::Buffer radarUniformBuffer;
    wgpu::RenderPipeline radarPipeline;
    wgpu::BindGroup radarBindGroup;
    float mapCentreX = 0.0f;
    float mapCentreZ = 0.0f;
    float mapHalf = 1.0f;

    if (mapTexture && !collision.empty())
    {
        constexpr uint32_t kMaskSize = 1024;
        const mh::Vec3 middle = zone->centre();
        const float half = std::max(zone->boundsMax.x - zone->boundsMin.x, zone->boundsMax.z - zone->boundsMin.z) * 0.5f;
        mapCentreX = middle.x;
        mapCentreZ = middle.z;
        mapHalf = half;

        const std::vector<uint8_t> mask = collision.rasteriseWalkable(kMaskSize, middle, half);

        wgpu::TextureDescriptor maskDescriptor{.usage = wgpu::TextureUsage::TextureBinding |
                                                        wgpu::TextureUsage::CopyDst,
                                               .dimension = wgpu::TextureDimension::e2D,
                                               .size = {kMaskSize, kMaskSize, 1},
                                               .format = wgpu::TextureFormat::R8Unorm,
                                               .mipLevelCount = 1,
                                               .sampleCount = 1};
        maskTexture = device.CreateTexture(&maskDescriptor);

        wgpu::TexelCopyTextureInfo maskDestination{.texture = maskTexture};
        wgpu::TexelCopyBufferLayout maskLayout{.bytesPerRow = kMaskSize, .rowsPerImage = kMaskSize};
        const wgpu::Extent3D maskExtent{kMaskSize, kMaskSize, 1};
        queue.WriteTexture(&maskDestination, mask.data(), mask.size(), &maskLayout, &maskExtent);

        wgpu::BufferDescriptor radarBufferDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                     .size = sizeof(RadarUniforms)};
        radarUniformBuffer = device.CreateBuffer(&radarBufferDescriptor);

        wgpu::ShaderSourceWGSL radarWgsl;
        radarWgsl.code = mh::kRadarShader;
        wgpu::ShaderModuleDescriptor radarModuleDescriptor{.nextInChain = &radarWgsl};
        wgpu::ShaderModule radarModule = device.CreateShaderModule(&radarModuleDescriptor);

        wgpu::BindGroupLayoutEntry radarLayoutEntries[4] = {};
        radarLayoutEntries[0].binding = 0;
        radarLayoutEntries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
        radarLayoutEntries[0].buffer.type = wgpu::BufferBindingType::Uniform;
        radarLayoutEntries[1].binding = 1;
        radarLayoutEntries[1].visibility = wgpu::ShaderStage::Fragment;
        radarLayoutEntries[1].texture.sampleType = wgpu::TextureSampleType::Float;
        radarLayoutEntries[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        radarLayoutEntries[2].binding = 2;
        radarLayoutEntries[2].visibility = wgpu::ShaderStage::Fragment;
        radarLayoutEntries[2].texture.sampleType = wgpu::TextureSampleType::Float;
        radarLayoutEntries[2].texture.viewDimension = wgpu::TextureViewDimension::e2D;
        radarLayoutEntries[3].binding = 3;
        radarLayoutEntries[3].visibility = wgpu::ShaderStage::Fragment;
        radarLayoutEntries[3].sampler.type = wgpu::SamplerBindingType::Filtering;

        wgpu::BindGroupLayoutDescriptor radarLayoutDescriptor{.entryCount = 4, .entries = radarLayoutEntries};
        wgpu::BindGroupLayout radarBindGroupLayout = device.CreateBindGroupLayout(&radarLayoutDescriptor);
        wgpu::PipelineLayoutDescriptor radarPipelineLayoutDescriptor{.bindGroupLayoutCount = 1,
                                                                     .bindGroupLayouts = &radarBindGroupLayout};
        wgpu::PipelineLayout radarPipelineLayout = device.CreatePipelineLayout(&radarPipelineLayoutDescriptor);

        wgpu::BlendState radarBlend{
            .color = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::SrcAlpha,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha},
            .alpha = {.operation = wgpu::BlendOperation::Add,
                      .srcFactor = wgpu::BlendFactor::One,
                      .dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha}};
        wgpu::ColorTargetState radarTarget{.format = surfaceFormat, .blend = &radarBlend};
        wgpu::FragmentState radarFragment{
            .module = radarModule, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &radarTarget};

        // Drawn over everything, so the depth test always passes and nothing is
        // written back.
        wgpu::DepthStencilState radarDepth{.format = kDepthFormat,
                                           .depthWriteEnabled = false,
                                           .depthCompare = wgpu::CompareFunction::Always};

        wgpu::RenderPipelineDescriptor radarPipelineDescriptor{
            .layout = radarPipelineLayout,
            .vertex = {.module = radarModule, .entryPoint = "vertexMain"},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &radarDepth,
            .fragment = &radarFragment};
        radarPipeline = device.CreateRenderPipeline(&radarPipelineDescriptor);

        wgpu::BindGroupEntry radarEntries[4] = {};
        radarEntries[0].binding = 0;
        radarEntries[0].buffer = radarUniformBuffer;
        radarEntries[0].size = sizeof(RadarUniforms);
        radarEntries[1].binding = 1;
        radarEntries[1].textureView = mapTexture.CreateView();
        radarEntries[2].binding = 2;
        radarEntries[2].textureView = maskTexture.CreateView();
        radarEntries[3].binding = 3;
        radarEntries[3].sampler = sampler;

        wgpu::BindGroupDescriptor radarBindGroupDescriptor{
            .layout = radarBindGroupLayout, .entryCount = 4, .entries = radarEntries};
        radarBindGroup = device.CreateBindGroup(&radarBindGroupDescriptor);

        std::printf("radar: ready, %zu entities to show\n", options.testEntities.size());
    }

    // `characterPath` is a semicolon-separated list of DATs to assemble
    // one character from, and MOGHOUSE_CHARACTER_AT is where to stand it.
    std::optional<LoadedCharacter> character;
    mh::Vec3 characterAt{};

    // `look` is what a player character actually is:
    // race,face,head,body,hands,legs,feet, all model ids. The skeleton comes
    // from the race and each slot from its own file, which is how a change of
    // outfit is one number rather than a different character.
    if (const char* lookEnv = options.look ? options.look->c_str() : nullptr)
    {
        ffxi::Look look;
        if (!ffxi::parseLook(lookEnv, look))
        {
            std::printf("MOGHOUSE_LOOK wants race,face,head,body,hands,legs,feet\n");
        }
        else
        {
            try
            {
                const ffxi::FileTable table{ffxi::defaultInstallRoot()};
                std::vector<std::string> paths;

                // The skeleton file first: it carries the bones every piece is
                // hung on, and nothing but the race decides it.
                if (auto skeleton = table.path(ffxi::skeletonFileId(look.race)))
                {
                    paths.push_back(skeleton->string());
                }
                std::printf("look: %s", ffxi::raceName(look.race));
                for (size_t i = 0; i < static_cast<size_t>(ffxi::LookSlot::Count); ++i)
                {
                    const auto slot = static_cast<ffxi::LookSlot>(i);
                    const size_t fileId = ffxi::modelFileId(look.race, slot, look.model[i]);
                    auto path = fileId ? table.path(fileId) : std::nullopt;
                    std::printf(", %s %u%s", ffxi::slotName(slot), look.model[i], path ? "" : " (missing)");
                    if (path)
                    {
                        paths.push_back(path->string());
                    }
                }
                std::printf("\n");
                character = loadCharacter(paths, textures);
            }
            catch (const std::exception& e)
            {
                std::printf("could not read the file table: %s\n", e.what());
            }
        }
    }
    else if (const char* charEnv = options.characterPath ? options.characterPath->c_str() : nullptr)
    {
        std::vector<std::string> paths;
        std::string current;
        for (const char* c = charEnv; *c; ++c)
        {
            if (*c == ';')
            {
                if (!current.empty())
                {
                    paths.push_back(current);
                }
                current.clear();
            }
            else
            {
                current.push_back(*c);
            }
        }
        if (!current.empty())
        {
            paths.push_back(current);
        }
        character = loadCharacter(paths, textures);
    }

    // The character reuses the zone's pipelines and bind group layout, so it
    // needs a zone loaded. Its geometry is already in world orientation, so
    // its one instance is a plain translation rather than a placement matrix.
    wgpu::Buffer characterVertexBuffer;
    wgpu::Buffer characterIndexBuffer;
    wgpu::Buffer characterInstanceBuffer;
    std::vector<wgpu::Texture> characterTextures;
    std::vector<wgpu::BindGroup> characterBindGroups;
    if (character && !character->geometry.indices.empty() && pipeline)
    {
        characterVertexBuffer = createBuffer(device, character->geometry.vertices.data(),
                                             character->geometry.vertices.size() * sizeof(mh::Vertex),
                                             wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
        characterIndexBuffer = createBuffer(device, character->geometry.indices.data(),
                                            character->geometry.indices.size() * sizeof(uint32_t), wgpu::BufferUsage::Index);

        float instance[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        wgpu::BufferDescriptor instanceDescriptor{.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                                                  .size = sizeof(instance)};
        characterInstanceBuffer = device.CreateBuffer(&instanceDescriptor);
        queue.WriteBuffer(characterInstanceBuffer, 0, instance, sizeof(instance));

        for (const mh::Batch& batch : character->geometry.batches)
        {
            wgpu::TextureView view = whiteTexture.CreateView();
            auto found = textures.find(batch.texture);
            if (found != textures.end())
            {
                if (wgpu::Texture gpu = mh::uploadTexture(device, found->second))
                {
                    characterTextures.push_back(gpu);
                    view = characterTextures.back().CreateView();
                }
            }
            else
            {
                std::printf("character texture %s is in none of those files\n", batch.texture.c_str());
            }

            wgpu::BindGroupEntry entries[3] = {};
            entries[0].binding = 0;
            entries[0].buffer = uniformBuffer;
            entries[0].size = sizeof(Uniforms);
            entries[1].binding = 1;
            entries[1].textureView = view;
            entries[2].binding = 2;
            entries[2].sampler = sampler;

            wgpu::BindGroupDescriptor bindGroupDescriptor{
                .layout = zoneBindGroupLayout, .entryCount = 3, .entries = entries};
            characterBindGroups.push_back(device.CreateBindGroup(&bindGroupDescriptor));
        }
    }
    else if (character)
    {
        std::printf("a character needs a zone loaded: it draws with the zone pipelines\n");
    }

    const mh::Vec3 centre = zone ? zone->centre() : mh::Vec3{};
    const float radius = zone ? std::max(zone->radius(), 1.0f) : 1.0f;

    mh::Camera camera;
    camera.target = centre;
    camera.distance = radius * 2.4f;
    // Start at the middle of the zone in all three axes. Starting from the
    // bottom of the bounds put the camera 25 units under the terrain in
    // Sarutabaruta, looking at the underside of the world - which reads as the
    // zone being mostly missing rather than as being in the wrong place.
    camera.position = centre;
    camera.pitch = 0.0f;

    // Somewhere visible by default, since nothing yet knows where the ground
    // is. MOGHOUSE_CHARACTER_AT overrides it, and c drops the character at
    // wherever the camera is standing.
    characterAt = centre;
    if (const char* atEnv = options.characterAt ? options.characterAt->c_str() : nullptr)
    {
        std::sscanf(atEnv, "%f,%f,%f", &characterAt.x, &characterAt.y, &characterAt.z);
    }
    // The model faces +x in its own space, so a heading of zero looks east.
    float characterFacing = 0.0f;
    if (const char* facingEnv = options.characterFacing ? options.characterFacing->c_str() : nullptr)
    {
        characterFacing = static_cast<float>(std::atof(facingEnv)) * 3.14159265f / 180.0f;
    }
    // `search` is how far to look for ground when there is none directly
    // below. Dropping a character into a zone wants a wide sweep; a footstep
    // wants none at all. Sharing one value is what let a step off a ledge
    // teleport into the water sixty units away, which is not falling and is
    // not blocking either.
    auto placeCharacter = [&](const mh::Vec3& where, float search) {
        characterAt = where;
        if (const std::optional<mh::Vec3> ground =
                collision.nearestGround(where.x, where.z, where.y + 1.0f, search))
        {
            characterAt = *ground;
        }
        if (characterInstanceBuffer)
        {
            // characterFacing is a compass heading: 0 is +z, and the
            // direction is (sin, cos) - the same convention camera.yaw and the
            // radar notch use. The model faces +x when unrotated, so the
            // rotation that points it along the heading is a quarter turn less.
            const float turn = 1.57079633f - characterFacing;
            const float c = std::cos(turn);
            const float sn = std::sin(turn);
            const float instance[16] = {c,       0, -sn,      0, 0,       1, 0,       0,
                                        sn,      0, c,        0, where.x, where.y, where.z, 1};
            queue.WriteBuffer(characterInstanceBuffer, 0, instance, sizeof(instance));
        }
    };
    placeCharacter(characterAt, 60.0f);
    if (character)
    {
        std::printf("character stands at %.1f %.1f %.1f%s\n", characterAt.x, characterAt.y, characterAt.z,
                    collision.empty() ? " (no collision - movement unconstrained)" : "");
    }

    // Framing a shot from a script needs the camera to be settable; dragging
    // it into place by hand cannot be repeated.
    if (const char* cameraEnv = options.camera ? options.camera->c_str() : nullptr)
    {
        std::sscanf(cameraEnv, "%f,%f,%f", &camera.position.x, &camera.position.y, &camera.position.z);
    }
    if (const char* lookEnv = options.cameraLook ? options.cameraLook->c_str() : nullptr)
    {
        float yawDegrees = 0.0f;
        float pitchDegrees = 0.0f;
        std::sscanf(lookEnv, "%f,%f", &yawDegrees, &pitchDegrees);
        camera.yaw = yawDegrees * 3.14159265f / 180.0f;
        camera.pitch = pitchDegrees * 3.14159265f / 180.0f;
    }

    bool dragging = false;

    std::printf("wasd to walk, mouse drag to look, space and ctrl for up and down,\n");
    std::printf("shift to run, tab to orbit, p to print position, c to place the character,\n");
    std::printf("f to swap between driving the character and flying the camera,\n");
    std::printf("escape to quit\n");

    // `screenshotPath` writes one frame to a BMP and quits. Without it
    // there is no way to check what the renderer actually produced except by
    // looking at the window, which rules out checking anything unattended.
    const char* screenshotPath = options.screenshotPath ? options.screenshotPath->c_str() : nullptr;

    // `screenshotSequence` captures a whole animation in one run,
    // one file per source frame, with the path taken as a printf format. The
    // alternative is relaunching for each frame, which for a walk cycle is
    // eighteen launches of a program that spends most of its time loading a
    // zone.
    const int sequenceCount = options.screenshotSequence;
    int shotIndex = -5; // let the first frames settle
    wgpu::Buffer readbackBuffer;

    // `animation` names one of the character's own animations - idl0
    // for a standing idle, wlk0 to walk, run0 to run. Skinning runs on the CPU
    // and the vertices are rewritten each frame: a couple of thousand
    // triangles is nothing next to a zone, and it keeps the pose maths
    // somewhere it can be read.
    const ffxi::Animation* playing = nullptr;
    const ffxi::Animation* idleClip = nullptr;
    const ffxi::Animation* walkClip = nullptr;
    const ffxi::Animation* runClip = nullptr;
    // MOGHOUSE_ANIMATION pins one clip; without it, movement picks.
    const bool pinnedClip = options.animation ? options.animation->c_str() : nullptr != nullptr;
    float animationOffset = 0.0f;
    bool driving = character.has_value();

    if (character && !character->animations.empty())
    {
        auto find = [&](const char* name) -> const ffxi::Animation* {
            auto found = character->animations.find(name);
            return found == character->animations.end() ? nullptr : &found->second;
        };
        idleClip = find("idl0");
        walkClip = find("wlk0");
        runClip = find("run0");

        const char* wanted = options.animation ? options.animation->c_str() : nullptr;
        auto found = character->animations.find(wanted ? wanted : "idl0");
        if (found == character->animations.end() && wanted)
        {
            std::printf("no animation called %s; this character has:", wanted);
            for (const auto& [name, unused] : character->animations)
            {
                std::printf(" %s", name.c_str());
            }
            std::printf("\n");
        }
        if (found != character->animations.end())
        {
            playing = &found->second;
            std::printf("playing %s: %u frames at %.1f a second\n", playing->name.c_str(), playing->frames,
                        1.0f / playing->frameSeconds());
        }
    }

    // `frame` pins the animation clock so a screenshot of a moving
    // character lands on the same pose every time.
    // What the radar shows. Fed from options until something is connected,
      // and replaced wholesale rather than merged - the list is already the
      // answer to "what can be seen right now".
    std::vector<mh::RadarEntity> radarEntities = options.testEntities;
    float radarRange = 120.0f;

    const float pinnedFrame = options.frame.value_or(-1.0f);
    const float shaderMode = options.shaderMode;
    const int cutoutMode = options.cutoutMode;

    // `timeOfDay` = 1830 pins the clock; otherwise a Vana'diel day passes in
    // one real minute, which is fast but makes the whole cycle visible.
    const bool timeFixed = options.timeOfDay.has_value();
    const int fixedMinutes = timeFixed ? (*options.timeOfDay / 100) * 60 + (*options.timeOfDay % 100) : 0;
    if (!lighting.empty())
    {
        std::printf("lighting: %zu times of day, clock %s\n", lighting.sets().size(),
                    timeFixed ? "fixed" : "running");
    }

    uint64_t previousTicks = SDL_GetTicksNS();
    bool running = true;
    while (running)
    {
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                running = false;
            }
            else if (event.type == SDL_EVENT_KEY_DOWN)
            {
                if (event.key.key == SDLK_ESCAPE)
                {
                    running = false;
                }
                else if (event.key.key == SDLK_TAB)
                {
                    camera.orbiting = !camera.orbiting;
                }
                else if (event.key.key == SDLK_P)
                {
                    const mh::Vec3 at = camera.eye();
                    std::printf("at %.1f %.1f %.1f   zone y runs %.1f to %.1f\n", at.x, at.y, at.z,
                                zone->boundsMin.y, zone->boundsMax.y);
                }
                else if (event.key.key == SDLK_F && character)
                {
                    driving = !driving;
                    camera.orbiting = driving;
                    if (!driving)
                    {
                        camera.position = camera.eye();
                    }
                    std::printf("%s\n", driving ? "driving the character" : "flying the camera");
                }
                else if (event.key.key == SDLK_C && character)
                {
                    // Stand the character where the camera is. Nothing knows
                    // where the ground is yet, so putting it somewhere useful
                    // is a matter of walking there and pressing a key.
                    const mh::Vec3 at = camera.eye();
                    placeCharacter(at, 60.0f);
                    std::printf("character moved to %.1f %.1f %.1f\n", at.x, at.y, at.z);
                }
            }
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                configure(static_cast<uint32_t>(event.window.data1), static_cast<uint32_t>(event.window.data2));
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            {
                dragging = true;
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            {
                dragging = false;
            }
            else if (event.type == SDL_EVENT_MOUSE_MOTION && dragging)
            {
                camera.look(-event.motion.xrel * 0.005f, -event.motion.yrel * 0.005f);
            }
            else if (event.type == SDL_EVENT_MOUSE_WHEEL)
            {
                camera.distance = std::clamp(camera.distance * (event.wheel.y > 0 ? 0.9f : 1.1f), radius * 0.05f,
                                             radius * 12.0f);
            }
        }

        // A Vana'diel day in a real minute when not pinned.
        const int clockMinutes =
            timeFixed ? fixedMinutes : static_cast<int>((SDL_GetTicksNS() / 1000000ull / 42ull) % 1440ull);

        const uint64_t nowTicks = SDL_GetTicksNS();
        const float delta = static_cast<float>(nowTicks - previousTicks) / 1e9f;
        previousTicks = nowTicks;
        const bool* held = SDL_GetKeyboardState(nullptr);
        float speed = 12.0f * delta;
        if (held[SDL_SCANCODE_LSHIFT] || held[SDL_SCANCODE_RSHIFT])
        {
            speed *= 5.0f;
        }
        const float ahead = (held[SDL_SCANCODE_W] ? speed : 0.0f) - (held[SDL_SCANCODE_S] ? speed : 0.0f);
        const float side = (held[SDL_SCANCODE_D] ? speed : 0.0f) - (held[SDL_SCANCODE_A] ? speed : 0.0f);
        const float lift = (held[SDL_SCANCODE_SPACE] ? speed : 0.0f) - (held[SDL_SCANCODE_LCTRL] ? speed : 0.0f);

        // Two ways to move: fly the camera around to look at the zone, or
        // drive the character and have the camera follow. A character in the
        // world starts in the second.
        float moved = 0.0f;
        if (driving && character)
        {
            // Movement is relative to where the camera is looking, which is
            // what makes it read as steering a person rather than nudging a
            // point on a map.
            const mh::Vec3 forward = mh::normalise(mh::Vec3{std::sin(camera.yaw), 0.0f, std::cos(camera.yaw)});
            const mh::Vec3 right = mh::normalise(mh::cross(forward, mh::Vec3{0.0f, 1.0f, 0.0f}));

            const mh::Vec3 wanted{characterAt.x + forward.x * ahead + right.x * side, characterAt.y,
                                  characterAt.z + forward.z * ahead + right.z * side};
            if (wanted.x != characterAt.x || wanted.z != characterAt.z)
            {
                const mh::Vec3 stepped = collision.empty() ? wanted : collision.move(characterAt, wanted, 0.5f);

                // A step needs ground directly under it. Searching outward for
                // some, the way an initial drop does, turns walking off a ledge
                // into a jump to wherever the nearest surface happens to be.
                const bool footing =
                    collision.empty() || collision.groundAt(stepped.x, stepped.z, characterAt.y + 1.0f).has_value();

                if (footing)
                {
                    const float dx = stepped.x - characterAt.x;
                    const float dz = stepped.z - characterAt.z;
                    moved = std::sqrt(dx * dx + dz * dz);
                    placeCharacter(stepped, 0.0f);
                }

                // Facing follows the camera, not the step. Walking backwards or
                // strafing should not spin the character round, and taking the
                // direction from the movement delta means sliding along a wall
                // turns you to face it.
                characterFacing = camera.yaw;
            }

            // The camera sits behind and above the character, at head height.
            camera.orbiting = true;
            camera.target = {characterAt.x, characterAt.y + 1.2f, characterAt.z};
            camera.distance = std::clamp(camera.distance - lift * 0.6f, 1.5f, 25.0f);
        }
        else
        {
            camera.walk(ahead, side, lift);
        }

        // Idle, walk or run, chosen by what the character is actually doing.
        if (!pinnedClip)
        {
            const bool running = held[SDL_SCANCODE_LSHIFT] || held[SDL_SCANCODE_RSHIFT];
            const ffxi::Animation* wanted = moved > 1e-4f ? (running ? runClip : walkClip) : idleClip;
            if (wanted && wanted != playing)
            {
                playing = wanted;
                animationOffset = static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;
            }
        }

        // A capture walks the animation a frame at a time; otherwise the clock
        // is either pinned or real.
        const float animationSeconds =
            sequenceCount > 0 && playing ? static_cast<float>(std::max(shotIndex, 0)) * playing->frameSeconds()
            : pinnedFrame >= 0.0f && playing
                ? pinnedFrame * playing->frameSeconds()
                : static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f - animationOffset;

        wgpu::SurfaceTexture surfaceTexture;
        surface.GetCurrentTexture(&surfaceTexture);
        if (!surfaceTexture.texture)
        {
            continue;
        }

        const uint32_t width = surfaceTexture.texture.GetWidth();
        const uint32_t height = surfaceTexture.texture.GetHeight();

        wgpu::RenderPassColorAttachment colour{.view = surfaceTexture.texture.CreateView(),
                                               .loadOp = wgpu::LoadOp::Clear,
                                               .storeOp = wgpu::StoreOp::Store,
                                               .clearValue = {0.05, 0.07, 0.09, 1.0}};
        wgpu::RenderPassDepthStencilAttachment depth{.view = depthTexture.CreateView(),
                                                     .depthLoadOp = wgpu::LoadOp::Clear,
                                                     .depthStoreOp = wgpu::StoreOp::Store,
                                                     .depthClearValue = 1.0f};
        wgpu::RenderPassDescriptor passDescriptor{.colorAttachmentCount = 1,
                                                  .colorAttachments = &colour,
                                                  .depthStencilAttachment = &depth};

        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDescriptor);

        {
            const float aspect = static_cast<float>(width) / static_cast<float>(height);
            const float tanHalfFov = std::tan(1.05f * 0.5f);
            const mh::Vec3 f = camera.orbiting ? mh::normalise(camera.lookAtPoint() - camera.eye()) : camera.forward();
            const mh::Vec3 r = mh::normalise(mh::cross(f, mh::Vec3{0.0f, 1.0f, 0.0f}));
            const mh::Vec3 u = mh::cross(r, f);

            SkyUniforms skyUniforms{};
            skyUniforms.forward[0] = f.x;
            skyUniforms.forward[1] = f.y;
            skyUniforms.forward[2] = f.z;
            skyUniforms.right[0] = r.x * tanHalfFov * aspect;
            skyUniforms.right[1] = r.y * tanHalfFov * aspect;
            skyUniforms.right[2] = r.z * tanHalfFov * aspect;
            skyUniforms.up[0] = u.x * tanHalfFov;
            skyUniforms.up[1] = u.y * tanHalfFov;
            skyUniforms.up[2] = u.z * tanHalfFov;

            const ffxi::LightingSet skySet = lighting.at(clockMinutes);
            for (size_t i = 0; i < 8; ++i)
            {
                skyUniforms.skyColours[i][0] = skySet.skyColours[i].r;
                skyUniforms.skyColours[i][1] = skySet.skyColours[i].g;
                skyUniforms.skyColours[i][2] = skySet.skyColours[i].b;
                skyUniforms.skyAltitudes[i][0] = skySet.skyAltitudes[i];
            }
            skyUniforms.fogColour[0] = skySet.landscapeFog.r;
            skyUniforms.fogColour[1] = skySet.landscapeFog.g;
            skyUniforms.fogColour[2] = skySet.landscapeFog.b;
            queue.WriteBuffer(skyUniformBuffer, 0, &skyUniforms, sizeof(skyUniforms));

            pass.SetPipeline(skyPipeline);
            pass.SetBindGroup(0, skyBindGroup);
            pass.Draw(3);
        }

        if (indexCount)
        {
            const mh::Mat4 view = mh::lookAt(camera.eye(), camera.lookAtPoint(), mh::Vec3{0, 1, 0});
            // A near plane scaled to the zone puts everything nearby inside it
            // when standing on the ground, so it is fixed rather than relative.
            const mh::Mat4 projection =
                // A far plane 20x the zone radius wastes most of the depth
                // buffer's precision on space nothing occupies, which is what
                // makes coplanar layers fight in the first place.
                mh::perspective(1.05f, static_cast<float>(width) / static_cast<float>(height), 0.25f, radius * 4.0f);

            Uniforms uniforms{};
            const mh::Mat4 viewProjection = projection * view;
            std::memcpy(uniforms.viewProjection, viewProjection.m, sizeof(uniforms.viewProjection));
            const mh::Vec3 light = mh::normalise(mh::Vec3{0.4f, 0.8f, 0.45f});
            uniforms.lightDirection[0] = light.x;
            uniforms.lightDirection[1] = light.y;
            uniforms.lightDirection[2] = light.z;

            const ffxi::LightingSet set = lighting.at(clockMinutes);
            uniforms.ambient[0] = set.landscapeAmbient.r * 0.5f;
            uniforms.ambient[1] = set.landscapeAmbient.g * 0.5f;
            uniforms.ambient[2] = set.landscapeAmbient.b * 0.5f;
            uniforms.sunlight[0] = set.landscapeSunlight.r * 0.5f;
            uniforms.sunlight[1] = set.landscapeSunlight.g * 0.5f;
            uniforms.sunlight[2] = set.landscapeSunlight.b * 0.5f;
            uniforms.fogColour[0] = set.landscapeFog.r;
            uniforms.fogColour[1] = set.landscapeFog.g;
            uniforms.fogColour[2] = set.landscapeFog.b;
            uniforms.fogRange[0] = set.landscapeMinFog;
            uniforms.fogRange[1] = set.landscapeMaxFog > 0.0f ? set.landscapeMaxFog : 10000.0f;
            const mh::Vec3 eyePoint = camera.eye();
            uniforms.eye[0] = eyePoint.x;
            uniforms.eye[1] = eyePoint.y;
            uniforms.eye[2] = eyePoint.z;
            // Seconds since start, for the water surface.
            uniforms.eye[3] = animationSeconds;

            // With no lighting data, fall back to a plain lit look rather than
            // a black zone.
            if (lighting.empty())
            {
                uniforms.ambient[0] = uniforms.ambient[1] = uniforms.ambient[2] = 0.35f;
                uniforms.sunlight[0] = uniforms.sunlight[1] = uniforms.sunlight[2] = 0.65f;
                uniforms.fogRange[1] = 1e9f;
            }
            // shaderMode: 0 draws colour with no alpha discard,
            // 2 draws alpha as greyscale, unset is normal rendering.
            uniforms.lightDirection[3] = shaderMode;
            queue.WriteBuffer(uniformBuffer, 0, &uniforms, sizeof(uniforms));

            pass.SetVertexBuffer(0, vertexBuffer);
            pass.SetVertexBuffer(1, instanceBuffer);
            pass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
            for (size_t i = 0; i < zone->draws.size() && i < batchBindGroups.size(); ++i)
            {
                const mh::InstancedDraw& draw = zone->draws[i];
                // cutoutMode: 0 never cuts out, 1 always, otherwise the
                // texture's own measurement decides.
                const bool cutout = cutoutMode == 0   ? false
                                    : cutoutMode == 1 ? true
                                                      : draw.cutout;
                pass.SetPipeline(cutout ? cutoutPipeline : pipeline);
                pass.SetBindGroup(0, batchBindGroups[i]);
                pass.DrawIndexed(draw.indexCount, draw.instanceCount, draw.indexOffset, 0, draw.instanceOffset);
            }

            if (!characterBindGroups.empty())
            {
                if (playing)
                {
                    const float frame = animationSeconds / playing->frameSeconds();
                    mh::reskin(character->geometry, mh::animatedPose(character->skeleton, *playing, frame),
                               character->meshes);
                    queue.WriteBuffer(characterVertexBuffer, 0, character->geometry.vertices.data(),
                                      character->geometry.vertices.size() * sizeof(mh::Vertex));
                }
                pass.SetVertexBuffer(0, characterVertexBuffer);
                pass.SetVertexBuffer(1, characterInstanceBuffer);
                pass.SetIndexBuffer(characterIndexBuffer, wgpu::IndexFormat::Uint32);
                for (size_t i = 0; i < character->geometry.batches.size() && i < characterBindGroups.size(); ++i)
                {
                    const mh::Batch& batch = character->geometry.batches[i];
                    pass.SetPipeline(batch.cutout ? cutoutPipeline : pipeline);
                    pass.SetBindGroup(0, characterBindGroups[i]);
                    pass.DrawIndexed(batch.indexCount, 1, batch.indexOffset, 0, 0);
                }
            }

            if (radarPipeline && radarBindGroup)
            {
                RadarUniforms radar{};
                // Top right, a fifth of the shorter side across.
                radar.placement[0] = 0.78f;
                radar.placement[1] = 0.70f;
                radar.placement[2] = 0.26f;
                radar.placement[3] = static_cast<float>(width) / static_cast<float>(height);

                radar.mapExtent[0] = mapCentreX;
                radar.mapExtent[1] = mapCentreZ;
                radar.mapExtent[2] = mapHalf;

                // The character if there is one, otherwise wherever the camera
                // is - a radar centred on nothing is worse than no radar.
                const mh::Vec3 eyePosition = camera.eye();
                radar.viewer[0] = character ? characterAt.x : eyePosition.x;
                radar.viewer[1] = character ? characterAt.z : eyePosition.z;
                radar.viewer[2] = character ? characterFacing : camera.yaw;
                radar.viewer[3] = radarRange;

                const size_t shown = std::min(radarEntities.size(), static_cast<size_t>(mh::kRadarMaxEntities));
                radar.counts[0] = static_cast<float>(shown);
                for (size_t i = 0; i < shown; ++i)
                {
                    radar.entities[i][0] = radarEntities[i].x;
                    radar.entities[i][1] = radarEntities[i].z;
                    radar.entities[i][2] = static_cast<float>(radarEntities[i].kind);
                }
                queue.WriteBuffer(radarUniformBuffer, 0, &radar, sizeof(radar));

                pass.SetPipeline(radarPipeline);
                pass.SetBindGroup(0, radarBindGroup);
                pass.Draw(3);
            }

            if (waterIndexCount && waterPipeline)
            {
                pass.SetPipeline(waterPipeline);
                pass.SetBindGroup(0, waterBindGroup);
                pass.SetVertexBuffer(0, waterVertexBuffer);
                pass.SetIndexBuffer(waterIndexBuffer, wgpu::IndexFormat::Uint32);
                pass.DrawIndexed(waterIndexCount);
            }
        }

        pass.End();

        // A texture copy has to start on a 256-byte row, so the readback is
        // padded and the padding is skipped when the rows are written out.
        const uint32_t bytesPerRow = (width * 4 + 255) / 256 * 256;
        const bool takingShot = screenshotPath && ++shotIndex >= 0 && (sequenceCount == 0 || shotIndex < sequenceCount);
        char shotPath[1024] = {};
        if (takingShot)
        {
            if (sequenceCount > 0)
            {
                std::snprintf(shotPath, sizeof(shotPath), screenshotPath, shotIndex);
            }
            else
            {
                std::snprintf(shotPath, sizeof(shotPath), "%s", screenshotPath);
            }
        }
        if (takingShot)
        {
            wgpu::BufferDescriptor readbackDescriptor{.usage = wgpu::BufferUsage::CopyDst |
                                                               wgpu::BufferUsage::MapRead,
                                                      .size = static_cast<uint64_t>(bytesPerRow) * height};
            readbackBuffer = device.CreateBuffer(&readbackDescriptor);

            wgpu::TexelCopyTextureInfo source{.texture = surfaceTexture.texture};
            wgpu::TexelCopyBufferInfo destination{
                .layout = {.bytesPerRow = bytesPerRow, .rowsPerImage = height}, .buffer = readbackBuffer};
            const wgpu::Extent3D extent{width, height, 1};
            encoder.CopyTextureToBuffer(&source, &destination, &extent);
        }

        wgpu::CommandBuffer commands = encoder.Finish();
        queue.Submit(1, &commands);
        surface.Present();
        instance.ProcessEvents();

        if (takingShot)
        {
            bool mapped = false;
            wgpu::Future future = readbackBuffer.MapAsync(
                wgpu::MapMode::Read, 0, wgpu::kWholeMapSize, wgpu::CallbackMode::AllowProcessEvents,
                [&](wgpu::MapAsyncStatus status, wgpu::StringView)
                { mapped = status == wgpu::MapAsyncStatus::Success; });
            instance.WaitAny(future, UINT64_MAX);

            if (mapped)
            {
                const auto* pixels = static_cast<const uint8_t*>(readbackBuffer.GetConstMappedRange());
                if (pixels && writeBmp(shotPath, pixels, width, height, bytesPerRow))
                {
                    std::printf("wrote %s (%ux%u)\n", shotPath, width, height);
                }
                else
                {
                    std::printf("could not write %s\n", shotPath);
                }
                readbackBuffer.Unmap();
            }
            else
            {
                std::printf("could not read the frame back\n");
            }

            if (sequenceCount == 0 || shotIndex + 1 >= sequenceCount)
            {
                break;
            }
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
