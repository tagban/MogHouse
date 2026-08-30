// PortJeuno's renderer. Opens a window on WebGPU - Metal on macOS, D3D12 on
// Windows, Vulkan on Linux - and draws FFXI zone geometry read from the retail
// DATs.
//
// Given a DAT it draws that zone's collision geometry. Given nothing it only
// clears, so the graphics path can still be checked on a machine with no game
// installed.

#include "ffxi/dat.h"
#include "ffxi/mmb.h"
#include "ffxi/lighting.h"
#include "ffxi/mzb.h"
#include "ffxi/texture.h"
#include "gputexture.h"
#include "camera.h"
#include "coverage.h"
#include "math.h"
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
                                                 .usage = wgpu::TextureUsage::RenderAttachment,
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

            wgpu::BindGroupLayoutEntry waterLayoutEntry{};
            waterLayoutEntry.binding = 0;
            waterLayoutEntry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
            waterLayoutEntry.buffer.type = wgpu::BufferBindingType::Uniform;
            wgpu::BindGroupLayoutDescriptor waterBglDescriptor{.entryCount = 1, .entries = &waterLayoutEntry};
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

            wgpu::BindGroupEntry waterEntry{.binding = 0, .buffer = uniformBuffer, .size = sizeof(Uniforms)};
            wgpu::BindGroupDescriptor waterBgDescriptor{.layout = waterBgl, .entryCount = 1, .entries = &waterEntry};
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
    bool dragging = false;

    std::printf("wasd to walk, mouse drag to look, space and ctrl for up and down,\n");
    std::printf("shift to move faster, tab to orbit, p to print position, escape to quit\n");

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
        wgpu::CommandBuffer commands = encoder.Finish();
        queue.Submit(1, &commands);
        surface.Present();
        instance.ProcessEvents();
    }

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
