// PortJeuno's renderer. Opens a window on WebGPU - Metal on macOS, D3D12 on
// Windows, Vulkan on Linux - and draws FFXI zone geometry read from the retail
// DATs.
//
// Given a DAT it draws that zone's collision geometry. Given nothing it only
// clears, so the graphics path can still be checked on a machine with no game
// installed.

#include "ffxi/dat.h"
#include "ffxi/mmb.h"
#include "ffxi/mzb.h"
#include "math.h"
#include "surface.h"
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
std::optional<pj::ZoneMesh> loadZone(const char* datPath, const char* keyPath, const char* key2Path, std::string& zoneId)
{
    auto keys = ffxi::KeyTable::load(keyPath);
    if (!keys)
    {
        std::printf("could not read a 256-byte key table from %s\n", keyPath);
        return std::nullopt;
    }

    ffxi::DatFile dat{std::filesystem::path{datPath}};

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

    std::optional<pj::ZoneMesh> best;
    for (const ffxi::Chunk& chunk : dat.chunksOfType(ffxi::kChunkMzb))
    {
        ffxi::Zone zone = ffxi::parseMzb(chunk, *keys);

        // Placed models are the visible world; collision geometry is the
        // fallback when the model key table is not available.
        pj::ZoneMesh mesh;
        if (!models.empty())
        {
            size_t resolved = 0;
            size_t missing = 0;
            mesh = pj::buildPlacedMesh(zone, models, resolved, missing);
            if (!mesh.vertices.empty())
            {
                std::printf("  %zu models (%zu unreadable), %zu placements drawn, %zu with no model\n", models.size(),
                            modelsFailed, resolved, missing);
            }
        }
        if (mesh.vertices.empty())
        {
            mesh = pj::buildZoneMesh(zone);
        }

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
    std::optional<pj::ZoneMesh> zone;
    if (argc > 1)
    {
        const char* keyPath = std::getenv("PORTJEUNO_FFXI_KEYTABLE");
        if (!keyPath)
        {
            std::printf("set PORTJEUNO_FFXI_KEYTABLE to the 256-byte MZB key table to load a zone\n");
            return 2;
        }
        zone = loadZone(argv[1], keyPath, std::getenv("PORTJEUNO_FFXI_KEYTABLE2"), zoneId);
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

    wgpu::DeviceDescriptor deviceDescriptor{};
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
    wgpu::Buffer uniformBuffer;
    wgpu::BindGroup bindGroup;
    wgpu::RenderPipeline pipeline;
    uint32_t indexCount = 0;

    if (zone && !zone->indices.empty())
    {
        vertexBuffer = createBuffer(device, zone->vertices.data(), zone->vertices.size() * sizeof(pj::Vertex),
                                    wgpu::BufferUsage::Vertex);
        indexBuffer = createBuffer(device, zone->indices.data(), zone->indices.size() * sizeof(uint32_t),
                                   wgpu::BufferUsage::Index);
        indexCount = static_cast<uint32_t>(zone->indices.size());
        std::printf("buffers created\n");

        wgpu::BufferDescriptor uniformDescriptor{.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                                                 .size = sizeof(Uniforms)};
        uniformBuffer = device.CreateBuffer(&uniformDescriptor);

        wgpu::ShaderSourceWGSL wgsl;
        wgsl.code = pj::kZoneShader;
        wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl};
        wgpu::ShaderModule module = device.CreateShaderModule(&moduleDescriptor);

        wgpu::VertexAttribute attributes[2] = {
            {.format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0},
            {.format = wgpu::VertexFormat::Float32x3, .offset = 3 * sizeof(float), .shaderLocation = 1}};
        wgpu::VertexBufferLayout vertexLayout{.stepMode = wgpu::VertexStepMode::Vertex,
                                              .arrayStride = sizeof(pj::Vertex),
                                              .attributeCount = 2,
                                              .attributes = attributes};

        wgpu::ColorTargetState colorTarget{.format = surfaceFormat};
        wgpu::FragmentState fragment{.module = module, .entryPoint = "fragmentMain", .targetCount = 1, .targets = &colorTarget};
        wgpu::DepthStencilState depthStencil{.format = kDepthFormat,
                                             .depthWriteEnabled = wgpu::OptionalBool::True,
                                             .depthCompare = wgpu::CompareFunction::Less};

        wgpu::RenderPipelineDescriptor pipelineDescriptor{
            .vertex = {.module = module, .entryPoint = "vertexMain", .bufferCount = 1, .buffers = &vertexLayout},
            .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
            .depthStencil = &depthStencil,
            .fragment = &fragment};
        pipeline = device.CreateRenderPipeline(&pipelineDescriptor);

        wgpu::BindGroupEntry entry{.binding = 0, .buffer = uniformBuffer, .size = sizeof(Uniforms)};
        wgpu::BindGroupDescriptor bindGroupDescriptor{.layout = pipeline.GetBindGroupLayout(0), .entryCount = 1, .entries = &entry};
        bindGroup = device.CreateBindGroup(&bindGroupDescriptor);
    }

    const pj::Vec3 centre = zone ? zone->centre() : pj::Vec3{};
    const float radius = zone ? std::max(zone->radius(), 1.0f) : 1.0f;

    float orbit = 0.0f;
    float pitch = 0.45f;
    float distance = radius * 2.4f;
    bool dragging = false;
    bool spinning = true;

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
                else if (event.key.key == SDLK_SPACE)
                {
                    spinning = !spinning;
                }
            }
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                configure(static_cast<uint32_t>(event.window.data1), static_cast<uint32_t>(event.window.data2));
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN)
            {
                dragging = true;
                spinning = false;
            }
            else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            {
                dragging = false;
            }
            else if (event.type == SDL_EVENT_MOUSE_MOTION && dragging)
            {
                orbit -= event.motion.xrel * 0.008f;
                pitch = std::clamp(pitch + event.motion.yrel * 0.008f, -1.4f, 1.4f);
            }
            else if (event.type == SDL_EVENT_MOUSE_WHEEL)
            {
                distance = std::clamp(distance * (event.wheel.y > 0 ? 0.9f : 1.1f), radius * 0.05f, radius * 12.0f);
            }
        }

        const uint64_t nowTicks = SDL_GetTicksNS();
        const float delta = static_cast<float>(nowTicks - previousTicks) / 1e9f;
        previousTicks = nowTicks;
        if (spinning)
        {
            orbit += delta * 0.25f;
        }

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
                                                  .depthStencilAttachment = indexCount ? &depth : nullptr};

        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&passDescriptor);

        if (indexCount)
        {
            const pj::Vec3 eye{centre.x + distance * std::cos(pitch) * std::sin(orbit),
                               centre.y + distance * std::sin(pitch),
                               centre.z + distance * std::cos(pitch) * std::cos(orbit)};
            const pj::Mat4 view = pj::lookAt(eye, centre, pj::Vec3{0, 1, 0});
            const pj::Mat4 projection = pj::perspective(1.05f, static_cast<float>(width) / static_cast<float>(height),
                                                        radius * 0.01f, radius * 20.0f);

            Uniforms uniforms{};
            const pj::Mat4 viewProjection = projection * view;
            std::memcpy(uniforms.viewProjection, viewProjection.m, sizeof(uniforms.viewProjection));
            const pj::Vec3 light = pj::normalise(pj::Vec3{0.4f, 0.8f, 0.45f});
            uniforms.lightDirection[0] = light.x;
            uniforms.lightDirection[1] = light.y;
            uniforms.lightDirection[2] = light.z;
            queue.WriteBuffer(uniformBuffer, 0, &uniforms, sizeof(uniforms));

            pass.SetPipeline(pipeline);
            pass.SetBindGroup(0, bindGroup);
            pass.SetVertexBuffer(0, vertexBuffer);
            pass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint32);
            pass.DrawIndexed(indexCount);
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
