#!/usr/bin/env bash
#
# Build a Fedora/Alma/RHEL .rpm for Argus from a source tarball of the repo.
#
# Build deps:  rpm-build cmake gcc-c++ libsndfile-devel mesa-libGL-devel
#              libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel
#              wayland-devel libxkbcommon-devel wayland-protocols-devel libdecor-devel
#
# Usage: packaging/linux/build-rpm.sh [version]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
VERSION="${1:-$(git -C "$ROOT" describe --tags --always 2>/dev/null | sed 's/^v//' || echo 1.0.0)}"
TOP="$ROOT/build-rpm/rpmbuild"
OUT="$ROOT/dist"

rm -rf "$ROOT/build-rpm"
mkdir -p "$TOP"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}

# Source tarball named <name>-<version>/ as %autosetup expects.
TARROOT="argus-${VERSION}"
git -C "$ROOT" archive --format=tar --prefix="${TARROOT}/" HEAD | gzip > "$TOP/SOURCES/${TARROOT}.tar.gz"

cp "$ROOT/packaging/linux/argus.spec" "$TOP/SPECS/argus.spec"
rpmbuild --define "_topdir $TOP" --define "_argus_version ${VERSION}" \
         -ba "$TOP/SPECS/argus.spec"

mkdir -p "$OUT"
find "$TOP/RPMS" -name '*.rpm' -exec cp {} "$OUT/" \;
echo "wrote rpms to $OUT:"
ls -1 "$OUT"/*.rpm
