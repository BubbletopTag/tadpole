#!/bin/bash
# Cross-compile glconform into a guest ARM binary that runs on BOTH sides.
#
#   ./tools/glconform/build.sh          -> runtime/glconform
#
# THE LINK IS THE INTERESTING PART, so read this before changing it.
#
# It links against the DEVICE'S OWN libraries out of rootfs/, not against our
# shims in runtime/shimlibs-gl. That is deliberate and it is what makes one
# binary work in both places:
#
#   * These firmware libraries carry NO SONAME and NO DT_NEEDED of their own —
#     checked, not assumed — so DT_NEEDED records exactly the filenames passed
#     here, and every one of those filenames exists on the device AND in
#     runtime/shimlibs-gl. Which implementation answers is then decided purely
#     by LD_LIBRARY_PATH at load time.
#   * Linking against our shims instead would bake in their SONAMEs
#     (libEGL.so.1, libGLESv1_CM.so.1). Neither of those files exists on the
#     device — it has libEGL.so.1.4 and libGLESv1_CM.so.1.1 with no unversioned
#     .1 links — so the binary would refuse to load on hardware, which is half
#     the point of building it.
#   * libopengles_lite.so is FIRST in the list on purpose. The loader resolves
#     in DT_NEEDED order, so under the emulator our gl* definitions win over the
#     stock libvr5.so that comes later in the same list. Same trick the shim
#     itself uses (see tadpole/Makefile).
#   * The six libraries after libvr5 are the DEPENDENCY CLOSURE of the stock GL
#     stack, computed rather than guessed: libGLESv1_CM/libEGL/libvr5 between
#     them leave 118 symbols undefined and declare no dependencies at all to
#     satisfy them, so the executable has to. Resolving that set greedily lands
#     on exactly libc, libstdc++, libgcc_s, libm, libpthread and libdl, with
#     only the weak _Jv_RegisterClasses left over. AppManager gets away without
#     listing them because something else in its process already pulled them in;
#     a standalone binary does not.
#
#     uClibc binds the PLT lazily, so a missing one does NOT fail at load — the
#     binary runs, prints a few lines, and dies mid-test with "can't resolve
#     symbol 'pthread_create'". If a hardware run stops partway with a message
#     like that, this list is what to extend.
#
# Same target as tadpole/Makefile's shim: armv7, softfp. NOT armhf — see the
# Tag_ABI_VFP_args comment there.

set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$(dirname "$HERE")")"
cd "$PROJ"

CLANG=${CLANG:-clang}
ARMFLAGS="--target=armv7-unknown-linux-gnueabi -march=armv7-a -mfpu=vfp -mfloat-abi=softfp"
OUT="$PROJ/runtime/glconform"

# Same discovery tadpole.sh does — whatever install-firmware.sh extracted, under
# whatever version the user's own device shipped.
ROOTFS=""
for cand in rootfs/*/emmc_rfs rootfs/*/ubi_rfs rootfs/*/*/ubi_rfs; do
	[ -d "$cand" ] || continue
	ROOTFS="$PROJ/$cand"; break
done
if [ -z "$ROOTFS" ]; then
	echo "no rootfs/*/{ubi,emmc}_rfs found — run tools/install-firmware.sh first" >&2
	exit 1
fi

USRLIB="$ROOTFS/usr/lib"
LIB="$ROOTFS/lib"
for f in "$USRLIB/libopengles_lite.so" "$USRLIB/libEGL.so" "$LIB/libc.so.0"; do
	[ -e "$f" ] || { echo "missing $f" >&2; exit 1; }
done

# --allow-shlib-undefined: the device's libGLESv1_CM has unresolved references
# to its own driver internals and declares no dependencies. They resolve at
# RUNTIME from libvr5/libEGL, which are in the NEEDED list below; the linker
# cannot see that and would otherwise refuse.
"$CLANG" $ARMFLAGS -O2 -Wall -Wextra -nostdlib -fno-builtin -fno-stack-protector \
	-fuse-ld=lld -Wl,--allow-shlib-undefined \
	-Wl,--dynamic-linker=/lib/ld-uClibc.so.0 \
	-o "$OUT" "$HERE/glconform.c" \
	-L"$USRLIB" -L"$LIB" \
	-l:libopengles_lite.so -l:libEGL.so -l:libvr5.so \
	-l:libstdc++.so.6 -l:libgcc_s.so.1 -l:libm.so.0 \
	-l:libpthread.so.0 -l:libdl.so.0 -l:libc.so.0

chmod +x "$OUT"
echo "built $OUT"
echo -n "  NEEDED: "
readelf -d "$OUT" | sed -n 's/.*NEEDED.*\[\(.*\)\]/\1/p' | tr '\n' ' '
echo
