#!/usr/bin/env bash
#
# Dev helper: build the Argus GUI and launch it. By default it (re)generates the
# synthetic test fixtures and opens the app with them loaded, so every detector
# (including baked-in clipping) is easy to see in action.
#
#   ./run.sh            build + open with all test fixtures
#   ./run.sh --clean    wipe build/ and reconfigure first
#   ./run.sh --empty    build + open with no files
#   ./run.sh <paths...> build + open with the given files/folders
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"

clean=0
empty=0
paths=()
for arg in "$@"; do
    case "$arg" in
        --clean) clean=1 ;;
        --empty) empty=1 ;;
        -h|--help)
            sed -n '3,11p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) paths+=("$arg") ;;
    esac
done

# Configure + build (GUI).
if [[ $clean -eq 1 ]]; then
    echo "==> wiping $BUILD"
    rm -rf "$BUILD"
fi
if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    echo "==> configuring"
    cmake -S "$ROOT" -B "$BUILD" -DARGUS_BUILD_GUI=ON
fi
echo "==> building"
cmake --build "$BUILD" -j

# Resolve the GUI binary for this platform.
case "$(uname -s)" in
    Darwin) APP="$BUILD/src/ui/Argus.app/Contents/MacOS/Argus" ;;
    *)      APP="$BUILD/src/ui/argus-gui" ;;
esac
if [[ ! -x "$APP" ]]; then
    echo "error: GUI binary not found at $APP" >&2
    exit 1
fi

# Decide what to open.
if [[ ${#paths[@]} -gt 0 ]]; then
    echo "==> launching with ${#paths[@]} path(s)"
    exec "$APP" "${paths[@]}"
elif [[ $empty -eq 1 ]]; then
    echo "==> launching empty"
    exec "$APP"
fi

# Default: open with the synthetic fixtures (needs python3 + numpy).
FX="${TMPDIR:-/tmp}/argus_fixtures"
if command -v python3 >/dev/null 2>&1 && python3 -c 'import numpy' >/dev/null 2>&1; then
    echo "==> generating fixtures in $FX"
    python3 "$ROOT/tests/make_fixtures.py" "$FX" >/dev/null
    echo "==> launching with fixtures"
    exec "$APP" "$FX"
else
    echo "warning: python3 + numpy not available; launching empty" >&2
    exec "$APP"
fi
