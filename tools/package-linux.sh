#!/usr/bin/env bash
# Build the linux-x64 tree that flatpak/com.tagban.MogHouse.yml packages.
#
# UNTESTED - written on a Mac, where neither this nor flatpak-builder can run.
# Treat it as "this is what the build should do", not "this is known to work",
# and read docs/linux-handoff.md before assuming a failure means the design is
# wrong. It is short; read it rather than trusting it.
#
# Produces dist/linux-x64/ holding:
#
#   MogHouse XI                    one file: the .NET runtime and every managed
#                                  assembly published inside it
#   libmoghouse_interop.so         the renderer
#   libSDL3.so.0                   unless the runtime provides it
#   assets/  keys/  res/  zones/
#
# Everything sits in one directory because the renderer looks for its assets
# beside whichever directory its library was loaded from, and the client looks
# for AppContext.BaseDirectory then BaseDirectory/data. One flat directory
# satisfies both, which is why this is simpler than the macOS bundle.
set -euo pipefail

VERSION="0.2.0"
OUTPUT="dist/linux-x64"
RES=""
ZONEDATA=""
NO_WATER=0
NO_BUILD=0
RENDERER_BUILD=""

usage() {
    cat <<'USAGE'
Usage: tools/package-linux.sh [options]

  --version X.Y.Z     stamped into the README
  --output DIR        where the tree is written (default: dist/linux-x64)
  --res DIR           LandSandBoat res/ with compress.dat and decompress.dat
  --zone-data DIR     LandSandBoat data/zones, for zone lines
  --no-water          leave the ~55MB of water surfaces out
  --no-build          use whatever is already in the renderer build tree
  --renderer-build D  the renderer's build tree (default: <root>/build-renderer)
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --version) VERSION="$2"; shift 2 ;;
        --output) OUTPUT="$2"; shift 2 ;;
        --res) RES="$2"; shift 2 ;;
        --zone-data) ZONEDATA="$2"; shift 2 ;;
        --no-water) NO_WATER=1; shift ;;
        --no-build) NO_BUILD=1; shift ;;
        --renderer-build) RENDERER_BUILD="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$root/$OUTPUT"
[ -n "$RENDERER_BUILD" ] || RENDERER_BUILD="$root/build-renderer"

step() { printf '\033[36m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    ! %s\033[0m\n' "$1"; }

# --- the native renderer -----------------------------------------------------

if [ "$NO_BUILD" -eq 0 ]; then
    step "Building the renderer"
    # A cmake that finds no compiler reports success having done nothing, which
    # leaves a stale library in the package - so check the timestamp moved.
    lib="$RENDERER_BUILD/moghouse_interop/libmoghouse_interop.so"
    before=$(stat -c %Y "$lib" 2>/dev/null || echo 0)
    cmake --build "$RENDERER_BUILD"
    after=$(stat -c %Y "$lib" 2>/dev/null || echo 0)
    [ "$after" = "0" ] && { echo "no library at $lib" >&2; exit 1; }
    [ "$before" = "$after" ] && warn "the renderer library did not change - already up to date, or nothing was rebuilt"
fi

native="$RENDERER_BUILD/moghouse_interop"
[ -f "$native/libmoghouse_interop.so" ] || { echo "libmoghouse_interop.so is missing from $native" >&2; exit 1; }

# --- the managed client ------------------------------------------------------

step "Publishing the client"
rm -rf "$out"
mkdir -p "$out"

dotnet publish "$root/src/MogHouse.App/MogHouse.App.csproj" \
    -c Release -r linux-x64 --self-contained true \
    -p:PublishSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true \
    -p:EnableCompressionInSingleFile=true \
    -p:DebugType=none -p:GenerateDocumentationFile=false \
    -o "$out" --nologo -v quiet

find "$out" -name '*.pdb' -delete 2>/dev/null || true
[ -f "$out/MogHouse XI" ] || { echo "the published executable is not where it was expected" >&2; exit 1; }
chmod +x "$out/MogHouse XI"

# --- the renderer and its libraries ------------------------------------------

step "Copying the renderer"
cp "$native/libmoghouse_interop.so" "$out/"

# Whether SDL3 has to travel depends on where it came from. The freedesktop
# runtime a Flatpak builds against does not ship SDL3, so a distro SDL3 linked
# from /usr/lib will not be there at runtime - copy it in and make the loader
# look beside itself. If SDL3 came from a Flatpak SDK extension instead, this
# finds nothing and correctly does nothing.
sdl_path=$(ldd "$out/libmoghouse_interop.so" 2>/dev/null | awk '/libSDL3/ {print $3; exit}')
if [ -n "${sdl_path:-}" ] && [ -f "$sdl_path" ]; then
    cp -L "$sdl_path" "$out/$(basename "$sdl_path")"
    if command -v patchelf >/dev/null 2>&1; then
        patchelf --set-rpath '$ORIGIN' "$out/libmoghouse_interop.so"
        echo "    bundled $(basename "$sdl_path") and set rpath to \$ORIGIN"
    else
        warn "patchelf is not installed - libSDL3 was copied but the rpath was not set,"
        warn "so the loader will still look in the system path. Install patchelf."
    fi
else
    warn "SDL3 was not found by ldd - assuming the runtime provides it"
fi

# --- assets ------------------------------------------------------------------

step "Copying assets"
mkdir -p "$out/assets"
cp "$root/renderer/assets/font."* "$out/assets/"
cp "$root/renderer/assets/subrooms.txt" "$out/assets/"
cp "$root/renderer/assets/hidden-models.txt" "$out/assets/"
cp "$root/renderer/assets/burrowers.txt" "$out/assets/"

# --- where bug reports go -----------------------------------------------------

# `/bug` posts to a Discord webhook, and a tester has no way to configure one -
# so a shipped build has to carry it. Taken from MOGHOUSE_BUG_WEBHOOK or from
# the per-user copy, and written into data/ beside the runtime.
#
# In the build, not in the repository. Anything shipped can be taken apart, so
# this URL should be treated as public to anyone holding the client - which is
# survivable for a webhook, because the worst it allows is posting into one
# channel and it is revoked in a click. A repository is different: it is
# permanently searchable and scraped, and history keeps what you delete.
#
# Without one, /bug still writes its local file and says nobody has seen it.
step "Bug report webhook"
webhook="${MOGHOUSE_BUG_WEBHOOK:-}"
if [ -z "$webhook" ] && [ -f "$HOME/Library/Application Support/MogHouse/bug-webhook.txt" ]; then
    webhook="$(cat "$HOME/Library/Application Support/MogHouse/bug-webhook.txt")"
fi
if [ -z "$webhook" ] && [ -f "$HOME/.local/share/MogHouse/bug-webhook.txt" ]; then
    webhook="$(cat "$HOME/.local/share/MogHouse/bug-webhook.txt")"
fi
if [ -n "$webhook" ]; then
    printf '%s\n' "$webhook" > "$out/bug-webhook.txt"
    echo "    bug-webhook.txt (reports will reach the channel)"
else
    warn "No bug webhook: /bug will write its local file and go no further. Set MOGHOUSE_BUG_WEBHOOK to include one."
fi


if [ "$NO_WATER" -eq 1 ]; then
    warn "No water: canals and seas will be dry."
else
    count=0
    if [ -d "$root/renderer/assets/water" ]; then
        count=$(find "$root/renderer/assets/water" -name '*.water' | wc -l | tr -d ' ')
    fi
    if [ "$count" = "0" ]; then
        warn "No .water files - run 'python3 tools/makewater.py --root <LandSandBoat>/ximeshes' first."
    else
        mkdir -p "$out/assets/water"
        cp "$root/renderer/assets/water/"*.water "$out/assets/water/"
        echo "    $count zones of water"
    fi
fi

step "Copying the key tables"
if ls "$root/keys/"*.bin >/dev/null 2>&1; then
    mkdir -p "$out/keys"; cp "$root/keys/"*.bin "$out/keys/"
else
    warn "No key tables - run 'python3 tools/keytables.py'. Without them no zone decrypts."
fi

step "Copying the compression tables"
if [ -n "$RES" ]; then
    mkdir -p "$out/res"
    for table in compress.dat decompress.dat; do
        [ -f "$RES/$table" ] || { echo "$table was not found in $RES" >&2; exit 1; }
        cp "$RES/$table" "$out/res/"
    done
else
    warn "No --res: the client will not be able to connect to any server."
fi

if [ -n "$ZONEDATA" ]; then
    step "Copying zone data"
    [ -d "$ZONEDATA" ] || { echo "no zone data at $ZONEDATA" >&2; exit 1; }
    cp -R "$ZONEDATA" "$out/zones"
else
    warn "No zone data: walking to the edge of a zone will not change zones. Use !zone."
fi

# --- check --------------------------------------------------------------------

# Checked rather than assumed, for the same reason the macOS script checks: a
# tree with one library of the wrong architecture builds and packages cleanly
# and only fails on the machines it was built for.
step "Checking architecture"
bad=0
while IFS= read -r binary; do
    got=$(file -b "$binary" 2>/dev/null || echo "?")
    case "$got" in
        *x86-64*) ;;
        *) warn "$(basename "$binary"): $got"; bad=1 ;;
    esac
done < <(find "$out" -maxdepth 1 -type f \( -name '*.so*' -o -perm -u+x \) 2>/dev/null)
[ "$bad" -eq 1 ] && { echo "refusing to package a tree with mixed architectures" >&2; exit 1; }
echo "    everything is x86-64"

size=$(du -sh "$out" | cut -f1 | tr -d ' ')
echo
printf '\033[32m%s  (%s)\033[0m\n' "$out" "$size"
echo
echo "Next: flatpak-builder --user --install --force-clean \\"
echo "        build-flatpak flatpak/com.tagban.MogHouse.yml"
