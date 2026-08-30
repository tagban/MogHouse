# MogHouse C++/C# interop - design notes

Status as of 2026-08-27: design + skeleton only. Nothing here has been
compiled - this machine has no CMake, Ninja, or Vulkan SDK installed (only
the redistributable `vulkaninfo.exe` that ships with graphics drivers, not
the SDK). Everything below was grounded in reading the real engine source
(`engine/lotus/engine.cppm`, `engine/lotus/game.cppm`, `engine/lotus/util/task.cppm`,
`ffxi-engine/ffxi/game.cppm`, `ffxi-engine/ffxi/main.cpp`), not guessed, but
"reads correctly" and "compiles" are different claims - treat this as a
draft to validate once a real toolchain exists.

## Architecture

`lotus::Engine::run()` owns a blocking main loop internally (a coroutine,
see `engine/lotus/engine.cppm`); it is not designed to be driven frame-by-frame
from outside. The seam the engine already provides for host code is
`lotus::Game::tick(time, delta)` - a virtual, called once per frame by the
engine's own loop via `Game::tick_all`. `FFXIGame` (lotus-ffxi) already
overrides this to do zone/entity simulation and already solves DAT asset
loading in `entry()` - the hardest, most uncertain part of this whole
project.

So the design here is: **the C++ engine keeps owning its main loop, on its
own thread**; `MogHouseGame : public FFXIGame` (see `src/moghouse_interop.cpp`)
overrides `tick()` to call `FFXIGame::tick()` first (so real zone/entity
simulation still happens), then invokes a registered C function pointer so
the managed side gets a per-frame hook. The managed side calls the blocking
`pj_game_run()` from a dedicated background thread, not the UI thread, and
communicates with the rest of the C# app (networking, chat, macros) through
whatever thread-safe hand-off it needs on top of the tick callback.

This mirrors `ffxi-engine/ffxi/main.cpp`'s own usage exactly (construct the
Game subclass with `Settings`, then call `.run()` - no explicit
`engine->Init()` call), on the theory that the one confirmed-working
reference usage is safer to copy than inventing a different lifecycle.

## Known open questions / next steps

1. **CMake target linkage - the real blocker.** `ffxi-engine/ffxi/CMakeLists.txt`
   attaches its `.cppm` module sources to the `ffxi` `add_executable` target
   via `FILE_SET CXX_MODULES`, and its `audio/`, `dat/`, and (presumably)
   `entity/` subdirectory `CMakeLists.txt` files each hardcode
   `target_sources(ffxi ...)` against that same target name - confirmed by
   reading `ffxi/CMakeLists.txt`, `ffxi/audio/CMakeLists.txt`, and
   `ffxi/dat/CMakeLists.txt` directly. That means nothing outside
   `ffxi-engine` can currently link against `FFXIGame`; `target_link_libraries(moghouse_interop PUBLIC ffxi-lib)`
   in this directory's `CMakeLists.txt` refers to a target that doesn't
   exist yet.

   The same hardcoding also shows up in `ffxi/entity/CMakeLists.txt` (plus its
   own nested `component/` and `loader/` subdirectories, not yet read) and in
   `ffxi/shaders/CMakeLists.txt`'s `add_dependencies(ffxi ...)` - the actual
   surface to patch is larger than just `audio/`+`dat/`, and wasn't fully
   catalogued before stopping for the night given there's no local toolchain
   to test any of it against anyway.

   Recommended fix: a small, maintained patch to `ffxi-engine`'s CMake files
   (not a hand-edit inside the submodule's working tree, which would just be
   lost on the next submodule update) that either renames the target to a
   variable so a library and a thin `main.cpp`-only executable can both
   depend on the same sources, or introduces a new `ffxi-lib` target and
   points the existing `ffxi` executable at it. Apply this as a patch file
   under version control (e.g. `native/patches/`) applied via CMake's
   `PATCH_COMMAND` if `ffxi-engine` is ever pulled in as a `FetchContent`, or
   just re-applied manually after a submodule bump - either way, don't lose
   track of it as a plain uncommitted local edit the way the earlier
   SSH-\>HTTPS `.gitmodules` fix was.

2. **Exceptions inside the engine's run loop aren't routed to
   `PjErrorCallback` yet.** `lotus::Engine::run()`'s exception behavior
   wasn't confirmed (only the `.cppm` interface was read, not `engine.cpp`'s
   body) - find out whether `run()` can throw, and if so wrap it, before
   relying on `pj_game_set_error_callback` for anything real.

3. **Threading/marshaling on the C# side.** The tick callback fires on the
   engine's own native thread, not a thread .NET's runtime started - the C#
   wrapper needs `[UnmanagedCallersOnly]` static callback targets (not
   instance delegates) for this to be safe, and the managed code on the
   other end of that callback needs to not block it for long. Not yet
   designed beyond the P/Invoke signatures in `MogHouse.Core/Interop/NativeEngine.cs`.

4. **`external/soloud`** (audio, referenced in `ffxi-engine/.gitmodules` but
   not a registered gitlink) is still unresolved - hit a local permission
   error cloning it tonight, unrelated to anything above. Not blocking this
   design.

---

## Superseded, 2026-08-29

**Do not compile this to validate it - the architecture it is built on has been
abandoned.**

This design drives `lotus::Engine::run()` and overrides `FFXIGame::tick()`, so
it depends on lotus-engine's Vulkan renderer owning the main loop. That renderer
cannot run on macOS at all: MoltenVK implements none of
`VK_KHR_ray_tracing_pipeline`, `VK_KHR_ray_query` or
`VK_KHR_acceleration_structure`, which lotus's only working render mode
requires. macOS is a hard requirement, so the renderer is being replaced with
WebGPU via Dawn - see `docs/renderer-webgpu.md`.

Getting this to compile would therefore prove a design we are not going to
ship. The toolchain excuse in the status note above is no longer the reason to
leave it alone; the design is.

**What is still worth keeping** is the analysis above of *where the seam is* -
that the engine owns a blocking main loop on its own thread and that host code
drives it through a per-frame callback rather than pumping frames from outside.
That ownership question is the same whatever the renderer is, and the answer
here is likely still right. The mechanism (subclassing `FFXIGame`, calling
`lotus::Engine::run`) is what does not carry over.

The working interop that replaced it is `ffxi-engine/interop/`, a C ABI shared
library verified end to end from C# into the retail DATs - see
`docs/engine-build.md`. That one is also renderer-independent, which is why it
survives this change.
