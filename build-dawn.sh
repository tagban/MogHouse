#!/usr/bin/env bash
# Build and install Dawn (WebGPU). macOS and Linux counterpart to build-dawn.bat.
#
# UNTESTED - written on a Windows machine. Needs cmake, ninja, git and python3.
set -euo pipefail
cd "$(dirname "$0")"

if [ ! -d vendor/dawn ]; then
    echo "fetching dawn"
    git clone --depth 1 https://dawn.googlesource.com/dawn vendor/dawn
fi

cmake -S vendor/dawn -B build-dawn -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DDAWN_FETCH_DEPENDENCIES=ON \
    -DDAWN_ENABLE_INSTALL=ON \
    -DDAWN_BUILD_SAMPLES=OFF \
    -DDAWN_BUILD_TESTS=OFF \
    -DTINT_BUILD_TESTS=OFF \
    -DTINT_BUILD_CMD_TOOLS=OFF \
    -DDAWN_ENABLE_OPENGLES=OFF \
    -DDAWN_ENABLE_DESKTOP_GL=OFF

cmake --build build-dawn

# DawnTargets.cmake only exists after installing, so find_package(Dawn) cannot
# use the build tree directly.
cmake --install build-dawn --prefix "$PWD/vendor/dawn-install"
echo "dawn installed to vendor/dawn-install"
