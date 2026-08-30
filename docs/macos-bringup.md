# Bringing the renderer up on macOS

Instructions for a Claude session running on the Mac. Everything below the
status block was written and verified on a Windows machine before the macOS
paths had ever been run.

> **STATUS: done, 2026-08-29. It comes up on Metal.**
>
> ```
> adapter: Apple M4 (Metal)
> ```
>
> Window renders, clear colour animates. Confirmed on an M4 Mac mini,
> macOS 26.5.2, from a clean clone. **The predictions below were wrong in a
> useful way** - read "What actually happened" at the bottom before following
> the instructions in the middle, because three of the steps as written do not
> work on a fresh clone and have since been fixed.

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

---

# What actually happened

Written by the macOS session on 2026-08-29, for whoever reads this next.

## The answer

`adapter: Apple M4 (Metal)`. Window appears, clear colour animates - verified
with two screen captures four seconds apart, purple then green, so this is a
live swapchain and not a window that never rendered. No MoltenVK involved.
Step 3a's premise holds.

## The predictions in this doc were wrong, in a useful way

Everything the doc expected to break was fine. Everything that actually broke
was packaging, not code.

**`surface_metal.mm` compiled clean** - no errors, no warnings. Both API shapes
it depends on were correct: `SDL_PROP_WINDOW_COCOA_WINDOW_POINTER` gives the
`NSWindow`, `wgpu::SurfaceSourceMetalLayer` took the single `void* layer`.
`-fobjc-arc` and the `__bridge` casts were right.

One thing worth recording because it looks like a bug and is not: the file
calls `setWantsLayer:YES` *before* `setLayer:`. AppKit documents the opposite
order for layer-hosting views, so this was flagged during review as a probable
cause of a blank window. **It does not manifest** - AppKit honours the later
assignment and the layer renders. Do not "fix" it based on the documentation
alone; it works as written and was tested.

**SDL3 discovery was a non-issue.** `find_package(SDL3)` found the Homebrew
install with no `CMAKE_PREFIX_PATH`. The documented fallback was never needed.

**Both Windows-era fixes carried over.** `find_package(Threads)` before `Dawn`
resolved cleanly, and `TimedWaitAny` meant no `WaitAny` ever produced the
misleading timeout error.

**The Dawn build is much cheaper than this doc suggests.** "Expect a long wait"
overstates it: 3m41s at 745% CPU, 1.45 GB total including the checkout and the
install prefix. CMake 4.4.3 cleared Dawn's configure without the
`cmake_minimum_required` incompatibility that version causes in older trees.

## What actually broke - all three will hit Linux identically

**1. `build-renderer/` was committed.** 33 files of generated MSVC output -
`CMakeCache.txt`, object files, `SDL3.dll`, `portjeuno-renderer.exe` - sitting
at exactly the path both build scripts configure into. Any non-Windows clone
hit the stale cache. The cache hardcoded `C:/Users/Gaming/Desktop/PortJeuno/`,
so it was already useless on any other Windows checkout too.

Removed from tracking and from disk. `.gitignore` now carries
`/build-renderer`. Every platform can use `build-renderer/` again and both
scripts run verbatim.

**2. The `.sh` scripts had no executable bit.** Committed from Windows, so
`./build-dawn.sh` failed on a fresh clone with `permission denied` (exit 126) -
the instructions in this doc could not run as written. Both are now mode
`100755`; Windows checkouts ignore the bit harmlessly.

**3. The `.gitignore` Dawn rules were directory-only.** `/vendor/dawn/`,
`build-dawn/` and `/vendor/dawn-install/` all ended in a slash, which matches
directories but not symlinks, so a build tree redirected to another volume
showed up as untracked. Trailing slashes dropped. This matters on Windows too -
a junction hits the same rule.

## One real bug, fixed: points vs pixels

`main.cpp` configured the surface with `kWidth/kHeight`, the values handed to
`SDL_CreateWindow`, which are **points**. `surface_metal.mm` sets the layer's
`contentsScale` from `backingScaleFactor`, so the backing store is 2x that on a
retina display. Dawn's Metal backend drives `drawableSize` from the surface
configuration, so the slice was rendering 1280x720 upscaled onto 2560x1440.

**A flat clear colour looks identical at any resolution, which is why the
vertical slice cannot detect this.** It would first appear at step 3a as
unexplained blurriness once real geometry is on screen. Note that this doc
points at the `setContentsScale:` line as the half-resolution suspect - that
line is doing its job; the mismatch was on the configure side.

Fixed by opting the window into `SDL_WINDOW_HIGH_PIXEL_DENSITY` and seeding the
first configure from `SDL_GetWindowSizeInPixels()`. This makes the initial path
agree with the resize path, which was already pixel-correct because
`SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` reports pixels. Portable by construction:
the same split exists on Windows under display scaling.

## The C# side also runs here

Not asked for by this doc, but tested while the toolchain was set up:

- `dotnet build PortJeuno.slnx -c Release` - **0 errors**, 2.9s, one trivial
  `CS9191` warning in `Ffxi/FfxiNavMesh.cs:99`.
- `dotnet test` - **100 passed, 0 failed, 12 skipped**, 33 ms.

The 12 skips are gated, not broken: they need `PORTJEUNO_FFXI_RES` pointing at
a directory holding `compress.dat` and `decompress.dat`.

**A retail install does not satisfy this.** A full one was staged on this
machine and searched - neither file exists anywhere in it. Per the docs on
`FfxiHuffmanTables`, they are dumps of in-memory tables distributed GPLv3 in
the LandSandBoat repo, whose contents likely originate in the retail client.
Sourcing them is a licensing decision the code deliberately leaves to whoever
ships a build, so it was not made here. Expect these 12 to stay skipped until
someone provides those two files on purpose.

The retail install itself is at:

```
/Volumes/AppStorage/FFXI Game Folder/PlayOnline/SquareEnix/FINAL FANTASY XI/
```

Note the `SquareEnix` level - the obvious guess of `PlayOnline/FINAL FANTASY XI`
is wrong - and the spaces, which need quoting everywhere. It is a complete
install: `ROM` through `ROM9`, `SYS`, `Tools`. That is what step 3a's DAT
parsers actually need; the compression tables are a separate problem.

Two things noticed in passing, neither acted on:

- **`PortJeuno.App` is not in `PortJeuno.slnx`.** Only Console, Core and
  Core.Tests are listed, so a solution-level build skips the app. It targets
  plain `net10.0` and uses Avalonia 12.1.1, which is cross-platform - its
  `WinExe` output type only suppresses a console window. It is a strong
  candidate to run on macOS and has not been tried.
- **`native/portjeuno_interop` has still never been compiled.** Its README says
  the authoring machine had no CMake or Ninja. This machine now has both, so
  that claim can finally be tested. `Core/Interop/NativeEngine.cs` declares
  `LibraryImport` against it, but no `.csproj` references it, so the .NET build
  does not produce it.

## Environment notes for the next session on this Mac

Two things were installed but invisible, and cost time before being spotted:

- **Homebrew** was a half-finished install: `/opt/homebrew` had the directory
  skeleton but no `Homebrew/` repo and an empty `bin/`. `/opt/homebrew` is
  user-writable, so it was completed with the official untar method - no sudo,
  no password.
- **The .NET 10 SDK was already present** at `~/.dotnet` (10.0.400), just not
  on `PATH`.

Neither is on `PATH` in a non-login shell, because `~/.zprofile` is only
sourced for login shells. Prefix commands accordingly:

```bash
export PATH="/opt/homebrew/bin:$HOME/.dotnet:$PATH"
```

The heavy Dawn paths are redirected to an external APFS volume, because the
internal disk had only 14.8 GB free:

```
vendor/dawn          -> /Volumes/AppStorage/PortJeuno-build/dawn
build-dawn           -> /Volumes/AppStorage/PortJeuno-build/build-dawn
vendor/dawn-install  -> /Volumes/AppStorage/PortJeuno-build/dawn-install
```

These are symlinks, so the build scripts still work unmodified. If that volume
is unplugged they dangle and Dawn must be rebuilt or re-linked.

## Versions this was verified against

| | |
|---|---|
| Host | Apple M4, arm64, macOS 26.5.2 |
| Compiler | AppleClang 16.0.0 |
| CMake | 4.4.3 |
| Ninja | 1.13.2 |
| SDL3 | 3.4.14 |
| Dawn | `053ad3188` |
| .NET SDK | 10.0.400 |
