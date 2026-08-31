#!/usr/bin/env bash
# Launch the client with the paths it needs.
#
# Four environment variables have to line up before the renderer will open,
# and finding that out one failed login at a time is slow. Anything already
# set in the environment wins, so this is a floor, not a policy.
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

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
: "${MOGHOUSE_FFXI_ZONEDATA:=C:/Users/Gaming/Desktop/LandSandBoat/data/zones}"
: "${MOGHOUSE_FFXI_NAVMESHES:=C:/Users/Gaming/Desktop/LandSandBoat/navmeshes}"

export MOGHOUSE_FFXI_RES MOGHOUSE_FFXI_KEYTABLE MOGHOUSE_FFXI_KEYTABLE2 MOGHOUSE_FONT
export MOGHOUSE_FFXI_ZONEDATA MOGHOUSE_FFXI_NAVMESHES

exec "$here/src/MogHouse.Console/bin/Debug/net10.0/MogHouse.Console.exe" "$@"
