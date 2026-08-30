#pragma once

#include <webgpu/webgpu_cpp.h>

struct SDL_Window;

namespace mh
{
// Creating a WebGPU surface from an SDL window is the one genuinely
// platform-specific piece of this renderer, because each windowing system hands
// over a different kind of handle - an HWND, a CAMetalLayer, an X11 window id.
// It is isolated here so the rest of the renderer never has to care.
//
// Returns a null surface if the platform is not supported or the handle could
// not be read; the caller reports the error.
wgpu::Surface CreateSurface(const wgpu::Instance& instance, SDL_Window* window);
} // namespace mh
