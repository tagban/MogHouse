# Vendored build dependencies for engine/ffxi-engine

Everything here exists because `ffxi-engine`'s (lotus-ffxi's) own README says
Windows only needs Visual Studio + the Vulkan SDK, and that turned out to be
stale - the current Vulkan SDK (1.4.357.0, and evidently for a while before
that) no longer bundles GLM or SDL2 the way the README assumes, and the
engine itself has since moved from SDL2 to SDL3. Everything below was worked
out empirically, 2026-08-28, actually getting `ffxi-engine` to configure and
build in this environment for the first time.

## glm/ (git submodule, pinned to tag 1.0.3)

`engine/cmake/FindGLM.cmake` expects a `glm.cppm` C++20 module file sitting
next to `glm/glm.hpp` - GLM 1.0.3 ships one officially (`glm/glm.cppm`,
re-exporting the `glm` namespace), so nothing needed to be hand-authored.
Point the build at it with:

```
set GLM_ROOT_DIR=<repo>/vendor/glm
```

## vma/ (git submodule, pinned to tag v3.4.0) + vma-install/ (gitignored, regenerate locally)

VulkanMemoryAllocator is header-only but `find_package(VulkanMemoryAllocator CONFIG REQUIRED)`
needs an actual installed package config (not just the source checkout) -
its own CMakeLists.txt supports `install()`, so build+install it once:

```
vendor/vma/build_and_install.bat
```

This produces `vendor/vma-install/share/cmake/VulkanMemoryAllocator/...`.
Point the engine build at it with:

```
set VulkanMemoryAllocator_DIR=<repo>/vendor/vma-install/share/cmake/VulkanMemoryAllocator
```

## SDL3-3.4.14/ (gitignored, re-download - not a submodule, it's a prebuilt zip)

The engine now needs SDL3, not SDL2. Downloaded the official prebuilt VC devel
package directly (`SDL3-devel-3.4.14-VC.zip` from
`github.com/libsdl-org/SDL/releases`), extracted as-is - contains its own
`cmake/SDL3Config.cmake`. Point the build at it with:

```
set SDL3_DIR=<repo>/vendor/SDL3-3.4.14/cmake
```

## Slang shader compiler

No separate install needed - the Vulkan SDK bundles `slangc.exe` at
`%VULKAN_SDK%/Bin/slangc.exe`. Pass it explicitly as a CMake cache variable
(the `SLANGC` env var alone was not picked up):

```
-DSLANGC=%VULKAN_SDK%\Bin\slangc.exe
```

## `import std;` (MSVC + CMake experimental gate)

The Vulkan SDK's own `vulkan.cppm` (a real C++20 module Vulkan-Hpp ships)
does `import std;`. MSVC supports this, but CMake only enables scanning/
building the standard library as a module behind an experimental gate that
must be set **before** `project()` runs, so it has to be a cache variable
passed to the initial `cmake -S ... -B ...` configure call, not set from
inside CMakeLists.txt:

```
-DCMAKE_CXX_MODULE_STD=1
-DCMAKE_EXPERIMENTAL_CXX_IMPORT_STD=<uuid>
```

The UUID is **specific to the exact CMake version installed** and changes
between releases - it's not published in the installed CMake's own docs
(only in CMake's git repo, `Help/dev/experimental.rst`, checked out at the
matching version tag). For CMake 4.3.1 (bundled with VS 2026/`18`), the
value is `451f2fe2-a8a2-47c3-bc32-94786d8fc91b`. If upgrading CMake, refetch
this from `Help/dev/experimental.rst` at the new version's tag - don't reuse
this value blindly.

## One local source fix (not vendored, but adjacent)

`ffxi-engine/lotus-engine/lotus/renderer/mesh.cpp` had an unused
`#include <unistd.h>` (the whole function body is commented out, nothing in
the file references anything from that header) - it doesn't exist on
Windows/MSVC at all, so this failed the build outright. Removed the include
directly in the submodule's checkout (a genuine dead-code cleanup, not a
platform workaround) - not yet upstreamed.

## Vulkan-Hpp module rename (source fix, not vendored)

lotus-engine/ffxi-engine's entire codebase does `import vulkan_hpp;`
everywhere (67 files) - but the Vulkan SDK 1.4.357.0's own `vulkan.cppm`
declares `export module vulkan;` (not `vulkan_hpp`). This is upstream
version skew between whatever Vulkan-Hpp release the engine was written
against and the current SDK's bundled one, not an environment problem.
Fixed with a straight, mechanical find-and-replace of the literal string
`import vulkan_hpp;` -> `import vulkan;` across every file that had it, in
both `ffxi-engine/` and its `lotus-engine` submodule. Not yet upstreamed.

## `export template <>` on explicit specializations (source fix, not vendored)

MSVC rejects `export` on an explicit template specialization when the
primary template is already exported (`error C7760`) - two places had this:
`lotus/util/task.cppm`'s `Promise<void>` and `lotus/util/worker_task.cppm`'s
`WorkerPromise<void>`. Fixed by dropping the redundant `export` keyword on
just the specialization (the primary template being exported already covers
it) - not a behavior change, just stricter standard conformance than
whatever compiler this was last tested against.

## Current hard blocker: MSVC's `import std` has an internal bug on `stop_token`

2026-08-28: after resolving everything above, the build gets deep into
actual compilation (past GLM/SDL3/VMA/Slang/vulkan_hpp-rename/export-template
issues) and then fails inside MSVC's *own* STL headers, not project code:

```
C:\...\VC\Tools\MSVC\14.51.36231\include\stop_token(248): fatal error C1116:
unrecoverable error importing module 'std'.  Specialization of
'std::_Stop_callback_base::_Do_attach' with arguments 'false'
```

This happens while importing the `std` module itself (not user code) in
multiple different translation units (`post_process_pipeline.cppm`,
`raster_pipeline.cppm`, `gpu.cppm`, ...), which points to a genuine MSVC
limitation in its experimental `import std;` support around
`std::stop_token`/`jthread`, not anything wrong in this codebase. No fix
found tonight - this isn't a source-level bug to patch, it needs either a
different MSVC toolset version or avoiding `import std` project-wide (which
would mean converting away from `import std;` everywhere it's used - a much
bigger undertaking, not attempted). Stopped here.

## Putting it all together

See `ffxi-engine/configure.bat` and `ffxi-engine/build.bat` for the full,
current working invocation combining all of the above.
