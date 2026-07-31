#!/bin/bash
# Run a LeapPad2 ARM binary under qemu-user against the stock rootfs.
#
#   ./run.sh                        -> AppManager (the system UI)
#   ./run.sh /bin/busybox sh        -> interactive ARM shell
#   ./run.sh /usr/bin/read_button a -> any guest binary
#
# Requires: qemu-arm (Arch: qemu-user)

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
ROOTFS="$PROJ/rootfs/stock-4.6.0.784/1221351650/ubi_rfs"
SYSROOT="$HERE/sysroot"
LIBS="$HERE/libs"

[ -d "$ROOTFS" ] || { echo "rootfs not found: $ROOTFS" >&2; exit 1; }
[ -d "$SYSROOT" ] || { echo "sysroot not found — see PLAN.txt 1b" >&2; exit 1; }

# WHY A FLAT LIB DIR:
# The guest libs are split across /lib, /usr/lib, /LF/Base/Brio/lib and
# /LF/Base/lib. qemu-user's -L translates absolute paths into the sysroot, but
# when a file is MISSING there it falls through to the HOST path — so the guest
# loader gets handed an x86-64 .so and dies with "is not an ELF executable for
# ARM". Any single search order loses. One flat dir of symlinks means every
# lookup hits on the first try and can never fall through.
if [ ! -d "$LIBS" ] || [ -z "$(ls -A "$LIBS" 2>/dev/null)" ]; then
    echo "building flat lib dir..." >&2
    mkdir -p "$LIBS"
    find "$ROOTFS/lib" "$ROOTFS/usr/lib" "$ROOTFS/LF/Base/Brio/lib" \
         "$ROOTFS/LF/Base/lib" "$ROOTFS/LF/Base/Flash/lib" -maxdepth 1 -name '*.so*' 2>/dev/null |
    while read -r f; do ln -sf "$f" "$LIBS/$(basename "$f")"; done
fi

BIN="${1:-$ROOTFS/LF/Base/bin/AppManager}"
[ $# -gt 0 ] && shift
# a guest-absolute path (/bin/sh) is resolved inside the rootfs
case "$BIN" in /*) [ -e "$BIN" ] || BIN="$ROOTFS$BIN" ;; esac

cd "$SYSROOT"
# -s 64MB: the default 8MB main stack overflows in Brio/Flash Lite. See
# docs/STATUS.md.
exec qemu-arm -s 67108864 -L "$SYSROOT" -E LD_LIBRARY_PATH="$LIBS" \
     -E TADPOLE_SYSROOT="$SYSROOT" "$BIN" "$@"
