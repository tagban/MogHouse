# Builds the C ABI interop shim (include/portjeuno_interop.h,
# src/portjeuno_interop.cpp) as a shared library for PortJeuno.Core to
# P/Invoke into.
#
# BLOCKED on a real target to link against - see README.md. This assumes an
# `ffxi-lib` CMake target exists exposing FFXIGame et al. as a library. That
# target does not exist yet: ffxi-engine/ffxi/CMakeLists.txt (and its
# audio/, dat/, entity/ subdirectories) all hardcode
# `target_sources(ffxi ...)` against the `ffxi` add_executable target
# directly, so nothing outside that repo can link against FFXIGame today.
# Confirmed by reading ffxi/CMakeLists.txt, ffxi/audio/CMakeLists.txt and
# ffxi/dat/CMakeLists.txt directly on 2026-08-27 - not a guess. README.md
# has the recommended fix.
#
# Also NOT YET BUILD-TESTED end to end - no CMake/Ninja/Vulkan SDK toolchain
# was available in the environment this was written in.
cmake_minimum_required(VERSION 3.28 FATAL_ERROR)

add_library(portjeuno_interop SHARED
    src/portjeuno_interop.cpp
)

target_include_directories(portjeuno_interop PUBLIC include)
target_compile_features(portjeuno_interop PUBLIC cxx_std_23)

# Does not exist yet - see README.md for why and how to add it.
target_link_libraries(portjeuno_interop PUBLIC ffxi-lib)
