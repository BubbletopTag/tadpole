#!/bin/bash
# Tadpole — install a LeapTV firmware from the "DONUT" recovery image.
#
#   ./tools/install-leaptv-donut.sh DONUT.zip
#   ./tools/install-leaptv-donut.sh <directory holding RFS, BULK, FAT32>
#
# WHAT DONUT IS. The LeapTV's recovery partition carries the payload its
# surgeon writes to the eMMC: RFS (the root filesystem), BULK (/LF/Bulk, with
# the bundled titles) and FAT32 (the kernel), each a plain GNU tar. That is
# the same shape as the LeapPad3's firmware package, whose RFS is a tar too —
# only there is no .lfp around it, which is why install-firmware.py does not
# take it and this script does.
#
# WHAT THIS LAYS OUT, next to every other device's firmware:
#
#   rootfs/leaptv-<version>/emmc_rfs    the root, as install-firmware.py names
#                                       a tar-based one (setup-sysroot.sh's
#                                       globs know emmc_rfs)
#   rootfs/leaptv-<version>/bulk        /LF/Bulk, mirrored into the sysroot by
#                                       setup-sysroot.sh
#   rootfs/leaptv-<version>/fat         uImage, kept for reference only
#
# then runs setup-sysroot.sh for the device.
set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
SRC="${1:-}"
[ -n "$SRC" ] || { echo "usage: $0 DONUT.zip | <dir with RFS BULK FAT32>" >&2; exit 2; }

# One reader for both forms: a member streamed out of the zip, or a file.
member() {
    if [ -d "$SRC" ]; then cat "$SRC/$1"; else unzip -p "$SRC" "$1"; fi
}

# tar exits 2 over the device nodes it cannot create as an ordinary user, and
# nothing in a sysroot needs them — the shim answers for /dev. Anything worse
# shows up as a missing Firmware/meta.inf below.
untar_into() {
    mkdir -p "$2"
    member "$1" | tar -x -C "$2" --no-same-owner --no-same-permissions 2>&1 \
        | grep -v -E 'Cannot mknod|previous errors' || true
}

TMP="$PROJ/rootfs/.leaptv-donut.tmp"
rm -rf "$TMP"
echo "==> RFS (the root filesystem, ~250 MB)"
untar_into RFS "$TMP/emmc_rfs"
META="$TMP/emmc_rfs/Firmware/meta.inf"
[ -r "$META" ] || { echo "no Firmware/meta.inf came out of RFS — not a DONUT image?" >&2; exit 1; }
DEV="$(sed -n 's/^Device="\([^"]*\)".*/\1/p' "$META" | head -1)"
VER="$(sed -n 's/^Version="\([^"]*\)".*/\1/p' "$META" | head -1)"
[ "$DEV" = LeapTV ] || { echo "RFS says Device=\"$DEV\", not LeapTV" >&2; exit 1; }
echo "    $DEV firmware $VER"

echo "==> BULK (/LF/Bulk, ~850 MB — this takes a while)"
untar_into BULK "$TMP/bulk"
echo "==> FAT32 (the kernel)"
untar_into FAT32 "$TMP/fat"

DEST="$PROJ/rootfs/leaptv-$VER"
if [ -e "$DEST" ]; then
    echo "==> $DEST exists — leaving it alone (delete it to re-extract)"
    rm -rf "$TMP"
else
    mv "$TMP" "$DEST"
    echo "==> installed to $DEST"
fi

echo "==> building the sysroot"
exec "$PROJ/runtime/setup-sysroot.sh" leaptv
