#!/bin/bash
# Tadpole — scan a folder of game backups so the viewer can show icons.
#
#   ./tools/scan-games.sh <folder> [--force]
#
# A one-line wrapper so the viewer can spawn a SCRIPT rather than having to
# work out which Python to use: that answer lives in tools/lib-deps.sh, and it
# differs between a source checkout and the AppImage. The work is in
# scan-games.py.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
. "$HERE/lib-deps.sh"

PY="$(tad_python || true)"
if [ -z "$PY" ]; then
    echo "no python3 available to read the game backups." >&2
    echo "  ./tools/fetch-deps.sh   stages one into build/deps" >&2
    exit 1
fi

exec "$PY" "$HERE/scan-games.py" "$@"
