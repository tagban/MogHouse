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
export MOGHOUSE_FFXI_RES MOGHOUSE_FFXI_KEYTABLE MOGHOUSE_FFXI_KEYTABLE2

exec "$here/src/MogHouse.Console/bin/Debug/net10.0/MogHouse.Console.exe" "$@"
