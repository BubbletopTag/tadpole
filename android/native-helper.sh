#!/system/bin/sh
# The root half of "Run System Menu" on a 32-bit Android device.
#
#     adb root
#     adb push android/native-helper.sh /data/local/tmp/
#     adb shell 'nohup sh /data/local/tmp/native-helper.sh >/dev/null 2>&1 &'
#
# WHY THERE HAS TO BE A ROOT HALF AT ALL. On this ABI there is no ARM engine —
# glasspole is arm64-only — and the guest's own code runs natively instead, which
# needs chroot (CAP_SYS_CHROOT) and needs to execute files in the app's data
# directory. SELinux forbids untrusted_app the second of those absolutely: the
# platform probe measures it every launch and prints
#
#     probe: exec from app files   DENIED (execve refused — SELinux/noexec)
#
# and no amount of root elsewhere changes what the app's own domain may do. `su`
# is not a way round it either — measured, the app gets "Permission denied"
# reaching for /system/xbin/su. So the app asks, and something already running as
# root does the work. See android/NOTES-arm32.md, "Amended".
#
# THE PROTOCOL IS A FILE, following the one the viewer already uses for
# screenshots (write a path into $TADPOLE_DIR/shot.req and the next frame lands
# there). $TADPOLE_DIR/guest.req holds one line:
#
#     boot            run the system menu
#     run <path>      run one .swf or binary
#     stop            kill whatever is running
#
# and the helper answers by writing the guest's pid into $TADPOLE_DIR/.lock,
# which is exactly where guest_external() in the viewer already looks.
PKG=${PKG:-org.tadpole.view}
# Spelled /data/user/0 because that is how push-firmware.sh re-pointed every
# symlink in the sysroot, and inside the chroot there is no /data/user symlink
# to follow. Bind at /data/data and /bin resolves to a path that is not there.
F=/data/user/0/$PKG/files
S=$F/runtime/sysroot
REQ=$F/guest.req
LOCK=$F/.lock
PIDF=$F/helper.pid

[ "$(id -u)" = "0" ] || { echo "native-helper: needs root (adb root)" >&2; exit 1; }
[ -d "$S" ] || { echo "native-helper: no sysroot at $S — run push-firmware.sh" >&2; exit 1; }

# The app's own SELinux context, which every file it has to read must carry.
# Android gives each app a category pair; a file created by ROOT lands as plain
# app_data_file:s0 without them and the app is denied its own framebuffer. Taken
# from a file the app itself made rather than hardcoded, because the categories
# differ per install.
app_ctx() {
    c=$(ls -Zd "$F/tadpole.png" 2>/dev/null | cut -d' ' -f1)
    [ -n "$c" ] || c=u:object_r:app_data_file:s0:c512,c768
    echo "$c"
}
CTX=$(app_ctx)

mounts() {
    mkdir -p "$S$F" "$S/proc"
    # TESTED BY WHETHER THE CHROOT RESOLVES, NOT BY WHETHER SOMETHING IS
    # MOUNTED. `mountpoint -q` answered yes on a bind the guest could not see
    # through, and the launch then died on
    #
    #     chroot: exec /bin/busybox: No such file or directory
    #
    # which is the one error that says nothing about which mount is missing:
    # /bin is a symlink into $F, so a bind that is present but not serving the
    # tree looks exactly like a firmware that was never pushed. Asking the
    # question the guest asks cannot be wrong about it.
    #
    # It also cannot stack, and stacking is unrecoverable here: a second bind on
    # top of a working one is invisible to mountpoint, and toybox's umount
    # answers EINVAL for every spelling of the path afterwards. The only way
    # back from that is a reboot.
    # ASKED THROUGH THE CHROOT, because that is the only place the question
    # means anything. $S/bin is an ABSOLUTE symlink into $F, so testing
    # $S/bin/busybox from out here follows it to the real file and succeeds
    # whether or not the bind exists — the guard passed, the mount was skipped,
    # and the launch died on the error above. Inside, the same path can only
    # resolve through the bind.
    chroot "$S" /bin/busybox true 2>/dev/null || mount -o bind "$F" "$S$F"
    [ -d "$S/proc/self" ] || mount -t proc proc "$S/proc"
    chroot "$S" /bin/busybox true 2>/dev/null || {
        echo "native-helper: /bin does not resolve inside $S — is the firmware pushed?" >&2
        return 1
    }
    return 0
}

relabel() {
    # Everything the guest just made, in ONE pass. One pass matters: try_map()
    # in the viewer latches — once state.bin maps it returns early for ever,
    # even if fb0.bin failed — so relabelling file by file loses the race to a
    # 60 fps poll and leaves a mapped arena with no pixels. The symptom is
    # correct rotation, working audio, an on-screen pad and a black picture.
    for f in "$F"/*; do
        case "$f" in *rootfs|*runtime) continue;; esac
        chcon "$CTX" "$f" 2>/dev/null
    done
}

kill_guest() {
    [ -s "$LOCK" ] || return 0
    p=$(cat "$LOCK" 2>/dev/null)
    rm -f "$LOCK"
    [ -n "$p" ] || return 0
    kill -TERM "$p" 2>/dev/null || return 0
    n=0
    while kill -0 "$p" 2>/dev/null && [ $n -lt 20 ]; do sleep 0.1; n=$((n+1)); done
    kill -KILL "$p" 2>/dev/null
    # VideoDaemon daemonises into its own session and outlives a kill aimed at
    # the group, which is the same straggler tadpole.sh sweeps on the desktop.
    pkill -KILL VideoDaemon 2>/dev/null
    return 0
}

# What Graphics Settings asked for, written beside the request by the viewer.
#
# WITHOUT THIS EVERY TITLE RUNS ON THE SOFTWARE RASTERISER. The shim only
# encodes to the host GPU when it sees TADPOLE_GL_HLE, and a helper that builds
# its own environment has no other way to be told — which is not a subtle
# failure: the software path is deprecated, samples one texture unit and ignores
# the blend factors, so Clam Prix came out slow AND wrong.
#
# WHITELISTED TO TADPOLE_*, because this file is written by the app and read by
# something running as root. Nothing here should be able to set LD_PRELOAD.
guest_env() {
    [ -s "$F/guest.env" ] || return 0
    while IFS= read -r line; do
        case "$line" in
            TADPOLE_*=*) printf '%s ' "$line" ;;
        esac
    done < "$F/guest.env"
}

start_guest() {
    prog=$1
    mounts || return 1
    rm -f "$LOCK"
    extra=$(guest_env)
    echo "native-helper: env${extra:+ $extra}"
    # $extra is deliberately unquoted: it is a list of KEY=VALUE words for env,
    # and none of the values it is allowed to carry contains a space.
    chroot "$S" /bin/busybox env \
        LD_LIBRARY_PATH=$F/runtime/shimlibs-gl:$F/runtime/shimlibs-z:$F/runtime/shimlibs:$F/runtime/libs \
        TADPOLE_DIR=$F \
        TSLIB_CONFFILE=/nonexistent-ts.conf \
        $extra \
        "$prog" > /data/local/tmp/tadpole-guest.log 2>&1 &
    gp=$!
    # Wait for the arena before answering, so that by the time the viewer sees a
    # pid there is something for it to map.
    n=0
    while [ ! -s "$F/state.bin" ] && [ $n -lt 60 ]; do
        kill -0 "$gp" 2>/dev/null || break
        sleep 0.25; n=$((n+1))
    done
    sleep 1
    relabel
    if kill -0 "$gp" 2>/dev/null; then
        echo "$gp" > "$LOCK"
        chcon "$CTX" "$LOCK" 2>/dev/null
        echo "native-helper: guest $gp ($prog)"
    else
        echo "native-helper: guest died on startup; see /data/local/tmp/tadpole-guest.log"
        tail -3 /data/local/tmp/tadpole-guest.log
    fi
}

cleanup() { rm -f "$PIDF"; }
trap cleanup EXIT HUP INT TERM

mounts
echo "native-helper: watching $REQ (ctx $CTX)"
while :; do
    # The pid file is the viewer's "is anyone listening". Rewritten every pass
    # so its mtime is also a heartbeat: a stale one left by a reboot would
    # otherwise make the front end wait for an answer that is never coming.
    echo $$ > "$PIDF"; chcon "$CTX" "$PIDF" 2>/dev/null
    if [ -s "$REQ" ]; then
        req=$(cat "$REQ" 2>/dev/null)
        rm -f "$REQ"
        case "$req" in
            stop)  kill_guest; echo "native-helper: stopped" ;;
            boot)  kill_guest; start_guest /LF/Base/bin/AppManager ;;
            run\ *) kill_guest; start_guest "${req#run }" ;;
            *)     echo "native-helper: ignoring request '$req'" ;;
        esac
    fi
    sleep 0.25
done
