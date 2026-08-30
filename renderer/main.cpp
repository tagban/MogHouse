// PortJeuno's renderer. Opens a window on WebGPU - Metal on macOS, D3D12 on
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
#include "coverage.h"
#include "linalg.h"
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
    pj::Character geometry;
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

    loaded.geometry = pj::buildCharacter(pj::bindPose(skeleton), meshes, textures);
    std::printf("character: %zu bones, %zu meshes, %zu triangles, %.2f tall, %zu animations\n",
                skeleton.bones.size(), meshes.size(), loaded.geometry.triangles(), loaded.geometry.height(),
                loaded.animations.size());
    return loaded;
}

std::optional<pj::Scene> loadZone(const char* datPath, const char* keyPath, const char* key2Path, std::string& zoneId,
                                     std::unordered_map<std::string, ffxi::Texture>& textures, ffxi::Lighting& lighting)
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

    std::optional<pj::Scene> best;
    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMzb))
    {
        ffxi::Zone zone = ffxi::parseMzb(chunk, *keys);

        // Placed models are the visible world; collision geometry is the
        // fallback when the model key table is not available.
        pj::Scene mesh;
        if (!models.empty())
        {
            size_t resolved = 0;
            size_t missing = 0;
            mesh = pj::buildScene(zone, models, textures, resolved, missing);
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

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    std::string zoneId;
    std::optional<pj::Scene> zone;
    std::unordered_map<std::string, ffxi::Texture> textures;
    ffxi::Lighting lighting;
    if (argc > 1)
    {
        const char* keyPath = std::getenv("PORTJEUNO_FFXI_KEYTABLE");
        if (!keyPath)
        {
            std::printf("set PORTJEUNO_FFXI_KEYTABLE to the 256-byte MZB key table to load a zone\n");
            return 2;
        }
        zone = loadZone(argv[1], keyPath, std::getenv("PORTJEUNO_FFXI_KEYTABLE2"), zoneId, textures, lighting);
        if (!zone)
        {
            return 1;
        }
        // The character is loaded after the zone so it can share the texture
        // map: a PC in a town wears textures the zone never mentions, and a
        // zone texture the character happens to name should not be read twice.
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

    SDL_Window* window = SDL_CreateWindow("PortJeuno renderer", kWidth, kHeight,
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

    wgpu::Surface surface = pj::CreateSurface(instance, window);
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
    skyWgsl.code = pj::kSkyShader;
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
        vertexBuffer = createBuffer(device, zone->vertices.data(), zone->vertices.size() * sizeof(pj::Vertex),
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
        wgsl.code = pj::kZoneShader;
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
                                              .arrayStride = sizeof(pj::Vertex),
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

        whiteTexture = pj::createWhiteTexture(device);
        const wgpu::TextureView whiteView = whiteTexture.CreateView();

        if (!zone->waterIndices.empty())
        {
            waterVertexBuffer = createBuffer(device, zone->waterVertices.data(),
                                             zone->waterVertices.size() * sizeof(pj::Vertex), wgpu::BufferUsage::Vertex);
            waterIndexBuffer = createBuffer(device, zone->waterIndices.data(),
                                            zone->waterIndices.size() * sizeof(uint32_t), wgpu::BufferUsage::Index);
            waterIndexCount = static_cast<uint32_t>(zone->waterIndices.size());

            wgpu::ShaderSourceWGSL waterWgsl;
            waterWgsl.code = pj::kWaterShader;
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
                    wgpu::Texture gpu = pj::uploadTexture(device, found->second);
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
        for (const pj::InstancedDraw& batch : zone->draws)
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
                        wgpu::Texture gpu = pj::uploadTexture(device, found->second);
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

    // PORTJEUNO_CHARACTER is a semicolon-separated list of DATs to assemble
    // one character from, and PORTJEUNO_CHARACTER_AT is where to stand it.
    std::optional<LoadedCharacter> character;
    pj::Vec3 characterAt{};

    // PORTJEUNO_LOOK is what a player character actually is:
    // race,face,head,body,hands,legs,feet, all model ids. The skeleton comes
    // from the race and each slot from its own file, which is how a change of
    // outfit is one number rather than a different character.
    if (const char* lookEnv = std::getenv("PORTJEUNO_LOOK"))
    {
        ffxi::Look look;
        if (!ffxi::parseLook(lookEnv, look))
        {
            std::printf("PORTJEUNO_LOOK wants race,face,head,body,hands,legs,feet\n");
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
    else if (const char* charEnv = std::getenv("PORTJEUNO_CHARACTER"))
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
                                             character->geometry.vertices.size() * sizeof(pj::Vertex),
                                             wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
        characterIndexBuffer = createBuffer(device, character->geometry.indices.data(),
                                            character->geometry.indices.size() * sizeof(uint32_t), wgpu::BufferUsage::Index);

        float instance[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        wgpu::BufferDescriptor instanceDescriptor{.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst,
                                                  .size = sizeof(instance)};
        characterInstanceBuffer = device.CreateBuffer(&instanceDescriptor);
        queue.WriteBuffer(characterInstanceBuffer, 0, instance, sizeof(instance));

        for (const pj::Batch& batch : character->geometry.batches)
        {
            wgpu::TextureView view = whiteTexture.CreateView();
            auto found = textures.find(batch.texture);
            if (found != textures.end())
            {
                if (wgpu::Texture gpu = pj::uploadTexture(device, found->second))
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

    const pj::Vec3 centre = zone ? zone->centre() : pj::Vec3{};
    const float radius = zone ? std::max(zone->radius(), 1.0f) : 1.0f;

    pj::Camera camera;
    camera.target = centre;
    camera.distance = radius * 2.4f;
    // Start at the middle of the zone in all three axes. Starting from the
    // bottom of the bounds put the camera 25 units under the terrain in
    // Sarutabaruta, looking at the underside of the world - which reads as the
    // zone being mostly missing rather than as being in the wrong place.
    camera.position = centre;
    camera.pitch = 0.0f;

    // Somewhere visible by default, since nothing yet knows where the ground
    // is. PORTJEUNO_CHARACTER_AT overrides it, and c drops the character at
    // wherever the camera is standing.
    characterAt = centre;
    if (const char* atEnv = std::getenv("PORTJEUNO_CHARACTER_AT"))
    {
        std::sscanf(atEnv, "%f,%f,%f", &characterAt.x, &characterAt.y, &characterAt.z);
    }
    // The model faces +x in its own space, so a heading of zero looks east.
    float characterFacing = 0.0f;
    if (const char* facingEnv = std::getenv("PORTJEUNO_CHARACTER_FACING"))
    {
        characterFacing = static_cast<float>(std::atof(facingEnv)) * 3.14159265f / 180.0f;
    }
    auto placeCharacter = [&](const pj::Vec3& where) {
        characterAt = where;
        if (characterInstanceBuffer)
        {
            const float c = std::cos(characterFacing);
            const float sn = std::sin(characterFacing);
            const float instance[16] = {c,       0, -sn,      0, 0,       1, 0,       0,
                                        sn,      0, c,        0, where.x, where.y, where.z, 1};
            queue.WriteBuffer(characterInstanceBuffer, 0, instance, sizeof(instance));
        }
    };
    placeCharacter(characterAt);

    // Framing a shot from a script needs the camera to be settable; dragging
    // it into place by hand cannot be repeated.
    if (const char* cameraEnv = std::getenv("PORTJEUNO_CAMERA"))
    {
        std::sscanf(cameraEnv, "%f,%f,%f", &camera.position.x, &camera.position.y, &camera.position.z);
    }
    if (const char* lookEnv = std::getenv("PORTJEUNO_CAMERA_LOOK"))
    {
        float yawDegrees = 0.0f;
        float pitchDegrees = 0.0f;
        std::sscanf(lookEnv, "%f,%f", &yawDegrees, &pitchDegrees);
        camera.yaw = yawDegrees * 3.14159265f / 180.0f;
        camera.pitch = pitchDegrees * 3.14159265f / 180.0f;
    }

    bool dragging = false;

    std::printf("wasd to walk, mouse drag to look, space and ctrl for up and down,\n");
    std::printf("shift to move faster, tab to orbit, p to print position, c to place the character,\n");
    std::printf("escape to quit\n");

    // PORTJEUNO_SCREENSHOT writes one frame to a BMP and quits. Without it
    // there is no way to check what the renderer actually produced except by
    // looking at the window, which rules out checking anything unattended.
    const char* screenshotPath = std::getenv("PORTJEUNO_SCREENSHOT");
    int framesBeforeShot = 5; // let the first frames settle
    wgpu::Buffer readbackBuffer;

    // PORTJEUNO_ANIMATION names one of the character's own animations - idl0
    // for a standing idle, wlk0 to walk, run0 to run. Skinning runs on the CPU
    // and the vertices are rewritten each frame: a couple of thousand
    // triangles is nothing next to a zone, and it keeps the pose maths
    // somewhere it can be read.
    const ffxi::Animation* playing = nullptr;
    if (character && !character->animations.empty())
    {
        const char* wanted = std::getenv("PORTJEUNO_ANIMATION");
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

    // PORTJEUNO_FRAME pins the animation clock so a screenshot of a moving
    // character lands on the same pose every time.
    const char* frameEnv = std::getenv("PORTJEUNO_FRAME");
    const float pinnedFrame = frameEnv ? static_cast<float>(std::atof(frameEnv)) : -1.0f;

    const char* modeEnv = std::getenv("PORTJEUNO_SHADER_MODE");
    const float shaderMode = modeEnv ? static_cast<float>(std::atof(modeEnv)) : 0.0f;
    const char* cutoutEnv = std::getenv("PORTJEUNO_CUTOUT");
    const int cutoutMode = cutoutEnv ? std::atoi(cutoutEnv) : 2;

    // PORTJEUNO_TIME=1830 pins the clock; otherwise a Vana'diel day passes in
    // one real minute, which is fast but makes the whole cycle visible.
    const char* timeEnv = std::getenv("PORTJEUNO_TIME");
    const bool timeFixed = timeEnv != nullptr;
    const int fixedMinutes = timeFixed ? (std::atoi(timeEnv) / 100) * 60 + (std::atoi(timeEnv) % 100) : 0;
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
                    const pj::Vec3 at = camera.eye();
                    std::printf("at %.1f %.1f %.1f   zone y runs %.1f to %.1f\n", at.x, at.y, at.z,
                                zone->boundsMin.y, zone->boundsMax.y);
                }
                else if (event.key.key == SDLK_C && character)
                {
                    // Stand the character where the camera is. Nothing knows
                    // where the ground is yet, so putting it somewhere useful
                    // is a matter of walking there and pressing a key.
                    const pj::Vec3 at = camera.eye();
                    placeCharacter(at);
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
        camera.walk(ahead, side, lift);

        const float animationSeconds =
            pinnedFrame >= 0.0f && playing
                ? pinnedFrame * playing->frameSeconds()
                : static_cast<float>(SDL_GetTicksNS() / 1000000ull) / 1000.0f;

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
            const pj::Vec3 f = camera.orbiting ? pj::normalise(camera.lookAtPoint() - camera.eye()) : camera.forward();
            const pj::Vec3 r = pj::normalise(pj::cross(f, pj::Vec3{0.0f, 1.0f, 0.0f}));
            const pj::Vec3 u = pj::cross(r, f);

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
            const pj::Mat4 view = pj::lookAt(camera.eye(), camera.lookAtPoint(), pj::Vec3{0, 1, 0});
            // A near plane scaled to the zone puts everything nearby inside it
            // when standing on the ground, so it is fixed rather than relative.
            const pj::Mat4 projection =
                // A far plane 20x the zone radius wastes most of the depth
                // buffer's precision on space nothing occupies, which is what
                // makes coplanar layers fight in the first place.
                pj::perspective(1.05f, static_cast<float>(width) / static_cast<float>(height), 0.25f, radius * 4.0f);

            Uniforms uniforms{};
            const pj::Mat4 viewProjection = projection * view;
            std::memcpy(uniforms.viewProjection, viewProjection.m, sizeof(uniforms.viewProjection));
            const pj::Vec3 light = pj::normalise(pj::Vec3{0.4f, 0.8f, 0.45f});
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
            const pj::Vec3 eyePoint = camera.eye();
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
            // PORTJEUNO_SHADER_MODE: 0 draws colour with no alpha discard,
            // 2 draws alpha as greyscale, unset is normal rendering.
            uniforms.lightDirection[3] = shaderMode;
            queue.WriteBuffer(uniformBuffer, 0, &uniforms, sizeof(uniforms));

            pass.SetVertexBuffer(0, vertexBuffer);
            pass.SetVertexBuffer(1, instanceBuffer);
            pass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
            for (size_t i = 0; i < zone->draws.size() && i < batchBindGroups.size(); ++i)
            {
                const pj::InstancedDraw& draw = zone->draws[i];
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
                    pj::reskin(character->geometry, pj::animatedPose(character->skeleton, *playing, frame),
                               character->meshes);
                    queue.WriteBuffer(characterVertexBuffer, 0, character->geometry.vertices.data(),
                                      character->geometry.vertices.size() * sizeof(pj::Vertex));
                }
                pass.SetVertexBuffer(0, characterVertexBuffer);
                pass.SetVertexBuffer(1, characterInstanceBuffer);
                pass.SetIndexBuffer(characterIndexBuffer, wgpu::IndexFormat::Uint32);
                for (size_t i = 0; i < character->geometry.batches.size() && i < characterBindGroups.size(); ++i)
                {
                    const pj::Batch& batch = character->geometry.batches[i];
                    pass.SetPipeline(batch.cutout ? cutoutPipeline : pipeline);
                    pass.SetBindGroup(0, characterBindGroups[i]);
                    pass.DrawIndexed(batch.indexCount, 1, batch.indexOffset, 0, 0);
                }
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
        const bool takingShot = screenshotPath && --framesBeforeShot == 0;
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
                if (pixels && writeBmp(screenshotPath, pixels, width, height, bytesPerRow))
                {
                    std::printf("wrote %s (%ux%u)\n", screenshotPath, width, height);
                }
                else
                {
                    std::printf("could not write %s\n", screenshotPath);
                }
                readbackBuffer.Unmap();
            }
            else
            {
                std::printf("could not read the frame back\n");
            }
            break;
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
