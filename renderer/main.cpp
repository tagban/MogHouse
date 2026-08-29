// Step 1 and 2 of the vertical slice in docs/renderer-webgpu.md: bring up Dawn
// on a real window and clear the screen. No FFXI data yet - the point is to
// prove the same source builds and runs on Windows, macOS and Linux before any
// of the renderer is written against it.

#include "surface.h"

#include <SDL3/SDL.h>
#include <webgpu/webgpu_cpp.h>

#include <cstdio>
#include <iterator>
#include <string>

namespace
{
constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;

const char* BackendName(wgpu::BackendType backend)
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
} // namespace

int main(int, char**)
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::printf("SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("PortJeuno renderer", kWidth, kHeight, SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        std::printf("SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }

    // Waiting on a future with a non-zero timeout is an opt-in feature. Without
    // it every WaitAny below fails with "Timeout waits are either not enabled
    // or not supported", which reads like a driver problem and is not one.
    static constexpr wgpu::InstanceFeatureName kInstanceFeatures[] = {wgpu::InstanceFeatureName::TimedWaitAny};

    wgpu::InstanceDescriptor instance_descriptor{.requiredFeatureCount = std::size(kInstanceFeatures), .requiredFeatures = kInstanceFeatures};
    wgpu::Instance instance = wgpu::CreateInstance(&instance_descriptor);
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

    // Adapter and device requests are asynchronous. Dawn will run the callbacks
    // on this thread when we wait on the future, so there is no threading here.
    wgpu::Adapter adapter;
    wgpu::RequestAdapterOptions adapter_options{.compatibleSurface = surface};
    instance.WaitAny(instance.RequestAdapter(&adapter_options, wgpu::CallbackMode::WaitAnyOnly,
                                             [&](wgpu::RequestAdapterStatus status, wgpu::Adapter result, wgpu::StringView message)
                                             {
                                                 if (status != wgpu::RequestAdapterStatus::Success)
                                                 {
                                                     std::printf("could not get an adapter: %.*s\n", static_cast<int>(message.length), message.data);
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
    std::printf("adapter: %.*s (%s)\n", static_cast<int>(info.device.length), info.device.data, BackendName(info.backendType));

    wgpu::DeviceDescriptor device_descriptor{};
    device_descriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message)
                                                 { std::printf("webgpu error: %.*s\n", static_cast<int>(message.length), message.data); });

    wgpu::Device device;
    instance.WaitAny(adapter.RequestDevice(&device_descriptor, wgpu::CallbackMode::WaitAnyOnly,
                                           [&](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView message)
                                           {
                                               if (status != wgpu::RequestDeviceStatus::Success)
                                               {
                                                   std::printf("could not get a device: %.*s\n", static_cast<int>(message.length), message.data);
                                                   return;
                                               }
                                               device = std::move(result);
                                           }),
                     UINT64_MAX);
    if (!device)
    {
        return 1;
    }

    wgpu::SurfaceCapabilities capabilities{};
    surface.GetCapabilities(adapter, &capabilities);
    wgpu::TextureFormat format = capabilities.formats[0];

    auto configure = [&](uint32_t width, uint32_t height)
    {
        wgpu::SurfaceConfiguration configuration{
            .device = device, .format = format, .usage = wgpu::TextureUsage::RenderAttachment, .width = width, .height = height, .presentMode = wgpu::PresentMode::Fifo};
        surface.Configure(&configuration);
    };
    configure(kWidth, kHeight);

    wgpu::Queue queue = device.GetQueue();

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
            else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
            {
                configure(static_cast<uint32_t>(event.window.data1), static_cast<uint32_t>(event.window.data2));
            }
        }

        wgpu::SurfaceTexture surface_texture;
        surface.GetCurrentTexture(&surface_texture);
        if (!surface_texture.texture)
        {
            continue;
        }

        wgpu::RenderPassColorAttachment colour{.view = surface_texture.texture.CreateView(),
                                               .loadOp = wgpu::LoadOp::Clear,
                                               .storeOp = wgpu::StoreOp::Store,
                                               .clearValue = {0.06, 0.09, 0.11, 1.0}};
        wgpu::RenderPassDescriptor pass_descriptor{.colorAttachmentCount = 1, .colorAttachments = &colour};

        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_descriptor);
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
