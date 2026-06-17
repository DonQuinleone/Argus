#!/usr/bin/env bash
#
# Build a distro-agnostic Argus AppImage (bundles the GUI + its shared libs).
#
# Build deps:  cmake g++ libsndfile1-dev libgl1-mesa-dev xorg-dev pkg-config wget
# Uses linuxdeploy (downloaded on demand if not on PATH).
#
# Usage: packaging/linux/build-appimage.sh [version]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${1:-$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed 's/^v//' || echo 1.0.0)}"

# Run the AppImage-based tools without FUSE (CI runners often lack libfuse2).
export APPIMAGE_EXTRACT_AND_RUN=1
BUILD="$ROOT/build-appimage"
APPDIR="$BUILD/AppDir"
OUT="$ROOT/dist"
TOOLS="$BUILD/tools"

rm -rf "$BUILD"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DARGUS_BUILD_GUI=ON \
      -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD" -j"$(nproc)"
DESTDIR="$APPDIR" cmake --install "$BUILD"

# Fetch linuxdeploy if not available.
LD="$(command -v linuxdeploy || true)"
if [[ -z "$LD" ]]; then
    mkdir -p "$TOOLS"
    LD="$TOOLS/linuxdeploy-x86_64.AppImage"
    wget -qO "$LD" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage"
    chmod +x "$LD"
fi

mkdir -p "$OUT"
cd "$BUILD"
OUTPUT="Argus-${VERSION}-x86_64.AppImage" \
"$LD" --appdir "$APPDIR" \
      --desktop-file "$APPDIR/usr/share/applications/argus.desktop" \
      --icon-file "$APPDIR/usr/share/pixmaps/argus.png" \
      --output appimage
mv -f "$BUILD"/Argus-*.AppImage "$OUT/" 2>/dev/null || mv -f "$BUILD"/*.AppImage "$OUT/"
echo "wrote AppImage to $OUT"
