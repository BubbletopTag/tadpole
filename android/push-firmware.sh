#!/bin/sh
# Put a desktop-prepared sysroot onto the device.
#
# WHY THIS EXISTS RATHER THAN AN INSTALLER. Everything in Tadpole that installs
# firmware is a shell or Python script, and Android has neither — and the one
# step that matters most, reading the UBIFS volume a firmware image is made of,
# needs ubi_reader and LZO. tools/install-firmware.py says so itself: "writing a
# UBIFS reader is a project in itself". So until that project happens, setting
# up is a desktop job and this carries the result across.
#
#     ./android/push-firmware.sh          # sysroot + rootfs + shim libraries
#
# It moves about 1.2 GB and takes under a minute on USB 3.
#
# ### Three things that are not obvious and cost an afternoon between them
#
# 1. THE SYSROOT IS A LINK FARM, NOT A TREE. runtime/setup-sysroot.sh builds it
#    with its top-level entries as ABSOLUTE symlinks into the checkout's own
#    rootfs/ — bin, lib, sbin, usr, boot, mnt, Firmware, linuxrc. Those paths do
#    not exist on a phone, so a straight copy lands a sysroot whose every
#    binary is a dangling link and glasspole reports "cannot open /bin/busybox".
#    So rootfs/ travels too, and the links are re-pointed on the device.
#
# 2. `adb push` CANNOT CREATE SYMLINKS. It fails with "remote symlink failed:
#    Permission denied" on the first one it meets, which for this tree is
#    sysroot/boot — about four entries in. A tar preserves them, and pushing one
#    1.1 GB file is far faster than pushing twelve thousand small ones anyway.
#
# 3. IT HAS TO LAND ON INTERNAL STORAGE. /sdcard is FUSE and has no symlinks at
#    all, so the tree cannot live there either. The app's own files directory is
#    real f2fs, and `run-as` is how a debuggable app's directory is written to
#    from a shell — the shell reads the tar and run-as writes the tree, because
#    the app's uid cannot read /data/local/tmp and the shell cannot write into
#    the app.
#
# Two hardlinks fail to extract (an /etc/terminfo alias and sudo/sudoedit)
# because the app's filesystem refuses hardlinks to the shell. Neither matters
# to running a title, and tar reports them rather than hiding them.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
. "$here/env.sh"

PKG=${PKG:-org.tadpole.view}
# The DESKTOP checkout to take the firmware from.
#
# RESOLVED THROUGH THE SYSROOT ITSELF, not through runtime/. In a worktree,
# runtime/ is a real directory and runtime/sysroot inside it is the symlink to
# the main checkout — so asking `pwd -P` about runtime/ answers "this worktree",
# tar then archives the symlink rather than following it, and the result is a
# 130 KB tarball that pushes in a millisecond and contains one broken link.
# Asking the sysroot where IT is lands in the checkout that really has one.
SRC=${TADPOLE_SRC:-$(cd "$root/runtime/sysroot" && pwd -P | sed 's:/runtime/sysroot$::')}
STAGE=${TMPDIR:-/tmp}/tadpole-push.$$

FILES=/data/user/0/$PKG/files

[ -d "$SRC/runtime/sysroot" ] || {
    echo "no sysroot at $SRC/runtime/sysroot — set TADPOLE_SRC to a checkout that has one" >&2
    exit 1
}

"$ADB" get-state >/dev/null 2>&1 || { echo "no device" >&2; exit 1; }
"$ADB" shell "run-as $PKG true" >/dev/null 2>&1 || {
    echo "run-as $PKG failed — is the app installed and debuggable?" >&2
    exit 1
}

mkdir -p "$STAGE"
trap 'rm -rf "$STAGE"' EXIT

echo "=== packing (no -h: the symlinks INSIDE the tree are meaningful) ==="
tar cf "$STAGE/tp.tar" -C "$SRC/runtime" \
    sysroot shimlibs shimlibs-gl shimlibs-z
tar cf "$STAGE/rootfs.tar" -C "$SRC" rootfs
ls -lh "$STAGE"/*.tar

echo "=== pushing ==="
"$ADB" push "$STAGE/tp.tar"     /data/local/tmp/tp.tar
"$ADB" push "$STAGE/rootfs.tar" /data/local/tmp/rootfs.tar

echo "=== unpacking into $FILES ==="
"$ADB" shell "cat /data/local/tmp/tp.tar | run-as $PKG sh -c 'mkdir -p files/runtime && tar xf - -C files/runtime'" || true
"$ADB" shell "cat /data/local/tmp/rootfs.tar | run-as $PKG tar xf - -C files" || true
"$ADB" shell "rm -f /data/local/tmp/tp.tar /data/local/tmp/rootfs.tar"

echo "=== re-pointing the sysroot's absolute links at this device ==="
# ALL OF THEM, NOT JUST THE TOP LEVEL. There are 208, and only eight are the
# obvious ones in the sysroot's root — the rest are deeper: every entry of
# var/, and LF/Base, which is the one prereq_check tests for. Fixing only the
# top level leaves a sysroot that looks populated, boots nothing, and reports
# "runtime sysroot missing" while bin/ and lib/ resolve perfectly.
#
# One rule covers all of them because they share a prefix: they point into the
# checkout they were built in, either at rootfs/ or at runtime/shimlibs/. So
# the desktop project path is replaced by the app's files directory and the
# rest of the path is kept, which is exactly what setup-sysroot.sh would have
# written had it run here.
"$ADB" shell "run-as $PKG sh -c '
F=$FILES
OLD=$SRC
cd \$F || exit 1
n=0
for l in \$(find runtime/sysroot -type l); do
  t=\$(readlink \"\$l\")
  case \"\$t\" in
    \$OLD/*) rm -f \"\$l\"; ln -s \"\$F/\${t#\$OLD/}\" \"\$l\"; n=\$((n+1));;
  esac
done
echo \"re-pointed \$n of \$(find runtime/sysroot -type l | wc -l) symlinks\"
'"

echo "=== proving it: ARM32 guest code, on this phone ==="
"$ADB" shell "run-as $PKG sh -c 'cd $FILES && ./glasspole/build/glasspole --sysroot runtime/sysroot /bin/busybox echo GUEST-OK'" \
    || echo "the guest did not run — is an engine linked? see linkEngine() in TadpoleActivity"
