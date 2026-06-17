#!/usr/bin/env bash
#
# Build a drag-to-Applications .dmg for Argus from a built Argus.app.
#
# Usage:
#   packaging/macos/build-dmg.sh [path/to/Argus.app]
#
# The app is shipped UNSIGNED by default; first-launch users right-click the app
# and choose "Open" (or run: xattr -dr com.apple.quarantine /Applications/Argus.app).
#
# To sign + notarize (no Xcode GUI needed - all CLI, Jenkins-friendly) set:
#   ARGUS_CODESIGN_ID   "Developer ID Application: Your Name (TEAMID)"
#   ARGUS_NOTARY_PROFILE name of a `notarytool store-credentials` keychain profile
# Both require an Apple Developer Program membership ($99/yr); no separate
# certificate purchase is needed - that membership IS the certificate authority.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
APP="${1:-$ROOT/build/src/ui/Argus.app}"
OUT="${OUT:-$ROOT/dist}"
VERSION="${VERSION:-$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed 's/^v//' || echo dev)}"

if [[ ! -d "$APP" ]]; then
    echo "error: app bundle not found at $APP" >&2
    echo "build it first: cmake -S . -B build -DARGUS_BUILD_GUI=ON && cmake --build build -j" >&2
    exit 1
fi

mkdir -p "$OUT"
DMG="$OUT/Argus-${VERSION}.dmg"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

cp -R "$APP" "$STAGE/Argus.app"
ln -s /Applications "$STAGE/Applications"

# Optional code signing (Developer ID) before packaging.
if [[ -n "${ARGUS_CODESIGN_ID:-}" ]]; then
    echo "Signing app with: $ARGUS_CODESIGN_ID"
    codesign --force --deep --options runtime --timestamp \
        --sign "$ARGUS_CODESIGN_ID" "$STAGE/Argus.app"
fi

echo "Creating $DMG"
hdiutil create -volname "Argus" -srcfolder "$STAGE" -ov -format UDZO "$DMG"

# Optional notarization + stapling of the finished .dmg.
if [[ -n "${ARGUS_CODESIGN_ID:-}" && -n "${ARGUS_NOTARY_PROFILE:-}" ]]; then
    echo "Notarizing $DMG"
    xcrun notarytool submit "$DMG" --keychain-profile "$ARGUS_NOTARY_PROFILE" --wait
    xcrun stapler staple "$DMG"
fi

echo "wrote $DMG"
