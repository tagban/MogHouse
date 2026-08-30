#!/usr/bin/env bash
# Build the WebGPU renderer slice. Run build-dawn.sh first.
#
# UNTESTED - written on a Windows machine.
#
# SDL3 is not vendored for these platforms (the checked-in one is a Windows
# devel package), so install it first - "brew install sdl3" on macOS, or your
# distribution's SDL3 development package on Linux.
set -euo pipefail
cd "$(dirname "$0")"

cmake -S renderer -B build-renderer -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DDawn_DIR="$PWD/vendor/dawn-install/lib/cmake/Dawn"

cmake --build build-renderer
echo
echo "run it with: ./build-renderer/moghouse-renderer"
echo "it should print which backend it got - on macOS that must say Metal."
