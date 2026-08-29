// Windows: SDL hands over an HWND directly.

#include "surface.h"

#include <SDL3/SDL.h>

namespace pj
{
wgpu::Surface CreateSurface(const wgpu::Instance& instance, SDL_Window* window)
{
    void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    if (!hwnd)
    {
        return nullptr;
    }

    wgpu::SurfaceSourceWindowsHWND source;
    source.hinstance = GetModuleHandle(nullptr);
    source.hwnd = hwnd;

    wgpu::SurfaceDescriptor descriptor{.nextInChain = &source};
    return instance.CreateSurface(&descriptor);
}
} // namespace pj
