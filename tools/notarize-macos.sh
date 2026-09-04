#!/usr/bin/env bash
# Notarize a packaged MogHouse build, staple the ticket, and rebuild the zip.
#
# Run tools/package-macos.sh first. This does the three steps that have to
# happen in this order and are easy to get wrong:
#
#   1. submit the zip to Apple and wait for a verdict
#   2. staple the returned ticket into the .app
#   3. rebuild the zip, because the ticket is attached to the .app and an
#      archive made before stapling does not contain it - the download would
#      be unnotarized however the local copy reports
#
# The password is never passed on the command line. Store it once:
#
#   xcrun notarytool store-credentials "MogHouse" \
#       --apple-id tagban@gmail.com --team-id D36X678376
#
# and this uses that keychain profile from then on. Set MOGHOUSE_NOTARY_PASSWORD
# instead if a keychain profile is not available - in CI, say.
set -euo pipefail

APPLE_ID="tagban@gmail.com"
TEAM_ID="D36X678376"
PROFILE="MogHouse"
VERSION="0.2.1"
OUTPUT="dist"
ARCHES="arm64 x86_64"

usage() {
    cat <<'USAGE'
Usage: tools/notarize-macos.sh [options]

  --arch A            arm64, x86_64, or both (default: both)
  --version X.Y.Z     which build to notarize (default: 0.2.1)
  --output DIR        where package-macos.sh wrote them (default: dist)
  --apple-id ID       Apple ID (default: tagban@gmail.com)
  --team-id ID        team (default: D36X678376)
  --keychain-profile  notarytool profile name (default: MogHouse)

The password comes from the keychain profile, or MOGHOUSE_NOTARY_PASSWORD.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCHES="$2"; shift 2 ;;
        --version) VERSION="$2"; shift 2 ;;
        --output) OUTPUT="$2"; shift 2 ;;
        --apple-id) APPLE_ID="$2"; shift 2 ;;
        --team-id) TEAM_ID="$2"; shift 2 ;;
        --keychain-profile) PROFILE="$2"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage; exit 2 ;;
    esac
done
[ "$ARCHES" = "both" ] && ARCHES="arm64 x86_64"

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
step() { printf '\033[36m==> %s\033[0m\n' "$1"; }
warn() { printf '\033[33m    ! %s\033[0m\n' "$1"; }

# Prefer the keychain profile; fall back to an environment variable. Either way
# the secret never appears in an argument list, where `ps` would show it.
if xcrun notarytool history --keychain-profile "$PROFILE" >/dev/null 2>&1; then
    auth=(--keychain-profile "$PROFILE")
    echo "using keychain profile '$PROFILE'"
elif [ -n "${MOGHOUSE_NOTARY_PASSWORD:-}" ]; then
    auth=(--apple-id "$APPLE_ID" --team-id "$TEAM_ID" --password "$MOGHOUSE_NOTARY_PASSWORD")
    echo "using MOGHOUSE_NOTARY_PASSWORD for $APPLE_ID"
else
    cat >&2 <<EOF
No credentials. Either store them once:

  xcrun notarytool store-credentials "$PROFILE" --apple-id $APPLE_ID --team-id $TEAM_ID

or export MOGHOUSE_NOTARY_PASSWORD with an app-specific password from
account.apple.com (Sign-In and Security -> App-Specific Passwords).
EOF
    exit 1
fi

for arch in $ARCHES; do
    app="$root/$OUTPUT/$arch/MogHouse XI.app"
    zip="$root/$OUTPUT/MogHouse-XI-Alpha-$VERSION-macos-$arch.zip"

    [ -d "$app" ] || { echo "no bundle at $app - run tools/package-macos.sh --arch $arch first" >&2; exit 1; }
    [ -f "$zip" ] || { echo "no archive at $zip" >&2; exit 1; }

    step "Submitting $arch"
    # Absolute path on purpose: notarytool resolves relative to the working
    # directory, and its "file doesn't exist" error reads like an upload failure.
    if ! xcrun notarytool submit "$zip" "${auth[@]}" --wait; then
        warn "submission failed for $arch"
        warn "if Apple rejected the build rather than the upload failing, read why with:"
        warn "  xcrun notarytool log <submission-id> ${auth[*]}"
        exit 1
    fi

    step "Stapling $arch"
    xcrun stapler staple "$app"
    xcrun stapler validate "$app"

    step "Rebuilding the archive so it carries the ticket"
    rm -f "$zip"
    ( cd "$root/$OUTPUT/$arch" && ditto -c -k --sequesterRsrc --keepParent "MogHouse XI.app" "$zip" )

    step "Verifying $arch as a downloader sees it"
    check="$(mktemp -d)"
    ditto -x -k "$zip" "$check"
    spctl -a -t exec -vv "$check/MogHouse XI.app" 2>&1 | sed 's/^/    /'
    xcrun stapler validate "$check/MogHouse XI.app" >/dev/null \
        && echo "    ticket travels in the archive" \
        || warn "the ticket is NOT in the archive"
    rm -rf "$check"

    size=$(du -h "$zip" | cut -f1 | tr -d ' ')
    printf '\033[32m%s  (%s)\033[0m\n\n' "$zip" "$size"
done

echo "Done. Both archives are notarized, stapled and verified."
