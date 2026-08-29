# Bringing the renderer up on macOS

Instructions for a Claude session running on the Mac. Everything here was
written and verified on a Windows machine; the macOS paths have never been run.
Expect something to break, and read "What is likely to break" before assuming
the design is wrong.

## What this is

PortJeuno is a from-scratch Final Fantasy XI client. Its networking, chat,
movement and radar are C# and already run on macOS. What does not is 3D
rendering.

The 3D side used to go through `lotus-engine`, a Vulkan renderer that requires
ray tracing. MoltenVK does not implement `VK_KHR_ray_tracing_pipeline`,
`VK_KHR_ray_query` or `VK_KHR_acceleration_structure` - measured on this very
Mac, 167 `VK_KHR_*` extensions present and none of those three - so that engine
cannot render here at all. lotus's two non-raytraced modes are both dead
upstream, so there was nothing to fall back to.

The replacement is a WebGPU renderer using Dawn, in C++. WebGPU picks the native
API per platform: **Metal on macOS**, D3D12 on Windows, Vulkan on Linux. No
MoltenVK anywhere.

Background, with the evidence: `docs/renderer-webgpu.md`.

## The goal

`renderer/` is a vertical slice - a window, a WebGPU device, and an animated
clear colour. No FFXI data yet, on purpose. It exists to answer one question
before any renderer gets written against it:

**does this come up on Metal?**

The program prints its adapter and backend on the first line of stdout. On
Windows it prints:

```
adapter: NVIDIA GeForce RTX 4060 (D3D12)
```

Here it must name the Apple GPU and say **Metal**. A window slowly cycling
through colours means the device, swapchain and render pass all work.

## Do not "fix" it by falling back to Vulkan

If Metal fails, do not make it work through MoltenVK and report success. Vulkan
on this machine is the problem being solved, not an acceptable answer. A Metal
failure is a real result and worth reporting as one.

## Steps

```bash
brew install cmake ninja sdl3
xcode-select --install   # if the command line tools are not already there
```

```bash
cd PortJeuno && git pull
```

Build Dawn. This is a large build - it fetches its own dependencies and compiles
Tint along with it. Expect a long wait, and it only has to happen once.

```bash
./build-dawn.sh
```

Then the slice:

```bash
./build-renderer.sh
./build-renderer/portjeuno-renderer
```

Escape or closing the window quits.

## What is likely to break

In rough order of probability.

**`renderer/surface_metal.mm`.** The one genuinely platform-specific file, and
the only one with no Windows equivalent to have shaken it out. It reads the
`NSWindow` out of SDL, attaches a `CAMetalLayer` to its content view, and hands
that layer to Dawn. Two API shapes it depends on were checked against the
headers and are correct as of this Dawn checkout: `SDL_PROP_WINDOW_COCOA_WINDOW_POINTER`
is the `NSWindow`, and `wgpu::SurfaceSourceMetalLayer` has a single `void* layer`
member. What was *not* checkable from Windows is whether it compiles and whether
the layer behaves. It is compiled with `-fobjc-arc` (set in
`renderer/CMakeLists.txt`) because it uses `__bridge` casts, which do not exist
without ARC.

If the window renders at half resolution, look at the `setContentsScale:` line -
that is there to follow the display's backing scale factor and is untested.

**SDL3 discovery.** `find_package(SDL3)` may not look in the Homebrew prefix. If
it fails, pass the prefix explicitly:

```bash
cmake -S renderer -B build-renderer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DDawn_DIR="$PWD/vendor/dawn-install/lib/cmake/Dawn" \
  -DCMAKE_PREFIX_PATH=/opt/homebrew
```

**The build scripts themselves.** `build-dawn.sh` and `build-renderer.sh` were
written blind. They are short; read them rather than trusting them.

## Already solved - do not rediscover these

Three problems were hit on Windows and fixed. All three report themselves
misleadingly, so they are worth recognising rather than debugging from scratch.

**`find_package(Threads REQUIRED)` must come before `find_package(Dawn)`.**
Dawn's exported target lists `Threads::Threads` in its link interface but its
config never looks for it, so the find fails with a confusing "target not found".
Already handled in `renderer/CMakeLists.txt`.

**`WaitAny` with a non-zero timeout needs the `TimedWaitAny` instance feature.**
Without it, every wait fails with "Timeout waits are either not enabled or not
supported", which reads like a driver limitation and is not one. Already handled
in `renderer/main.cpp`.

**`DAWN_FORCE_SYSTEM_COMPONENT_LOAD=ON`** - a Windows-only problem, mentioned so
its absence here is not mistaken for an oversight. On Windows, Dawn's
`DynamicLib::Open` passes `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` with a bare
filename, which that flag forbids, so loading `d3dcompiler_47.dll` fails with
error 87. `build-dawn.sh` does not set it because the macOS path does not go
through that code.

## Reporting back

What is useful to send back to the Windows session:

- the first stdout line - the adapter and backend
- whether the window appears and whether the colours animate
- for a build failure, the first error and the file it came from, not the whole log
- for `surface_metal.mm` specifically, the exact compiler diagnostic

## What comes next, for context

Step 3a is extracting the FFXI DAT parsers into a library with no lotus and no
Vulkan dependency, so the renderer can reach them on a platform where Vulkan is
not an option. Ten of the fourteen parsers are already free of Vulkan; the four
that are not (`mmb`, `d3m`, `dxt3`, `mzb`) mix parsing with GPU upload and split
along that seam. Then `MZB`'s `CollisionMeshData` - plain vertices, normals and
indices, no GPU types - becomes the first real FFXI geometry rendered through
Dawn.

None of that is blocked on this bring-up, but all of it is wasted if the answer
to "does it come up on Metal" turns out to be no.
