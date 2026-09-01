#!/usr/bin/env bash
# Launch the windowed client - installer, login, character select and the world.
#
# The same paths tools/play.sh sets up, because the renderer inside the app
# needs exactly what the console one does. Use this rather than starting the
# exe directly: without these the window opens and then cannot find its key
# tables, its glyph atlas or the server data, and the failures are quiet.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if command -v cygpath >/dev/null 2>&1; then
    here="$(cygpath -m "$here")"
fi

: "${MOGHOUSE_FFXI_RES:=C:/Users/Gaming/Desktop/LandSandBoat/res}"
: "${MOGHOUSE_FFXI_KEYTABLE:=$here/keys/mzb_key_table.bin}"
: "${MOGHOUSE_FFXI_KEYTABLE2:=$here/keys/mmb_key_table2.bin}"

# The font atlas is otherwise found relative to the working directory, so
# launching from anywhere else silently loses the HUD and every nameplate -
# the whole nameplate pass is gated on the font having loaded.
: "${MOGHOUSE_FONT:=$here/renderer/assets}"

# Server-side data the project does not ship. Without the zone directory there
# are no zone lines, so walking to the edge of a zone does nothing at all - the
# client is the only side that can start a zone change.
# Which DAT files hold each zone's building interiors. Found relative to the
# working directory otherwise, and a zone loads perfectly well without it -
# just with every building an empty shell.
: "${MOGHOUSE_SUBROOMS:=$here/renderer/assets/subrooms.txt}"

: "${MOGHOUSE_FFXI_ZONEDATA:=C:/Users/Gaming/Desktop/LandSandBoat/data/zones}"
: "${MOGHOUSE_FFXI_NAVMESHES:=C:/Users/Gaming/Desktop/LandSandBoat/navmeshes}"

export MOGHOUSE_FFXI_RES MOGHOUSE_FFXI_KEYTABLE MOGHOUSE_FFXI_KEYTABLE2 MOGHOUSE_FONT
export MOGHOUSE_FFXI_ZONEDATA MOGHOUSE_FFXI_NAVMESHES MOGHOUSE_SUBROOMS

# Where the client writes down what it did. It is a windowed application with
# no console, so without this everything it says goes nowhere and a bug reported
# from inside the game leaves nothing to read afterwards.
: "${MOGHOUSE_LOG:=$here/moghouse.log}"
export MOGHOUSE_LOG

# Git Bash rewrites an argument that looks like a Unix path into a Windows one.
export MSYS_NO_PATHCONV=1
export MSYS2_ARG_CONV_EXCL="*"

exec "$here/src/MogHouse.App/bin/Debug/net10.0/MogHouse.App.exe" "$@"
