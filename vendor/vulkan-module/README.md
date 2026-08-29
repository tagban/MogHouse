# A Vulkan C++ module that doesn't need `import std`

`vulkan.cppm` here is the Vulkan SDK's own module (1.4.357.0) with two changes.
It exists to remove this project's only dependency on the C++23 standard
library module — which is what had the native engine build stuck.

## The problem

The engine build died inside MSVC's STL while compiling the `std` module:

```
fatal error C1116 ... Specialization of 'std::_Stop_callback_base::_Do_attach'
```

That is a compiler bug in MSVC's experimental `import std` support, not
anything wrong with the project's source, and it fails in several unrelated
translation units.

Nothing in the engine actually wants `import std`. Searching `ffxi-engine/`
for it turns up exactly one hit, in a git commit message. The sole thing
dragging it in is the Vulkan SDK's module, which contains an unconditional:

```cpp
export import std;
```

Every `vulkan_*.hpp` header then skips its own standard includes when built as
a module — `#if !defined( VULKAN_HPP_CXX_MODULE )` guards the whole block — and
relies on that import to supply `std::array`, `std::string`, `std::vector` and
the rest.

## The change

1. Include those standard headers in the module's **global module fragment**
   (after `module;`, before `export module vulkan;`), where they attach to the
   global module rather than to `vulkan`.
2. Delete the `export import std;` line.

The header list came from grepping every `vulkan_*.hpp` for its `#include <…>`
lines, not from adding headers until it compiled.

Consumers that previously got `std` re-exported through `import vulkan;` must
include what they use. The engine already does, since it never used
`import std` in the first place.

## Verified

Compiles clean with MSVC 14.51.36231, producing a 67 MB `vulkan.ifc` and its
object file, with no standard library module anywhere in the build:

```
cl /nologo /std:c++latest /Zc:__cplusplus /EHsc /c ^
   /I "%VULKAN_SDK%\Include" ^
   /DVULKAN_HPP_CXX_MODULE_EXPERIMENTAL_WARNING= ^
   vulkan.ixx
```

Still to do: wire this into the engine's CMake in place of the SDK's module,
and drop `-DCMAKE_CXX_MODULE_STD=1` and
`-DCMAKE_EXPERIMENTAL_CXX_IMPORT_STD=<uuid>` from the configure line.

## Two traps worth knowing

**`/std:c++23` is not a valid MSVC flag.** MSVC takes `/std:c++20`,
`/std:c++23preview` or `/std:c++latest`. Given `/std:c++23` it falls back to an
older standard *without a clear error*, so `module;` stops being recognised and
you get a flood of misleading diagnostics — `C4430 missing type specifier` on
the `module;` line, and `C3378 "a declaration can be exported only from a
module interface unit"` pointing at every `export` in every header. All of it
cascade noise from one bad flag.

**Compile module interfaces as `.ixx`.** MSVC detects that extension
automatically; `/interface /TP` on a `.cppm` is fiddlier and the flag order
matters.

## Licensing

This is a derivative of a Khronos file, Apache-2.0 OR MIT — both permissive and
both fine to modify and redistribute with attribution. The original header is
preserved at the top of the file, and the two edits are marked `// PATCHED:`.
Regenerate it from a newer SDK by re-applying the same two changes rather than
hand-merging.
