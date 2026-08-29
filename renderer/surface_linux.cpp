// Linux: either X11 or Wayland, decided at runtime by whichever SDL actually
// used - a build that supports both still has to pick per session.
//
// UNTESTED - written on a Windows machine.

#include "surface.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>

namespace pj
{
wgpu::Surface CreateSurface(const wgpu::Instance& instance, SDL_Window* window)
{
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    const char* driver = SDL_GetCurrentVideoDriver();

    if (driver && std::strcmp(driver, "wayland") == 0)
    {
        void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
        void* wl_surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
        if (!display || !wl_surface)
        {
            return nullptr;
        }

        wgpu::SurfaceSourceWaylandSurface source;
        source.display = display;
        source.surface = wl_surface;

        wgpu::SurfaceDescriptor descriptor{.nextInChain = &source};
        return instance.CreateSurface(&descriptor);
    }

    void* display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    auto x11_window = static_cast<uint64_t>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
    if (!display || x11_window == 0)
    {
        return nullptr;
    }

    wgpu::SurfaceSourceXlibWindow source;
    source.display = display;
    source.window = x11_window;

    wgpu::SurfaceDescriptor descriptor{.nextInChain = &source};
    return instance.CreateSurface(&descriptor);
}
} // namespace pj
