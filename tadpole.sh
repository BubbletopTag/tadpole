#!/bin/bash
# Tadpole — LeapPad2 emulator. One command to run it.
#
#   ./tadpole.sh                 open the Tadpole front end (nothing runs yet;
#                                use File -> Run System Menu, or the wizard)
#   ./tadpole.sh --boot          open it AND start the system menu straight away
#   ./tadpole.sh --shell         ARM shell inside the guest, no viewer
#   ./tadpole.sh --logo          draw the boot logo (quick "is it alive" test)
#   ./tadpole.sh --app NAME      launch an installed app by name or PackageID
#   ./tadpole.sh --list           list installed apps
#   ./tadpole.sh --run PROG ...  run any guest binary with the viewer up
#   ./tadpole.sh --no-viewer     skip the SDL window
#   ./tadpole.sh --debug         verbose shim logging
#   ./tadpole.sh --touch-debug   red crosshair where the viewer thinks you tapped
#   TADPOLE_GL=0 ./tadpole.sh    use the stock GPU stack (titles will assert)
#   ./tadpole.sh -r 90 ...       rotate the display (portrait titles)
#
# Viewer controls:  arrows = D-pad, Z/X = A/B, Home = menu, Esc = back,
#                   mouse click/drag = stylus, Ctrl+Q = quit.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOTFS="$HERE/rootfs/stock-4.6.0.784/1221351650/ubi_rfs"
SYSROOT="$HERE/runtime/sysroot"
# Two shim variants, because targets link different libraries: AppManager and
# VideoDaemon pull in libdl.so.0, while the display tools (imager-fb etc.) link
# libz.so.1 and no libdl at all. Both dirs are on the path; whichever the
# binary actually loads provides the interception.
# TADPOLE_GL puts our EGL/GLES1 shims ahead of the stock ones.
#
# ON BY DEFAULT since the software rasteriser became usable: without it every
# native Brio title dies at startup, because the stock libEGL asserts when it
# cannot open /dev/vmem and /dev/ogl_vr5:
#     ERROR src/egl/vr5_platform_fbdev.cpp line 391
#     The memory device has not been opened. <ASSERT>: eglInitialize() failed
# Set TADPOLE_GL=0 to go back to the stock stack.
#
# The viewer's Options -> Graphics checkbox has to work no matter how the
# emulator was started, so when the variable is not already set we read the
# front end's config. Otherwise `./tadpole.sh` silently ignored the checkbox and
# the title asserted, while `TADPOLE_GL=1 ./tadpole.sh` worked — which is
# exactly the confusing pair of behaviours that got reported.
UICFG="${XDG_CONFIG_HOME:-$HOME/.config}/tadpole/ui.cfg"
if [ -z "${TADPOLE_GL:-}" ] && [ -r "$UICFG" ]; then
    TADPOLE_GL="$(awk '$1=="gl"{print $2}' "$UICFG" | tail -1)"
fi
: "${TADPOLE_GL:=1}"
export TADPOLE_GL

LIBS="$HERE/runtime/shimlibs-z:$HERE/runtime/shimlibs:$HERE/runtime/libs"
[ "$TADPOLE_GL" != 0 ] && LIBS="$HERE/runtime/shimlibs-gl:$LIBS"
VIEWER="$HERE/tadpole/viewer/tadpole-view"
export TADPOLE_DIR="${TADPOLE_DIR:-/tmp/tadpole}"

use_viewer=1; debug=0; mode=front; scale=2; rotate=0; prog=""; appname=""; declare -a progargs=()
while [ $# -gt 0 ]; do
    case "$1" in
        --boot)      mode=ui ;;          # start the system menu immediately
        --shell)     mode=shell; use_viewer=0 ;;
        --logo)      mode=logo ;;
        --run)       mode=run; shift; prog="${1:-}" ;;
        --app)       mode=app; shift; appname="${1:-}" ;;
        --list)      mode=list; use_viewer=0 ;;
        --no-viewer) use_viewer=0 ;;
        --debug)     debug=1 ;;
        # Draw a red crosshair at the framebuffer point the viewer computed
        # from your click, and log the mapping. The cross is drawn INTO the
        # guest's pixel buffer, so it is rotated and scaled by exactly the same
        # path as the content: if it appears under your pointer the
        # window->framebuffer mapping is correct and any offset you see is
        # further down the stack; if it does not, the error is right there to
        # measure.
        --touch-debug) export TADPOLE_TOUCH_DEBUG=1 ;;
        -s)          shift; scale="${1:-2}" ;;
        -r)          shift; rotate="${1:-0}" ;;   # 90 for portrait titles
        --)          shift; progargs=("$@"); break ;;
        *)           progargs+=("$1") ;;
    esac
    shift || true
done

for f in "$ROOTFS" "$SYSROOT"; do
    [ -e "$f" ] || { echo "missing: $f" >&2
                     echo "run runtime/setup-sysroot.sh first" >&2; exit 1; }
done
[ -e "$HERE/runtime/shimlibs/libdl.so.0" ] || {
    echo "shim not built — run: cd tadpole && make" >&2; exit 1; }

# /tmp is a tmpfs on the real device ("/dev/ram0 /tmp tmpfs" in /proc/mounts),
# so it starts EMPTY every boot. Ours is a real directory that persists, which
# means flags from a previous session survive — including /tmp/shutdown and
# /tmp/restart, which make AppManager exit immediately instead of booting.
# Wipe it and re-create only the boot-time contents.
if [ -d "$SYSROOT/tmp" ]; then
    rm -rf "$SYSROOT/tmp"/* 2>/dev/null || true
    printf '1\n'                        > "$SYSROOT/tmp/bulk_ready"
    printf '0'                          > "$SYSROOT/tmp/splash"
    : >                                   "$SYSROOT/tmp/initial"
    # Cartridge state. This file is written by /sbin/cnotify (format "%d, %s")
    # and read by Brio's CartridgeTask, which otherwise learns about carts over
    # /tmp/cart_events_socket from mdev. We have no mdev, so we set it here.
    #
    # The enum is POSITIONAL, in the order the strings appear in cnotify:
    #   0 NONE  1 INSERTED  2 DRIVER_READY  3 READY  4 REMOVED
    #   5 FS_CLEAN  6 CLEAN  7 REINSERT  8 RESTART_APPMANAGER  9 REBOOT
    #
    # We used to hardcode 7 (REINSERT) — i.e. we were telling the UI "that
    # cartridge is bad, take it out and put it back in" on every single boot,
    # which is exactly what the home screen's cart tile then displayed.
    # Report READY when /LF/Cart has contents, NONE when it does not — the same
    # two states a real device reaches via cartridge.sh's `cnotify 3` / no cart.
    if [ -n "$(ls -A "$SYSROOT/LF/Cart" 2>/dev/null)" ]; then
        printf '3, CARTRIDGE_STATE_READY' > "$SYSROOT/tmp/cart_brio_state"
    else
        printf '0, CARTRIDGE_STATE_NONE'  > "$SYSROOT/tmp/cart_brio_state"
    fi
fi
# A stale audio.fmt from a previous run would make the viewer open its device
# with the wrong rate before the guest has negotiated anything.
rm -f "$TADPOLE_DIR/audio.fmt"

# Stranded atomic temp files confuse the next run's reads.
find "$SYSROOT/LF/Bulk" -name '*.atomic' -delete 2>/dev/null || true

mkdir -p "$TADPOLE_DIR"

# Every instance shares TADPOLE_DIR (framebuffers, state, input FIFOs), so two
# concurrent runs silently corrupt each other's display — you end up looking at
# another title's pixels and drawing wrong conclusions. Refuse to start a
# second guest on the same dir.
LOCK="$TADPOLE_DIR/.lock"
if [ -e "$LOCK" ] && kill -0 "$(cat "$LOCK" 2>/dev/null)" 2>/dev/null; then
    echo "tadpole: another instance (pid $(cat "$LOCK")) is using $TADPOLE_DIR" >&2
    echo "  stop it first, or run with a separate dir:" >&2
    echo "    TADPOLE_DIR=/tmp/tadpole-2 $0 ..." >&2
    exit 1
fi
echo $$ > "$LOCK"
cleanup_lock() { rm -f "$LOCK"; }
trap cleanup_lock EXIT

guest() {
    local bin="$1"; shift
    case "$bin" in /*) [ -e "$bin" ] || bin="$ROOTFS$bin" ;; esac
    ( cd "$SYSROOT"
      # -s 64MB: qemu-user's default 8MB main stack is not enough. Brio and
      # Flash Lite recurse deeply through their scene graph and printf-family
      # calls have large frames; with the default you get a SIGSEGV faulting
      # on "str r1, [sp]" with sp exactly 8MB below the stack base.
      # TSLIB IS DISABLED BY DEFAULT.
      # tslib's module chain (input->pthres->variance->dejitter->linear) has a
      # null ops->read once a touch event actually arrives here, so the first
      # tap crashes with "blx r3" through a null pointer. Brio has its own
      # touchscreen path and says so — "Falling back on touchscreen interface"
      # — and that path handles taps correctly. Point TSLIB_CONFFILE at a
      # nonexistent file so tslib init fails cleanly and Brio uses its own.
      # Set TADPOLE_TSLIB=1 to re-enable tslib (expect the tap crash).
      # TADPOLE_STRACE=1 prints every GUEST SYSCALL. Needed because the shim
      # only sees open/fopen — stat, access and opendir bypass it, so "the
      # guest never opened X" is NOT a conclusion the shim log can support.
      exec qemu-arm -s 67108864 -L "$SYSROOT" ${TADPOLE_STRACE:+-strace} \
           ${TADPOLE_TSLIB:+-E TSLIB_REAL=1} \
           ${TADPOLE_TSLIB:--E TSLIB_CONFFILE=/nonexistent-ts.conf} \
           -E LD_LIBRARY_PATH="$LIBS" \
           -E TADPOLE_DIR="$TADPOLE_DIR" \
           -E TADPOLE_SYSROOT="$SYSROOT" \
           ${debug:+-E TADPOLE_DEBUG=$debug} \
           "$bin" "$@" )
}

# The shim creates the framebuffer and state files on first open, and the
# viewer must be able to map them before it starts. Draw the boot logo to do
# it — the real device does exactly this from rcS, so it doubles as a splash.
# (Must be a binary that loads a shim variant: imager-fb pulls in libz.)
if [ ! -e "$TADPOLE_DIR/state.bin" ]; then
    guest /usr/bin/imager-fb /dev/fb0 /var/screens/Valencia-Boot-logoCW.png \
        >/dev/null 2>&1 || true
fi

viewer_pid=""
if [ "$use_viewer" = 1 ] && [ -x "$VIEWER" ]; then
    "$VIEWER" -s "$scale" -r "$rotate" -d "$TADPOLE_DIR" &
    viewer_pid=$!
    trap 'kill $viewer_pid 2>/dev/null' EXIT INT TERM
    sleep 0.5
fi

PF="$SYSROOT/LF/Bulk/ProgramFiles"

# Each package's meta.inf names its entry point in AppSo. It is NOT always
# "Main.swf" — Pet Pad uses lowercase "main.swf", and several apps are native
# Brio .so files rather than Flash. Read it rather than guessing.
list_apps() {
    for d in "$PF"/*/; do
        [ -f "$d/meta.inf" ] || continue
        n=$(grep -oE 'Name="[^"]*"'  "$d/meta.inf" | head -1 | cut -d\" -f2)
        a=$(grep -oE 'AppSo="[^"]*"' "$d/meta.inf" | head -1 | cut -d\" -f2)
        case "$a" in
            *.swf) kind="flash" ;;
            *.so)  kind="native (needs AppManager)" ;;
            *)     kind="unknown" ;;
        esac
        printf "  %-28s %-22s %-14s %s\n" "$(basename "$d")" "$n" "$a" "$kind"
    done
}

case "$mode" in
    list)
        printf "  %-28s %-22s %-14s %s\n" PACKAGE NAME ENTRY KIND
        list_apps
        exit 0 ;;
    app)
        found=""
        for d in "$PF"/*/; do
            [ -f "$d/meta.inf" ] || continue
            n=$(grep -oE 'Name="[^"]*"' "$d/meta.inf" | head -1 | cut -d\" -f2)
            b=$(basename "$d")
            case "$b" in *"$appname"*) found="$d"; break ;; esac
            case "$n" in *"$appname"*) found="$d"; break ;; esac
        done
        [ -n "$found" ] || { echo "no app matching '$appname'. try --list" >&2; exit 1; }
        entry=$(grep -oE 'AppSo="[^"]*"' "$found/meta.inf" | head -1 | cut -d\" -f2)
        name=$(grep -oE 'Name="[^"]*"'  "$found/meta.inf" | head -1 | cut -d\" -f2)
        [ -n "$entry" ] || { echo "no AppSo in $found/meta.inf" >&2; exit 1; }
        [ -e "$found/$entry" ] || { echo "entry '$entry' missing in $found" >&2; exit 1; }
        guestpath="/LF/Bulk/ProgramFiles/$(basename "$found")/$entry"
        case "$entry" in
            *.swf)
                echo "=== $name — $guestpath ==="
                guest /LF/Base/Flash/bin/saplayer "$guestpath" ;;
            *)
                echo "$name uses a native entry point ($entry)." >&2
                echo "Native Brio apps need AppManager, which is not up yet." >&2
                exit 1 ;;
        esac ;;
    shell)
        guest /bin/busybox sh ;;
    logo)
        guest /usr/bin/imager-fb /dev/fb0 /var/screens/Valencia-Boot-logoCW.png
        echo "logo drawn to $TADPOLE_DIR/fb0.bin"
        [ -n "$viewer_pid" ] && { echo "viewer showing it; Ctrl+C to stop"; wait $viewer_pid; } ;;
    run)
        guest "$prog" ${progargs[@]+"${progargs[@]}"} ;;
    ui)
        # rcS launches VideoDaemon alongside AppManager; AppManager connects to
        # it over /tmp/video_events_socket.
        guest /LF/Base/bin/VideoDaemon 750 >/dev/null 2>&1 &
        sleep 1
        echo "=== AppManager ==="
        guest /LF/Base/bin/AppManager ;;
    front)
        # FRONT END ONLY — nothing boots until the user asks for it.
        #
        # Starting the guest automatically made the emulator feel like a script
        # rather than an application, and it meant a first-time user with no
        # firmware installed got a wall of errors instead of a setup screen. The
        # viewer now owns that decision: File -> Run System Menu, or the wizard
        # it shows when the system files are missing.
        if [ -n "$viewer_pid" ]; then
            wait "$viewer_pid"
        else
            echo "viewer not built — run: cd tadpole && make viewer" >&2
            exit 1
        fi ;;
esac
