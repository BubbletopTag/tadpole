#!/bin/bash
# Tadpole — Online System Update: fetch the system files from LeapFrog and
# install them, with nothing to dump off hardware first.
#
#   ./tools/online-update.sh [stage-dir]
#
# WHAT THIS REPLACES. Setting Tadpole up has always begun with "find a device,
# run LFConnect on a PC, dig its download cache out". That is a wall in front
# of anyone who has the device in a drawer and no PC software — and it is
# unnecessary, because LFConnect fetches those packages from a public server
# and so can we.
#
#     https://digitalcontent.leapfrog.com/packages/<middle>/<PackageID>.lf2
#
# THE FIRMWARE IS THE EXCEPTION and it is worth knowing why, because probing
# it the obvious way says "not there". Content lives under the middle field of
# its ID; the firmware lives under a per-DEVICE directory named in
# OpenLFConnect's profile — for the LeapPad2, PADFW — and is a .lfp:
#
#     [names]    LF_URL:PADFW
#     [packages] FIRMWARE:PAD2-0x00220004-000000.lfp
#
# WHAT IT DOES NOT GET. Two things, and neither is a bug:
#
#   * "Firmware-BulkEmpty" is exactly what its name says — a 15 MB UBI volume
#     of zeros that compresses to 19 KB. /LF/Bulk is populated by the CONTENT
#     packages, not by the firmware, which is why fetching only the firmware
#     produced a system with an empty home screen.
#   * .lf3 packages are encrypted and Tadpole ships no key. They are skipped
#     unless keys/lf3.keys exists; everything else installs regardless.
#
# The download lands in a staging directory laid out the way LFConnect leaves
# its cache, and then install-firmware.sh takes over — so this shares every
# step that already worked: finding Firmware-Base, extracting the UBIFS
# volume, building the sysroot, installing content.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
. "$HERE/lib-deps.sh"

STAGE="${1:-$PROJ/sources/online-update}"
CACHE="$STAGE/cache"

PY="$(tad_python || true)"
[ -n "$PY" ] || {
    echo "no python3 available to download with." >&2
    echo "  ./tools/fetch-deps.sh   stages one into build/deps" >&2
    exit 1
}

echo "==> Online System Update"
echo "    from digitalcontent.leapfrog.com"
echo

mkdir -p "$CACHE" || { echo "cannot write to $CACHE" >&2; exit 1; }

# Reachability first, and say so plainly. "Connection refused" three minutes
# into a download reads as a broken emulator; said up front it reads as no
# internet, which is what it is.
if ! "$PY" - <<'PY'
import socket, sys, urllib.request
try:
    urllib.request.urlopen("https://digitalcontent.leapfrog.com/packages/", timeout=20)
except Exception as e:
    code = getattr(e, "code", None)
    # A 403 or 404 still proves the host answered.
    sys.exit(0 if code else 1)
PY
then
    echo "cannot reach digitalcontent.leapfrog.com." >&2
    echo "  Check the network and try again; nothing has been changed." >&2
    exit 1
fi

echo "==> downloading packages"
"$PY" "$HERE/fetch-firmware.py" --get all -o "$CACHE" || {
    echo "download failed; nothing has been installed." >&2
    exit 1
}

echo
echo "==> installing"
exec "$HERE/install-firmware.sh" "$STAGE"
