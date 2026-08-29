# Replacing the renderer

## Why

macOS is a requirement, and lotus cannot render there. Measured on the user's
M4 Mac mini with MoltenVK 1.4.2, which exposes 167 `VK_KHR_*` extensions and
none of these:

```
vulkaninfo | grep -iE "ray_tracing|ray_query|acceleration_structure"   # empty
```

lotus has three render modes and only one of them is real:

| Mode | State |
| --- | --- |
| Raytrace | Works. Full colour. The only maintained path. |
| Hybrid | Renders geometry, no lighting - `raytrace_hybrid.slang`'s `Raygen()` is an empty function, its body commented out since `e69ea94` (2025-01-25). |
| Rasterization | Does not start. Loads `shaders/deferred.spv` and `deferred_raster.spv`, whose GLSL sources were deleted 2020-05-14 and never ported to slang. Upstream `551ae3b` is titled "Started removing pure raster renderer". |

So the only working path needs exactly the extensions macOS does not have, and
the two that do not need them are a stub and a corpse respectively. Reviving
either would mean writing deferred lighting from scratch *and* maintaining a
renderer upstream is actively deleting.

## What instead

A WebGPU renderer, using Dawn, **in C++**. Native Metal on macOS with no
MoltenVK in the path, D3D12 on Windows, Vulkan on Linux.

Dawn rather than Rust `wgpu` specifically so this stays in C++ and keeps
everything above the renderer: the DAT parsing, skeletal animation, entity and
scene systems in lotus-ffxi are the years of work worth preserving, and an FFI
boundary between them and the renderer would be pure cost.

## What we already know

**Slang targets WGSL directly** (`slangc -target wgsl`, alongside `metal` and
`metallib`). `ffxi/shaders/mmb.slang` compiles to 203 lines of WGSL with no
source changes, module imports and all.

**But the resource model does not survive.** That generated WGSL contains:

```wgsl
@align(16) vertex_buffer_0 : u64,                              // no 64-bit ints in WGSL
@binding(1) var textures_texture_0 : array<texture_2d<f32>>;   // unbounded = bindless
```

Those are `VK_KHR_buffer_device_address` and bindless descriptor arrays, and
WebGPU has neither by design. The shading maths translates; the binding
architecture has to be redesigned around bind groups and indices.

That is the right outcome anyway. lotus is bindless-with-raw-pointers because a
raytracer needs the whole scene addressable at once. Nothing here does. FFXI's
retail client was a 2002 rasterizer - directional light, ambient, vertex colour,
fog - so a conventional forward or lightly-deferred renderer is both less work
and closer to how the game actually looked. lotus's raytraced PBR is a
reimagining, not fidelity.

## Quality

Raytracing is not a goal; looking good is. Those are separable, and a rasterizer
has plenty of headroom before anyone misses ray tracing - anisotropic filtering,
real mipmaps, MSAA, and a decent tonemap already put us well past what the
retail client did in 2002.

The constraint that has to be designed in from the start, rather than retrofitted:
**texture sources are pluggable and no resolution is ever assumed.** Community
upscale packs replace the DAT textures at much higher resolution, and they are
in scope once the DAT path renders cross-platform. Nothing in the renderer may
hardcode DXT3, DAT-native dimensions, or a 1:1 mapping from DAT entry to GPU
texture. Loading from the DATs is the default, not the only path.

Order of work is deliberate: get the DAT path rendering on all three platforms
first, then raise quality. Quality work on a renderer that only runs on one
platform would have to be redone.

## Plan

A vertical slice first, to fail in days rather than months:

1. Dawn building through CMake on Windows and macOS
2. Slang -> WGSL on one existing FFXI shader (**done** - with the caveat above)
3. Real MZB zone geometry, parsed by `ffxi-lib`, rendered through Dawn
4. The same verified on the M4

Step 3 splits, and the first half is much cheaper than expected. `MZB` parses
zone collision geometry into `CollisionMeshData`:

```cpp
struct CollisionMeshData
{
    std::vector<uint8_t> vertices;
    std::vector<uint8_t> normals;
    std::vector<uint16_t> indices;
};
```

Plain CPU data, no Vulkan in it at all. So:

- **3a** - render those meshes untextured. Proves DAT -> parse -> Dawn -> screen
  on all three platforms without touching materials, textures, or any of the
  four Vulkan-entangled parsers.
- **3b** - render MMB models with their DXT3 textures. This is where the binding
  redesign actually has to happen, and it is worth doing only once 3a has shown
  the foundation works everywhere.

`ffxi-lib` is already linkable from outside the engine's own executable, and a
C ABI round trip from C# through it into the retail DATs is verified - see
`docs/engine-build.md`.

## The dependency problem step 3a runs into

To render MZB geometry the renderer needs the MZB parser, which lives in
`ffxi-lib`, which links `lotus-engine`, which is Vulkan. Pulling the whole
Vulkan engine into a WebGPU renderer to reach a parser is obviously wrong, and
on macOS it is worse than wrong - the engine cannot initialise there at all.

So the parsers have to come out into a library of their own with no lotus and
no Vulkan in it. The measurement above says that is mostly a matter of moving
files: `dat`, `dat_loader`, `key_tables`, `sk2`, `mo2`, `scheduler`, `generator`,
`sep`, `cib` and `os2` are already clean. Only `mmb`, `d3m`, `dxt3` and `mzb`
need splitting, and the split is along one seam - parse into plain CPU data on
one side, upload to the GPU on the other.

This is the real architectural work, and it is worth doing properly rather than
shortcutting for the slice: it is also what makes shipping on macOS possible at
all, since nothing Vulkan-dependent can be on that path.

## Not decided yet

Whether the new renderer consumes lotus's entity/scene system or sits directly
on lotus-ffxi's parsed asset data. That depends on how entangled the per-frame
render path is with the Vulkan types, which the slice will show.

## DAWN_FORCE_SYSTEM_COMPONENT_LOAD is required on Windows

Without it, Dawn cannot load `d3dcompiler_47.dll` or `vulkan-1.dll` and no
device can be created at all - both fail with `Windows Error: 87`.

`DynamicLib::Open` in `src/dawn/common/DynamicLib.cpp` does:

```cpp
const DWORD loadLibraryFlags =
    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS;
mHandle = LoadLibraryExA(filename.c_str(), nullptr, loadLibraryFlags);
```

`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` requires a fully qualified path, and these
are bare filenames, so Windows returns `ERROR_INVALID_PARAMETER` (87). Building
with `DAWN_FORCE_SYSTEM_COMPONENT_LOAD=ON` compiles the other branch of that
`#if`, which uses `LOAD_LIBRARY_SEARCH_SYSTEM32` and works.

The error message points at the DLL, so it reads like a missing or broken system
library. Both files are present in System32 and load fine for other programs.
