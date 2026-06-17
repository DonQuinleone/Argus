#!/usr/bin/env bash
#
# Build a Debian/Ubuntu .deb for Argus (CLI + GUI). Produces dist/argus_<ver>_<arch>.deb
#
# Build deps:  cmake g++ libsndfile1-dev libgl1-mesa-dev xorg-dev pkg-config
#              libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev
#              (optional, for AAC/ALAC) libavformat-dev libavcodec-dev libavutil-dev
# Runtime deps declared below: libsndfile1, libgl1.
#
# Usage: packaging/linux/build-deb.sh [version]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${1:-$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed 's/^v//' || echo 1.0.0)}"
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
BUILD="$ROOT/build-deb"
PKG="$ROOT/build-deb/pkgroot"
OUT="$ROOT/dist"

rm -rf "$BUILD"
cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -DARGUS_BUILD_GUI=ON \
      -DCMAKE_INSTALL_PREFIX=/usr
cmake --build "$BUILD" -j"$(nproc)"
DESTDIR="$PKG" cmake --install "$BUILD"

mkdir -p "$PKG/DEBIAN"
cat > "$PKG/DEBIAN/control" <<EOF
Package: argus
Version: ${VERSION}
Section: sound
Priority: optional
Architecture: ${ARCH}
Depends: libsndfile1, libc6, libstdc++6, libgl1, libx11-6, libxrandr2, libxinerama1, libxcursor1, libxi6, libwayland-client0, libwayland-cursor0, libwayland-egl1, libxkbcommon0
Maintainer: Argus <argus@localhost>
Description: Audio master QA - defect, loudness and true-peak analysis
 Argus catches dropouts, glitches, clipping and loudness/true-peak violations in
 delivered audio masters, with an RX-style spectrogram and PDF/CSV/JSON reports.
 Ships a desktop app (argus-gui) and a headless CLI (argus) for CI gating.
EOF

mkdir -p "$OUT"
DEB="$OUT/argus_${VERSION}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "$PKG" "$DEB"
echo "wrote $DEB"
