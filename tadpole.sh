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
# The bundled qemu-arm if Tadpole has one, the host's otherwise. A user who
# installed qemu-user themselves keeps working; a user who did nothing at all
# gets the static copy that ships with the AppImage.
PROJ="$HERE"
. "$HERE/tools/lib-deps.sh"
QEMU="$(tad_qemu || true)"
if [ -z "$QEMU" ]; then
    echo "tadpole: no qemu-arm." >&2
    echo "  ./tools/fetch-deps.sh   stages one into build/deps (installs nothing)" >&2
    echo "  or install your distribution's qemu-user package" >&2
    exit 1
fi
# DISCOVER the rootfs rather than hardcoding one firmware version. Whatever
# install-firmware.sh extracted lands under rootfs/<version>/…/ubi_rfs, and the
# version is whatever the user's own device shipped with.
ROOTFS=""
for cand in "$HERE"/rootfs/*/ubi_rfs "$HERE"/rootfs/*/*/ubi_rfs; do
    [ -d "$cand" ] || continue
    ROOTFS="$cand"; break
done
: "${ROOTFS:=$HERE/rootfs/MISSING/ubi_rfs}"
# WHICH GUEST FILESYSTEM TO RUN. Normally this checkout's own, but an override
# lets a freshly built viewer drive a DIFFERENT install's content — the games
# and profile in ~/.local/share/tadpole, say — without copying either into the
# other. That is exactly what an A/B capture needs: one variable changed, and
# the thing being measured is the code, not the content.
SYSROOT="${TADPOLE_SYSROOT:-$HERE/runtime/sysroot}"

# WHICH DEVICE. Screen geometry, the splash image and — the part that actually
# differs — WHICH PROGRAM IS THE SYSTEM MENU all come from the profile that
# matches the installed firmware. A LeapPad2 boots AppManager; a LeapPad Ultra
# boots AppServer, a Qt application, and keeps AppManager only for cartridges.
# See runtime/device.sh.
. "$HERE/runtime/device.sh"
tad_load_device || exit 1
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

# HLE — HOST-GPU REPLAY — ON BY DEFAULT, read from the same config file.
#
# This used to be opt-in, and the only thing that set it was tools/probe-race.sh.
# A normal launch therefore started the viewer, announced HLE, and then quietly
# software-rasterised instead. The two are not close: the software path draws
# simple screens correctly, visibly mangles busy ones, and is far slower — so
# every "how does this title look" judgement made from a plain launch was
# judging the fallback. Same trap as TADPOLE_GL above, same fix.
#
# TADPOLE_GL_HLE=0 forces the software rasteriser, which is still how you tell
# whether a rendering fault is in the shared GL core or only in the replay. The
# shim tests for the variable's PRESENCE, so "off" has to mean unset, not 0.
if [ -z "${TADPOLE_GL_HLE:-}" ] && [ -r "$UICFG" ]; then
    TADPOLE_GL_HLE="$(awk '$1=="gl_hle"{print $2}' "$UICFG" | tail -1)"
fi
: "${TADPOLE_GL_HLE:=1}"
if [ "$TADPOLE_GL_HLE" = 0 ]; then unset TADPOLE_GL_HLE; else export TADPOLE_GL_HLE; fi

if [ "${DEV_HAS_QT:-0}" = 1 ]; then
    # EXACTLY ONE IMPERSONATION VARIANT, AND THIS IS WHY.
    #
    # libdl.so.0, libz.so.1 and libEGL.so are all the SAME SHIM under three
    # names, and having all three on the path is harmless only while a guest
    # links exactly one of them. The Ultra's Qt shell links libEGL AND —
    # through libpng — libz, so it loaded two copies, whose open() chained
    # into each other and recursed until the 64 MB guest stack was gone:
    #
    #     Program received signal SIGSEGV
    #     #0  0x45ea0320 in ?? () from runtime/shimlibs-z/libz.so.1
    #     #1  0x45ea034c in ?? () from runtime/shimlibs-z/libz.so.1
    #     Backtrace stopped: previous frame identical (corrupt stack?)
    #     sp  0x40001008        <- the bottom of the stack
    #
    # It leaves no trace in an strace, because the recursion never reaches a
    # syscall, and no crash report, because the handler needs stack that has
    # gone. The shim now detects the condition at init and says so; this is
    # what stops it arising.
    #
    # shimlibs-egl is self-contained (it carries its own libdl.so.9), so
    # nothing else is needed. Note NO shimlibs-gl either: it holds a second
    # libEGL.so, and only one may win.
    # shimlibs-pkg is SAFE to have here alongside shimlibs-egl: it impersonates
    # libWebServices.so.1, which only package-manager links, so the two never
    # land in the same process. Checked, not assumed — see the Makefile.
    LIBS="$HERE/runtime/shimlibs-egl:$HERE/runtime/shimlibs-pkg:$HERE/runtime/libs"
else
    LIBS="$HERE/runtime/shimlibs-z:$HERE/runtime/shimlibs:$HERE/runtime/libs"
    [ "$TADPOLE_GL" != 0 ] && LIBS="$HERE/runtime/shimlibs-gl:$LIBS"
fi
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

# THE FRONT END MUST START WITHOUT SYSTEM FILES. That is the whole point of the
# setup wizard: a first-time user has no firmware yet, and dying here with a
# missing-path error is exactly the wall of text the wizard replaces. Only the
# modes that actually BOOT something need the guest filesystem.
if [ "$mode" != front ]; then
    for f in "$ROOTFS" "$SYSROOT"; do
        [ -e "$f" ] || { echo "missing: $f" >&2
                         echo "No system files installed. Start Tadpole with" >&2
                         echo "  ./tadpole.sh" >&2
                         echo "and follow the setup wizard, or run" >&2
                         echo "  ./tools/install-firmware.sh <LFC_Downloads>" >&2
                         exit 1; }
    done
fi
# The shim is only needed to run a guest; the front end alone does not use it.
if [ "$mode" != front ] && [ ! -e "$HERE/runtime/shimlibs/libdl.so.0" ]; then
    echo "shim not built — run: cd tadpole && make" >&2; exit 1
fi
if [ ! -x "$VIEWER" ]; then
    echo "Tadpole is not built yet. Run:" >&2
    echo "  cd tadpole && make" >&2
    exit 1
fi

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
    # Qt Embedded binds /tmp/qtembedded-0/QtEmbedded-0 and will not create the
    # parent itself. The wipe above removes it every boot, so put it back here
    # as well as in setup-sysroot.sh.
    [ "${DEV_HAS_QT:-0}" = 1 ] && {
        mkdir -p "$SYSROOT/tmp/qtembedded-0"; chmod 700 "$SYSROOT/tmp/qtembedded-0"; }
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

# START FROM AN EMPTY RUNTIME DIRECTORY.
#
# Everything in here is regenerated on demand — the framebuffer arena, the
# shared state, the input FIFOs, the GL ring — and stale copies cause two
# reliable bugs: the window opens showing the last frame of the PREVIOUS
# session (a dead frame that looks like a live one), and leftover guests from an
# earlier run leave the arena in a state the new viewer cannot make sense of,
# which shows up as a permanently black screen.
#
# ONLY WHEN THIS INVOCATION OWNS THE VIEWER. Removing the arena while a viewer
# has it mapped is catastrophic: the guest's shim recreates the file, gets a NEW
# inode, and the running viewer goes on reading the old unlinked one — black
# forever, home screen included. That is exactly what happened once already,
# because "Run System Menu" spawns a SECOND tadpole.sh (--no-viewer --boot)
# underneath the first one's viewer, and the child ran this line. The guard is
# `use_viewer`: the child skips it, the parent clears up before any viewer
# exists to map anything.
if [ "$use_viewer" = 1 ]; then
    # Reap guests still bound to this directory first. tadpole.sh execs qemu as
    # a GRANDCHILD, so closing a previous session leaves AppManager and
    # VideoDaemon alive holding the arena; clearing the directory underneath
    # them is what turns a stale run into a black screen.
    #
    # EXCLUDE OUR OWN ANCESTORS, not just $$. The pattern appears in the
    # command line of whatever launched us too, and killing a parent kills the
    # pipeline — a run that dies with no output for no visible reason. Never
    # `pkill -f "$TADPOLE_DIR"` for the same reason.
    _is_ancestor() {
        local p=$$
        while [ "${p:-0}" -gt 1 ]; do
            [ "$p" = "$1" ] && return 0
            p=$(awk '{print $4}' "/proc/$p/stat" 2>/dev/null) || return 1
        done
        return 1
    }
    for _pid in $(pgrep -f "TADPOLE_DIR=$TADPOLE_DIR" 2>/dev/null); do
        _is_ancestor "$_pid" && continue
        kill -9 "$_pid" 2>/dev/null || true
    done
    rm -rf "$TADPOLE_DIR"
fi

# Stranded atomic temp files confuse the next run's reads.
find "$SYSROOT/LF/Bulk" -name '*.atomic' -delete 2>/dev/null || true

mkdir -p "$TADPOLE_DIR"

# Every instance shares TADPOLE_DIR (framebuffers, state, input FIFOs), so two
# concurrent runs silently corrupt each other's display — you end up looking at
# another title's pixels and drawing wrong conclusions. Refuse to start a
# second guest on the same dir.
# THE LOCK PROTECTS A RUNNING GUEST, NOT THE DIRECTORY.
#
# Front-end mode runs no guest at all: it opens the viewer and waits. Taking the
# lock there meant the viewer's own "Run System Menu" — which spawns
# `tadpole.sh --no-viewer --boot` — hit its own parent's lock and refused with
#     tadpole: another instance (pid NNN) is using /tmp/tadpole
# Two processes, one of them doing nothing, deadlocking over a directory neither
# was using yet.
LOCK="$TADPOLE_DIR/.lock"
if [ "$mode" = front ]; then
    LOCK=""            # nothing to guard
elif [ -e "$LOCK" ] && kill -0 "$(cat "$LOCK" 2>/dev/null)" 2>/dev/null; then
    echo "tadpole: another instance (pid $(cat "$LOCK")) is using $TADPOLE_DIR" >&2
    echo "  stop it first, or run with a separate dir:" >&2
    echo "    TADPOLE_DIR=/tmp/tadpole-2 $0 ..." >&2
    exit 1
fi
if [ -n "$LOCK" ]; then
    echo $$ > "$LOCK"
    # Belt and braces for --boot, which DOES take the lock and also starts the
    # viewer: without this the viewer reads its own launcher's lock and greys
    # out the menu items that require an idle system.
    export TADPOLE_LOCK_PID=$$
    cleanup_lock() { rm -f "$LOCK"; }
    trap cleanup_lock EXIT
fi

# Per-device guest environment (DEV_ENV in the profile), as qemu -E arguments.
# The device sets these in /etc/profile; we exec the shell binary directly and
# never source one, so they have to be handed over explicitly.
DEV_ENV_ARGS=()
if [ -n "${DEV_ENV:-}" ]; then
    while read -r kv; do
        [ -n "$kv" ] && DEV_ENV_ARGS+=(-E "$kv")
    done <<< "$DEV_ENV"
fi

guest() {
    local bin="$1"; shift
    # THE ROOTFS WINS, NOT THE HOST. This used to keep the path as given
    # whenever it existed, and only fall back to the rootfs when it did not —
    # which silently runs the DEVELOPER'S binary for every name both systems
    # happen to share. /usr/bin/dbus-daemon exists on most desktops, so
    # starting the guest's bus produced
    #     qemu-arm: /usr/bin/dbus-daemon: Invalid ELF image for this architecture
    # and the same trap is waiting for every other common name. An absolute
    # path here means a path INSIDE the guest; look there first.
    case "$bin" in
        /*) [ -e "$ROOTFS$bin" ] && bin="$ROOTFS$bin" ;;
    esac
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
      exec "$QEMU" -s 67108864 -L "$SYSROOT" ${TADPOLE_STRACE:+-strace} \
           ${TADPOLE_TSLIB:+-E TSLIB_REAL=1} \
           ${TADPOLE_TSLIB:--E TSLIB_CONFFILE=/nonexistent-ts.conf} \
           -E LD_LIBRARY_PATH="$LIBS" \
           -E TADPOLE_DIR="$TADPOLE_DIR" \
           -E TADPOLE_SYSROOT="$SYSROOT" \
           -E TADPOLE_W="${DEV_LCD%x*}" -E TADPOLE_H="${DEV_LCD#*x}" \
           -E TADPOLE_QEMU="$QEMU" \
           ${DEV_ENV_ARGS[@]+"${DEV_ENV_ARGS[@]}"} \
           $([ "$debug" = 1 ] && echo "-E TADPOLE_DEBUG=1") \
           ${TADPOLE_LOG:+-E TADPOLE_LOG="$TADPOLE_LOG"} \
           "$bin" "$@" )
}

# The shim creates the framebuffer and state files on first open, and the
# viewer must be able to map them before it starts. Draw the boot logo to do
# it — the real device does exactly this from rcS, so it doubles as a splash.
# (Must be a binary that loads a shim variant: imager-fb pulls in libz.)
if [ ! -e "$TADPOLE_DIR/state.bin" ]; then
    guest /usr/bin/imager-fb /dev/fb0 "$DEV_SPLASH" \
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
        # A SHELL THAT ACTUALLY LOOKS AT THE GUEST.
        #
        # This used to be `guest /bin/busybox sh`, which started a real ARM
        # shell and then lied to you about everything it showed. qemu-user
        # does not chroot: its -L prefix redirects open() and stat(), but NOT
        # execve(). So when that shell ran `ls`, it searched the HOST's PATH,
        # found the host's /usr/bin/ls, and qemu handed the exec straight to
        # the host kernel — you got a host directory listing, from a host
        # binary, inside what looked like a guest prompt.
        #
        #     $ ls /          bin boot dev etc home lib64 lost+found opt run
        #     $ cat /etc/hostname
        #     acpc            <- the host's hostname
        #
        # Pointing PATH at the guest instead does not help: exec'ing an ARM
        # binary from a host shell needs binfmt_misc, which needs root, and
        # "cannot execute binary file" is where that ends.
        #
        # So each command is run as its own guest process, where -L works.
        # busybox applets are invoked directly (`busybox ls /`) because that
        # is the one form needing no exec at all. Pipes and redirection belong
        # to the host shell you typed into, not to this.
        echo "Tadpole guest shell — every command runs inside the emulator."
        echo "busybox applets: ls, cat, ps, find, grep, od, mount, ..."
        echo "'exit' to leave."
        while :; do
            printf 'guest %s $ ' "$(basename "$SYSROOT")"
            IFS= read -r line || { echo; break; }
            case "$line" in
                ""|"#"*) continue ;;
                exit|quit) break ;;
            esac
            # shellcheck disable=SC2086
            guest /bin/busybox $line
        done ;;
    logo)
        guest /usr/bin/imager-fb /dev/fb0 "$DEV_SPLASH"
        echo "logo drawn to $TADPOLE_DIR/fb0.bin"
        [ -n "$viewer_pid" ] && { echo "viewer showing it; Ctrl+C to stop"; wait $viewer_pid; } ;;
    run)
        guest "$prog" ${progargs[@]+"${progargs[@]}"} ;;
    ui)
        # rcS launches VideoDaemon alongside AppManager; AppManager connects to
        # it over /tmp/video_events_socket.
        #
        # ITS OUTPUT USED TO GO TO /dev/null, and that cost real time: the
        # daemon narrates every step it takes ("Creating Display Surface",
        # "Starting Video", "Stopping Video"), and with all of it discarded the
        # only visible symptom of a failed video was a layer that turned on and
        # went blank again. Prefixed so it does not read as AppManager's.
        # sed -u, not sed: the daemon double-forks and then says very little,
        # so a 4 KB block buffer holds its entire account of a failed video
        # until the process group dies. Unbuffered or not at all.
        # A D-BUS SESSION BUS, FOR A Qt DEVICE.
        #
        # rcS runs `/etc/init.d/dbus-1 start` and `dbus-session start`, and the
        # Ultra's shell is built on it: RioPkgManager — which is what tells the
        # home screen WHICH APPS EXIST — is a D-Bus proxy, and libQtDBusE is in
        # every module's NEEDED. Without a bus the picker comes up correctly
        # and completely empty, which looks like missing content rather than a
        # missing service.
        #
        # dbus-daemon --fork detaches, so the address has to be captured from
        # its stdout before it goes. The socket is an abstract one, so it is
        # visible to the other qemu processes without a path in the sysroot.
        if [ "${DEV_HAS_QT:-0}" = 1 ]; then
            mkdir -p "$SYSROOT/var/run" "$SYSROOT/var/lib/dbus"
            guest /usr/bin/dbus-uuidgen --ensure >/dev/null 2>&1 || true
            dbus_addr="$(guest /usr/bin/dbus-daemon --session --print-address --fork \
                         2>/dev/null | grep '^unix:' | head -1)"
            if [ -n "$dbus_addr" ]; then
                DEV_ENV_ARGS+=(-E "DBUS_SESSION_BUS_ADDRESS=$dbus_addr")
                echo "=== dbus session bus: $dbus_addr"
            else
                echo "tadpole: no d-bus session bus — the home screen will be empty" >&2
            fi
            # THE PACKAGE MANAGER DAEMON — this is what puts icons on the home
            # screen, and nothing else does.
            #
            # MainPicker does not read the package databases itself and does
            # not scan ProgramFiles. It asks com.leapfrog.PackageManager over
            # D-Bus, through libQtRioPkgManager, which is only a PROXY. The
            # service is /usr/bin/package-manager. Without it the picker comes
            # up correct, themed and completely empty, however many packages
            # are installed and however carefully they are registered in the
            # databases — the query never reaches anything.
            #
            # Found by grepping the image for the service name: exactly two
            # files contain it, the proxy and the daemon.
            guest /usr/bin/package-manager 2>&1 | sed -u 's/^/[pkg] /' &
            sleep 2
            # AND ASK IT TO SCAN, ONCE, IF THE DATABASE IS EMPTY.
            #
            # THIS IS WHAT PUTS THE ICONS ON THE HOME SCREEN. The daemon does
            # not scan at startup — on hardware the databases arrive populated
            # on the Bulk partition and lfpkg maintains them at install time.
            # We install by untarring, so nothing does, and the picker is
            # correct and empty.
            #
            # RebuildPackageDatabase is the device's own answer: it walks
            # ProgramFiles, parses every meta.inf with the code that wrote
            # them, and fills both databases in the right shape. Hand-written
            # registration cannot compete with that and is no longer needed —
            # tools/register-packages.py is kept for inspection (--list) and
            # for the record of what the schema is.
            #
            # Only when Packages is empty: a rebuild discards per-package
            # local state (install dates, "NEW!" flags, last played), which is
            # not something to do on every boot.
            if ! guest /usr/bin/dbus-send --session --print-reply \
                    --dest=com.leapfrog.PackageManager / \
                    com.leapfrog.PackageManager.GetPendingPackages \
                    >/dev/null 2>&1; then
                echo "tadpole: package manager did not answer; home screen may be empty" >&2
            elif [ ! -s "$SYSROOT/LF/Bulk/SharedPackageInfo.db" ] ||
                 ! "${TADPOLE_PYTHON:-python3}" -c "import sqlite3,sys; sys.exit(0 if sqlite3.connect('$SYSROOT/LF/Bulk/SharedPackageInfo.db').execute('select count(*) from Packages').fetchone()[0] else 1)" 2>/dev/null; then
                echo "=== package database is empty; asking the device to build it"
                guest /usr/bin/dbus-send --session --print-reply \
                    --dest=com.leapfrog.PackageManager / \
                    com.leapfrog.PackageManager.RebuildPackageDatabase \
                    >/dev/null 2>&1 || true
            fi
        fi
        # NO VideoDaemon ON A Qt DEVICE — not yet.
        #
        # On this firmware it links none of the three names we impersonate, so
        # the only way the shim reaches it is transitively, and from down there
        # its own dlsym(RTLD_NEXT, "open") comes back round to itself. The
        # shim's own guard catches that and stops rather than recursing away
        # the stack, which is correct and also means the daemon never runs.
        #
        # AppServer copes: it logs "DaemonControl socket connect failed" and
        # carries on to a working home screen. Video playback will need this
        # solved; the picker does not.
        if [ "${DEV_HAS_QT:-0}" != 1 ]; then
            guest /LF/Base/bin/VideoDaemon 750 2>&1 | sed -u 's/^/[vd] /' &
        fi
        sleep 1
        # THE SYSTEM MENU IS NOT THE SAME PROGRAM ON EVERY DEVICE.
        # LeapPad2: AppManager, no arguments.
        # LeapPad Ultra: AppServer <first QML app> -qws, mirroring the
        # `nice -n $niceLevel AppServer $appPath $2 -qws` in its /usr/bin/app.
        echo "=== $(basename "$DEV_SHELL") ==="
        guest "$DEV_SHELL" ${DEV_FIRST_APP:+"$DEV_FIRST_APP"} $DEV_SHELL_ARGS ;;
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
        elif [ "$use_viewer" = 0 ]; then
            # --no-viewer WITH NO MODE. The front end is a window and nothing
            # else, so asking for it without one is a contradiction — and
            # answering "viewer not built" sent a script hunting for a missing
            # binary that was sitting right there, built. Say what was actually
            # asked for.
            echo "tadpole: --no-viewer needs something to run." >&2
            echo "  --boot --no-viewer     the system menu, headless" >&2
            echo "  --run PROG --no-viewer a single guest binary" >&2
            exit 2
        else
            echo "viewer not built — run: cd tadpole && make viewer" >&2
            exit 1
        fi ;;
esac
