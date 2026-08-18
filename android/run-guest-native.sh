#!/system/bin/sh
# Run the guest's OWN ARM32 code on a 32-bit Android device — no emulator.
#
#     adb root && adb push android/run-guest-native.sh /data/local/tmp/
#     adb shell sh /data/local/tmp/run-guest-native.sh
#
# NEEDS ROOT, and that is the one real limitation. Everything else that
# NOTES-arm32.md lists against "Option A" turns out to be about running the
# guest from INSIDE the app process; from a root shell the objections come
# apart. There is no JIT here and no address-space reservation, so dynarmic's
# missing 32-bit backend is irrelevant; chroot does the sysroot path rewriting
# that glasspole's syscall.cpp does in software, because the kernel is already
# very good at it; and SELinux's ban on an app executing a file it wrote is a
# rule about untrusted_app, not about the kernel.
#
# Measured on a Phh-Treble GSI, Android 8.1, armeabi-v7a only, Mali-T720,
# kernel 3.18.79 armv7l: AppManager boots, the LeapPad home screen draws through
# the viewer, taps on the tablet's own touchscreen reach the guest, and a title
# launches from the menu.
#
# The one piece of code this needed is shim/tadpole_mqueue.c — see its header.
#
# Every step below exists because of something measured; the notes say what.
set -e
PKG=org.tadpole.view
# THE PATH HAS TO BE SPELLED THE WAY THE SYSROOT'S SYMLINKS SPELL IT.
# /data/user/0 is a symlink to /data/data, so outside the chroot either works —
# but push-firmware.sh re-pointed every link in the sysroot at
# /data/user/0/<pkg>/files/rootfs/..., and inside the chroot there is no
# /data/user symlink to follow. Bind at /data/data and /bin resolves to a path
# that does not exist in there:  chroot: exec /bin/busybox: No such file.
F=/data/user/0/$PKG/files
S=$F/runtime/sysroot

# 1. THE SYSROOT IS THE CHROOT ROOT, NOT rootfs/ubi_rfs. The raw firmware tree
#    has no /sys, and libDisplay.so stats /sys/class/graphics, gets ENOENT and
#    dereferences NULL. setup-sysroot.sh already synthesises that tree.
# 2. The sysroot's top-level entries are ABSOLUTE symlinks into $F/rootfs/...,
#    so $F has to appear at its own absolute path inside the chroot for them to
#    resolve. Guarded, because a second bind stacks and the stack cannot be
#    unpicked with toybox's umount.
# NO procfs ON $S/proc: the sysroot's /proc is synthesised, and a real one
# hides proc/mtd, which CMfgData::Init needs or it segfaults. See the long note
# in android/native-helper.sh.
mkdir -p "$S$F"
mountpoint -q "$S$F" || mount -o bind "$F" "$S$F"

# 3. Start the guest. LD_LIBRARY_PATH order matters: the shims must be found
#    before the real libraries they stand in front of.
rm -f "$F/.lock" /data/local/tmp/am.log
nohup chroot "$S" /bin/busybox env \
    LD_LIBRARY_PATH=$F/runtime/shimlibs-gl:$F/runtime/shimlibs-z:$F/runtime/shimlibs:$F/runtime/libs \
    TADPOLE_DIR=$F \
    TSLIB_CONFFILE=/nonexistent-ts.conf \
    /LF/Base/bin/AppManager > /data/local/tmp/am.log 2>&1 &

# 4. Wait for the guest to build the arena.
n=0
while [ ! -s "$F/state.bin" ] && [ $n -lt 40 ]; do sleep 1; n=$((n+1)); done
[ -s "$F/state.bin" ] || { echo "guest never created state.bin"; tail -5 /data/local/tmp/am.log; exit 1; }
sleep 6

# 5. RELABEL BEFORE THE VIEWER EVER LOOKS, AND IN ONE PASS.
#    Android gives every app's files an MLS category pair; a file created by
#    root lands as plain app_data_file:s0 and the app is denied it. Relabelling
#    one file at a time loses the race — the viewer polls try_map() every frame,
#    and try_map() latches: once state.bin maps it returns early for ever, even
#    if fb0.bin failed. That is a mapped arena with no pixels: correct rotation,
#    working audio, on-screen pad, black picture.
CTX=$(ls -Zd "$F/glcmd.bin" 2>/dev/null | cut -d' ' -f1)
[ -n "$CTX" ] || CTX=u:object_r:app_data_file:s0:c512,c768
for f in "$F"/*; do
    case "$f" in *rootfs|*runtime) continue;; esac
    chcon "$CTX" "$f" 2>/dev/null || true
done
echo "relabelled to $CTX"

# 6. Now the viewer, which will map a complete, readable arena on its first try.
am start -n $PKG/.TadpoleActivity >/dev/null 2>&1
echo "guest pid: $(pgrep AppManager 2>/dev/null || echo '?')"
