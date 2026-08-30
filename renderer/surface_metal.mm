// macOS: SDL gives us an NSWindow, but WebGPU wants a CAMetalLayer, so we have
// to attach one to the window's content view ourselves. That needs Objective-C,
// which is why this file exists and its siblings are plain C++.
//
// UNTESTED - written on a Windows machine. This is the first thing to check if
// the mac build fails.

#include "surface.h"

#include <SDL3/SDL.h>

#import <QuartzCore/CAMetalLayer.h>
#import <Cocoa/Cocoa.h>

namespace mh
{
wgpu::Surface CreateSurface(const wgpu::Instance& instance, SDL_Window* window)
{
    NSWindow* ns_window = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!ns_window)
    {
        return nullptr;
    }

    NSView* view = [ns_window contentView];
    [view setWantsLayer:YES];

    CAMetalLayer* layer = [CAMetalLayer layer];
    // Without this the layer does not follow the window's backing scale factor
    // and everything renders at half resolution on a retina display.
    [layer setContentsScale:[ns_window backingScaleFactor]];
    [view setLayer:layer];

    wgpu::SurfaceSourceMetalLayer source;
    source.layer = (__bridge void*)layer;

    wgpu::SurfaceDescriptor descriptor{.nextInChain = &source};
    return instance.CreateSurface(&descriptor);
}
} // namespace mh
