// Windows: SDL hands over an HWND directly.

#include "surface.h"

#include <SDL3/SDL.h>

// For GetModuleHandle. WIN32_LEAN_AND_MEAN keeps windows.h from dragging in
// winsock and friends, which collide with plenty of things.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace mh
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
} // namespace mh
