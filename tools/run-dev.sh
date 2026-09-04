#!/usr/bin/env bash
#
# Runs the client from a development build.
#
# The repository does not hold everything the client needs at runtime. The key
# tables and the renderer's assets are staged into the build output by
# MogHouse.App.csproj, but two directories belong to the server rather than to
# us and cannot be:
#
#   res     LandSandBoat's compress.dat and decompress.dat, without which a
#           zone reply decrypts, passes its checksum and decodes to nothing
#   zones   the zone lines, without which walking to the edge of a zone does
#           nothing
#
# Point MOGHOUSE_LSB at a LandSandBoat checkout and both are found. Set
# DOTNET_ROOT too if dotnet lives somewhere the app host does not look, which
# it does whenever the SDK was installed to ~/.dotnet rather than system-wide.

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
lsb="${MOGHOUSE_LSB:-/Volumes/AppStorage/LandSandBoat}"
dotnet="${DOTNET:-$(command -v dotnet || echo "$HOME/.dotnet/dotnet")}"

if [ ! -x "$dotnet" ]; then
    echo "no dotnet found - set DOTNET to it" >&2
    exit 1
fi

if [ ! -d "$lsb/res" ]; then
    echo "no LandSandBoat res directory at $lsb/res" >&2
    echo "set MOGHOUSE_LSB to a checkout, or MOGHOUSE_FFXI_RES to the res folder itself" >&2
fi

"$dotnet" build "$root/src/MogHouse.App/MogHouse.App.csproj" -v q --nologo

export DOTNET_ROOT="${DOTNET_ROOT:-$(dirname "$dotnet")}"
export MOGHOUSE_FFXI_RES="${MOGHOUSE_FFXI_RES:-$lsb/res}"
export MOGHOUSE_FFXI_ZONEDATA="${MOGHOUSE_FFXI_ZONEDATA:-$lsb/data/zones}"
export MOGHOUSE_FFXI_NAVMESHES="${MOGHOUSE_FFXI_NAVMESHES:-$lsb/navmeshes}"

exec "$dotnet" "$root/src/MogHouse.App/bin/Debug/net10.0/MogHouse XI.dll" "$@"
