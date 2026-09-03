#!/usr/bin/env bash
# Build a MogHouse .app someone else can drag to Applications and run.
#
# The macOS counterpart to package-windows.ps1. Same idea, different shape:
#
#   MogHouse XI.app/Contents/
#     Info.plist          the bundle identity - without this the menu bar and
#                         the Dock say "Avalonia Application"
#     MacOS/
#       MogHouse XI       one file: the .NET runtime and every managed
#                         assembly are published inside it
#       data/             the renderer and everything it reads
#         libmoghouse_interop.dylib, libSDL3.0.dylib
#         assets/  keys/  res/  zones/
#
# data/ sits beside the executable rather than in Contents/Resources on
# purpose. NativeViewer.SearchDirectories looks in AppContext.BaseDirectory and
# then BaseDirectory/data, and every managed data path falls back the same way,
# so this layout needs no macOS-specific code at all - it is the Windows layout
# with the executable in Contents/MacOS. The renderer also looks for its assets
# beside whichever directory its library was loaded from, which this satisfies.
#
# What does not ship: the game's own DATs. The client reads an install the
# player already has.
set -euo pipefail

VERSION="0.1.2"
OUTPUT="dist"
RES=""                      # LandSandBoat's res/ - compress.dat, decompress.dat
ZONEDATA=""                 # LandSandBoat's data/zones - optional, zone lines
NO_WATER=0
NO_BUILD=0
SIGN_ID="Developer ID Application"   # substring match; "-" for ad-hoc
NO_SIGN=0
ARCH="arm64"                # arm64 (Apple silicon) or x86_64 (Intel)
RENDERER_BUILD=""           # defaults to <root>/build-renderer

usage() {
    sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
    cat <<'USAGE'

Options:
  --version X.Y.Z     stamped into the bundle and the zip name
  --output DIR        where the .app and zip are written (default: dist)
  --res DIR           LandSandBoat res/ holding compress.dat and decompress.dat
  --zone-data DIR     LandSandBoat data/zones, for zone lines
  --no-water          leave the ~50MB of water surfaces out
  --no-build          use whatever is already in build-renderer
  --sign-id NAME      signing identity, or "-" for ad-hoc (default: Developer ID)
  --no-sign           do not sign at all
  --arch A            arm64 (default) or x86_64. Each needs its own Dawn and
                      SDL3 build, so pass --renderer-build with it.
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
        --sign-id) SIGN_ID="$2"; shift 2 ;;
        --no-sign) NO_SIGN=1; shift ;;
        --arch) ARCH="$2"; shift 2 ;;
        --renderer-build) RENDERER_BUILD="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# The .NET runtime identifier does not use the same spelling as the compiler,
# and the two architectures cannot share a build tree - Dawn and SDL3 have to
# be built for each, so the renderer build directory differs too.
case "$ARCH" in
    arm64)  RID="osx-arm64" ;;
    x86_64) RID="osx-x64" ;;
    *) echo "unknown --arch '$ARCH': expected arm64 or x86_64" >&2; exit 2 ;;
esac
[ -n "$RENDERER_BUILD" ] || RENDERER_BUILD="$root/build-renderer"

# One .app per architecture, so a build of one does not overwrite the other.
app="$root/$OUTPUT/$ARCH/MogHouse XI.app"
contents="$app/Contents"
macos="$contents/MacOS"
# The real files live in Contents/Resources/data, with a symlink to them at
# Contents/MacOS/data. codesign treats everything under Contents/MacOS as code
# and refuses to seal a bundle with a font.png in there, but the client looks
# for AppContext.BaseDirectory/data in five different places - so the symlink
# keeps that convention true without a macOS branch in any of them.
data="$contents/Resources/data"
zip="$root/$OUTPUT/MogHouse-XI-Alpha-$VERSION-macos-$ARCH.zip"

step() { printf '\033[36m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    ! %s\033[0m\n' "$1"; }

# --- the native renderer -----------------------------------------------------

if [ "$NO_BUILD" -eq 0 ]; then
    step "Building the renderer"
    # A cmake that finds no compiler reports success having done nothing, which
    # leaves a stale library in the package - so check the timestamp moved.
    before=$(stat -f %m "$RENDERER_BUILD/moghouse_interop/libmoghouse_interop.dylib" 2>/dev/null || echo 0)
    cmake --build "$RENDERER_BUILD" >/dev/null
    after=$(stat -f %m "$RENDERER_BUILD/moghouse_interop/libmoghouse_interop.dylib" 2>/dev/null || echo 0)
    if [ "$after" = "0" ]; then
        echo "the renderer did not build - no library at $RENDERER_BUILD/moghouse_interop" >&2
        exit 1
    fi
    [ "$before" = "$after" ] && warn "the renderer library did not change - already up to date, or nothing was rebuilt"
fi

native="$RENDERER_BUILD/moghouse_interop"
[ -f "$native/libmoghouse_interop.dylib" ] || { echo "libmoghouse_interop.dylib is missing from $native" >&2; exit 1; }

# --- the managed client ------------------------------------------------------

step "Publishing the client"
rm -rf "$app"
mkdir -p "$data" "$macos"

dotnet publish "$root/src/MogHouse.App/MogHouse.App.csproj" \
    -c Release -r "$RID" --self-contained true \
    -p:PublishSingleFile=true \
    -p:IncludeNativeLibrariesForSelfExtract=true \
    -p:EnableCompressionInSingleFile=true \
    -p:DebugType=none -p:GenerateDocumentationFile=false \
    -o "$data" --nologo -v quiet

find "$data" -name '*.pdb' -delete 2>/dev/null || true

[ -f "$data/MogHouse XI" ] || { echo "the published executable is not where it was expected: $data/MogHouse XI" >&2; exit 1; }
mv "$data/MogHouse XI" "$macos/MogHouse XI"
chmod +x "$macos/MogHouse XI"

# AppContext.BaseDirectory is Contents/MacOS, and the client looks for "data"
# beside it. The files themselves are in Contents/Resources/data, where
# codesign will accept them as resources rather than unsigned code.
ln -sfn "../Resources/data" "$macos/data"

# --- the renderer and its libraries ------------------------------------------

step "Copying the renderer"
cp "$native/libmoghouse_interop.dylib" "$data/"

# SDL3 is linked by its absolute Homebrew path, so a copy of this bundle on a
# Mac without Homebrew would not start. Bring the library along and rewrite the
# reference to sit beside the loader.
sdl_ref=$(otool -L "$data/libmoghouse_interop.dylib" | awk '/libSDL3/ {print $1; exit}')
if [ -n "$sdl_ref" ]; then
    sdl_name=$(basename "$sdl_ref")
    sdl_path="$sdl_ref"

    # An SDL3 built from source records itself as @rpath/libSDL3.0.dylib rather
    # than an absolute path, so the reference is not a file and has to be
    # resolved. Ask the renderer's own CMake cache where it found SDL3 - that
    # is the one answer guaranteed to match what the renderer linked against.
    # Falling straight through to Homebrew here is what silently put an arm64
    # SDL3 into an x86_64 bundle: it signed, it verified, and it could not have
    # loaded on an Intel Mac.
    if [ ! -f "$sdl_path" ]; then
        sdl_dir=$(awk -F= '/^SDL3_DIR:/ {print $2}' "$RENDERER_BUILD/CMakeCache.txt" 2>/dev/null || true)
        for guess in "${sdl_dir%/cmake/SDL3}/lib/$sdl_name" \
                     "${sdl_dir%/lib/cmake/SDL3}/lib/$sdl_name" \
                     "/opt/homebrew/opt/sdl3/lib/$sdl_name"; do
            if [ -n "$guess" ] && [ -f "$guess" ]; then sdl_path="$guess"; break; fi
        done
    fi

    if [ -f "$sdl_path" ]; then
        cp -L "$sdl_path" "$data/$(basename "$sdl_ref")"
        chmod u+w "$data/$(basename "$sdl_ref")"
        install_name_tool -id "@loader_path/$(basename "$sdl_ref")" "$data/$(basename "$sdl_ref")"
        install_name_tool -change "$sdl_ref" "@loader_path/$(basename "$sdl_ref")" "$data/libmoghouse_interop.dylib"
        echo "    bundled $(basename "$sdl_ref")"
    else
        warn "SDL3 was not found at $sdl_ref - the bundle will only run where that path exists"
    fi
fi

# --- assets ------------------------------------------------------------------

step "Copying assets"
mkdir -p "$data/assets"
cp "$root/renderer/assets/font."* "$data/assets/"
cp "$root/renderer/assets/subrooms.txt" "$data/assets/"
cp "$root/renderer/assets/hidden-models.txt" "$data/assets/"

if [ "$NO_WATER" -eq 1 ]; then
    warn "No water: canals and seas will be dry."
else
    # Counted with find rather than a glob: under `set -o pipefail` an `ls`
    # that matches nothing fails the whole pipeline and takes the script with
    # it, which is a confusing way to be told there is no water.
    count=0
    if [ -d "$root/renderer/assets/water" ]; then
        count=$(find "$root/renderer/assets/water" -name '*.water' | wc -l | tr -d ' ')
    fi
    if [ "$count" = "0" ]; then
        warn "No .water files found - run 'python tools/makewater.py' first, or pass --no-water."
    else
        mkdir -p "$data/assets/water"
        cp "$root/renderer/assets/water/"*.water "$data/assets/water/"
        echo "    $count zones of water"
    fi
fi

step "Copying the key tables"
if ls "$root/keys/"*.bin >/dev/null 2>&1; then
    mkdir -p "$data/keys"
    cp "$root/keys/"*.bin "$data/keys/"
else
    warn "No key tables in keys/ - run 'python3 tools/keytables.py'. Without them no zone decrypts."
fi

step "Copying the compression tables"
if [ -n "$RES" ]; then
    mkdir -p "$data/res"
    for table in compress.dat decompress.dat; do
        [ -f "$RES/$table" ] || { echo "$table was not found in $RES" >&2; exit 1; }
        cp "$RES/$table" "$data/res/"
    done
else
    warn "No --res given: the client will not be able to connect to any server."
    warn "Point --res at LandSandBoat's res directory holding compress.dat and decompress.dat."
fi

if [ -n "$ZONEDATA" ]; then
    step "Copying zone data"
    [ -d "$ZONEDATA" ] || { echo "no zone data at $ZONEDATA" >&2; exit 1; }
    cp -R "$ZONEDATA" "$data/zones"
else
    warn "No zone data: walking to the edge of a zone will not change zones. Use !zone, or pass --zone-data."
fi

# --- bundle identity ---------------------------------------------------------

# Without this the menu bar, the Dock and the force-quit list all say "Avalonia
# Application", and NSHighResolutionCapable decides whether the window is
# retina or a scaled-up 1x one.
step "Copying the icon"
# <ApplicationIcon> in the csproj only reaches a Windows PE executable - .NET
# embeds moghouse.ico into the .exe and does nothing with it anywhere else. A
# .app takes an .icns named by CFBundleIconFile instead, so without this the
# Mac build shows the generic executable icon and is impossible to pick out of
# a Dock or an Applications list.
#
# Assets/MogHouse.icns was generated from Assets/moghouse.ico, whose largest
# frame is 256x256. Regenerate with sips into a .iconset and `iconutil -c
# icns`. There is deliberately no 1024px entry: it would be a 4x upscale of the
# source and visibly soft.
icon="$root/src/MogHouse.App/Assets/MogHouse.icns"
if [ -f "$icon" ]; then
    cp "$icon" "$contents/Resources/MogHouse.icns"
    echo "    MogHouse.icns"
else
    warn "no MogHouse.icns in src/MogHouse.App/Assets - the app will have a generic icon"
fi

step "Writing Info.plist"
cat > "$contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>MogHouse XI</string>
    <key>CFBundleDisplayName</key>       <string>MogHouse XI</string>
    <key>CFBundleExecutable</key>        <string>MogHouse XI</string>
    <key>CFBundleIconFile</key>          <string>MogHouse.icns</string>
    <key>CFBundleIdentifier</key>        <string>com.tagban.moghouse</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>$VERSION</string>
    <key>CFBundleVersion</key>           <string>$VERSION</string>
    <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
    <key>LSMinimumSystemVersion</key>    <string>13.0</string>
    <key>LSApplicationCategoryType</key> <string>public.app-category.role-playing-games</string>
    <key>NSHighResolutionCapable</key>   <true/>
    <key>NSSupportsAutomaticGraphicsSwitching</key><true/>
    <key>NSHumanReadableCopyright</key>  <string>MogHouse XI - ships no game data</string>
</dict>
</plist>
PLIST

step "Writing README"
mkdir -p "$contents/Resources"
cat > "$contents/Resources/README.txt" <<README
MogHouse XI - Alpha $VERSION (macOS, Apple silicon)

A from-scratch Final Fantasy XI client. This is an alpha.

What you need
-------------
  * A Final Fantasy XI installation on the AUGUST 2026 patch. The client reads
    the game's own files; no game data is included here. On a Mac the game
    usually lives inside a Wine or CrossOver prefix - point the client at the
    "FINAL FANTASY XI" folder when it asks.
  * A private server on that same version, and an account on it.

It is not backwards compatible, and an older install fails quietly rather than
loudly: file ids move between versions so the wrong model loads, and packet
layouts shift so fields are read from the wrong place.

Running it
----------
Drag "MogHouse XI.app" wherever you like and open it. Settings and the servers
you add are written inside the bundle's data folder.

If something is wrong
---------------------
Two logs, both plain text: moghouse.log for the client, moghouse.log.renderer
for the world. Attaching them to a bug report is the most useful thing you can
do.
README

# --- signing -----------------------------------------------------------------

# --- architecture -------------------------------------------------------------

# Checked rather than assumed. A bundle with one library of the wrong
# architecture signs cleanly, verifies cleanly, and then fails to load on the
# only machines it was built for - and it cannot be caught by running it here,
# because the machine that builds it is usually the one architecture that
# happens to work. This is the cheap check that catches it.
step "Checking architectures"
arch_bad=0
while IFS= read -r binary; do
    got=$(lipo -archs "$binary" 2>/dev/null || echo "?")
    case " $got " in
        *" $ARCH "*) ;;
        *) warn "$(basename "$binary") is $got, expected $ARCH"; arch_bad=1 ;;
    esac
done < <(find "$macos" "$data" -type f \( -name '*.dylib' -o -perm -u+x \) 2>/dev/null | grep -v '\.dylib\.' || true)
if [ "$arch_bad" -eq 1 ]; then
    echo "refusing to package a bundle with mixed architectures" >&2
    exit 1
fi
echo "    everything is $ARCH"

# --- signing -------------------------------------------------------------------

# Everything nested has to be signed before the bundle that contains it, or the
# outer signature is invalidated the moment anything inside changes.
if [ "$NO_SIGN" -eq 0 ]; then
    step "Signing"
    if [ "$SIGN_ID" != "-" ] && ! security find-identity -v -p codesigning | grep -q "$SIGN_ID"; then
        warn "no identity matching '$SIGN_ID' - falling back to ad-hoc, which runs here but not elsewhere"
        SIGN_ID="-"
    fi

    # The hardened runtime is required for notarization, and it forbids memory
    # that is writable and executable at once - which is exactly what CoreCLR's
    # JIT allocates. Without these three the app is signed, verifies, and then
    # dies at launch with "Failed to create CoreCLR, HRESULT: 0x80070008".
    # disable-library-validation is the third because the bundle loads its own
    # renderer and a copy of SDL3 that Apple did not sign.
    entitlements="$root/$OUTPUT/moghouse.entitlements"
    cat > "$entitlements" <<'ENT'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>com.apple.security.cs.allow-jit</key>                       <true/>
    <key>com.apple.security.cs.allow-unsigned-executable-memory</key><true/>
    <key>com.apple.security.cs.disable-library-validation</key>      <true/>
</dict>
</plist>
ENT

    sign_one() {
        codesign --force --timestamp --options runtime --entitlements "$entitlements" \
            --sign "$SIGN_ID" "$1" 2>/dev/null \
            || codesign --force --options runtime --entitlements "$entitlements" --sign "$SIGN_ID" "$1"
    }

    while IFS= read -r dylib; do sign_one "$dylib"; done < <(find "$data" -name '*.dylib')
    sign_one "$macos/MogHouse XI"
    sign_one "$app"

    codesign --verify --deep --strict "$app" && echo "    signature verifies"
    if [ "$SIGN_ID" = "-" ]; then
        warn "ad-hoc signed: Gatekeeper will refuse this on another Mac."
    else
        warn "signed but NOT notarized - Gatekeeper will still block it. Finish with:"
        warn "  tools/notarize-macos.sh --arch $ARCH --version $VERSION"
        warn "which submits, staples, and rebuilds the archive so it carries the ticket."
    fi
fi

# --- zip ---------------------------------------------------------------------

step "Compressing"
rm -f "$zip"
# ditto rather than zip: it preserves the bundle's symlinks and extended
# attributes, which a plain zip drops and which a signature depends on.
( cd "$root/$OUTPUT/$ARCH" && ditto -c -k --sequesterRsrc --keepParent "MogHouse XI.app" "$zip" )

size=$(du -h "$zip" | cut -f1 | tr -d ' ')
echo
printf '\033[32m%s\033[0m\n' "$zip"
printf '\033[32m    %s\033[0m\n' "$size"
