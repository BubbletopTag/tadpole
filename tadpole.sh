#!/bin/bash
# Tadpole — LeapPad2 emulator. One command to run it.
#
#   ./tadpole.sh                 open the Tadpole front end (nothing runs yet;
#                                use File -> Run System Menu, or the wizard)
#   ./tadpole.sh --boot          open it AND start the system menu straight away
#   ./tadpole.sh --shell         ARM shell inside the guest, no viewer
#   ./tadpole.sh --logo          draw the boot logo (quick "is it alive" test)
#   ./tadpole.sh --app NAME      launch an installed app by name or PackageID
#                                (native titles too — straight in, no home screen)
#   ./tadpole.sh --list           list installed apps
#   ./tadpole.sh --run PROG ...  run any guest binary with the viewer up
#   ./tadpole.sh --no-viewer     skip the SDL window
#   ./tadpole.sh --debug         verbose shim logging
#   ./tadpole.sh --touch-debug   red crosshair where the viewer thinks you tapped
#   TADPOLE_GL=0 ./tadpole.sh    use the stock GPU stack (titles will assert)
#   ./tadpole.sh -r 270 ...      start at a fixed rotation. The window turns
#                                with the guest on its own now — portrait for
#                                the LeapPad UI, landscape for a title — so
#                                this is a starting point, not a setting.
#                                Untick Options -> Graphics -> Turn with the
#                                app to make it stick.
#
# Runs on Glasspole, this project's own ARM JIT, whenever one is built or
# bundled; qemu-arm is the fallback. TADPOLE_QEMU="$(command -v qemu-arm)"
# puts a single run back on qemu.
#
# Viewer controls:  arrows = D-pad, Z/X = A/B, Home = menu, Esc = back,
#                   mouse click/drag = stylus, Ctrl+R = turn, Ctrl+Q = quit.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# WHICH ENGINE RUNS THE GUEST. Glasspole if this checkout has one built, the
# bundled or installed qemu-arm otherwise — tad_qemu() owns that order and the
# reasoning behind it. A user who installed qemu-user themselves keeps working
# either way; nothing here needs to know which of the two it got.
PROJ="$HERE"
. "$HERE/tools/lib-deps.sh"
QEMU="$(tad_qemu || true)"
if [ -z "$QEMU" ]; then
    echo "tadpole: no ARM engine — neither glasspole nor qemu-arm." >&2
    echo "  cd glasspole && ./fetch-deps.sh && cmake -S . -B build -GNinja && ninja -C build" >&2
    echo "  ./tools/fetch-deps.sh   stages a qemu-arm into build/deps instead" >&2
    echo "  or install your distribution's qemu-user package" >&2
    exit 1
fi
# TELL THE REST OF TADPOLE WHAT IT IS ACTUALLY RUNNING ON.
#
# The viewer needs this for two things it cannot work out for itself: the
# straggler sweep, which only kills processes whose comm matches the engine
# that was launched, and the About box, which used to name qemu unconditionally
# and would now be wrong for most people. It was already read from here when
# the user set it by hand; setting it ourselves means the answer no longer
# depends on whether they did.
export TADPOLE_QEMU="$QEMU"
# DISCOVER the rootfs rather than hardcoding one firmware version. Whatever
# install-firmware.sh extracted lands under rootfs/<version>/…/ubi_rfs, and the
# version is whatever the user's own device shipped with. emmc_rfs is the same
# thing for an eMMC device (the LeapPad3), whose firmware carries a tar rather
# than a UBI volume — see runtime/setup-sysroot.sh for why it is not called
# ubi_rfs anyway.
ROOTFS=""
for cand in "$HERE"/rootfs/*/emmc_rfs "$HERE"/rootfs/*/ubi_rfs "$HERE"/rootfs/*/*/ubi_rfs; do
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

# WHICH LOGO. rcS picks by platform_family — Lucy-Boot-logo.png on a LeapsterGS,
# Valencia-Boot-logoCW.png on everything else, this device included.
#
# "CW" READS AS SIDEWAYS AND IS CORRECT. The panel is portrait, so everything
# the device draws is stored a quarter turn from how it is held — the home
# screen and every title as much as this logo, which is why shots/01-boot-logo.png
# has always looked like that. Presentation is the viewer's `-r` and nothing
# else's, so this is drawn exactly as stored, same as the guest's own output.
BOOTLOGO=/var/screens/Valencia-Boot-logoCW.png

# HLE — HOST-GPU REPLAY — ON BY DEFAULT, read from the same config file.
#
# This used to be opt-in, and the only thing that set it was tools/probe-race.sh.
# A normal launch therefore started the viewer, announced HLE, and then quietly
# software-rasterised instead. The two are not close: the software path draws
# simple screens correctly, visibly mangles busy ones, and is far slower — so
# every "how does this title look" judgement made from a plain launch was
# judging the fallback. Same trap as TADPOLE_GL above, same fix.
#
# AND NOW IT IS THE ONLY PATH. The software rasteriser is deprecated: it is
# years behind the GL core it shares, several times slower, and it cannot
# express state the titles rely on — it samples one texture unit and ignores the
# blend factors. Two real bugs this month were invisible in software for exactly
# that reason. So the config checkbox no longer turns it off (the front end
# shows it ticked and greyed), and a guest that asks for replay and cannot get
# it now STOPS rather than quietly rendering the rest of the session wrongly.
#
# TADPOLE_GL_SOFTWARE=1 is the way back, and it has to be asked for by name:
#
#     TADPOLE_GL_SOFTWARE=1 ./tadpole.sh --app X
#
# It is still the right tool for one job — telling whether a rendering fault is
# in the shared GL core or only in the replay — which is why it is kept at all.
if [ -n "${TADPOLE_GL_SOFTWARE:-}" ] && [ "${TADPOLE_GL_SOFTWARE}" != 0 ]; then
    export TADPOLE_GL_SOFTWARE
    unset TADPOLE_GL_HLE
else
    unset TADPOLE_GL_SOFTWARE
    TADPOLE_GL_HLE=1; export TADPOLE_GL_HLE
fi

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
    # shimlibs-egl is self-contained (it carries its own libdl.so.9, its own
    # libGLESv1_CM.so and its own libasound.so.2), so nothing else is needed.
    # Note NO shimlibs-gl either: it holds a second libEGL.so, and only one may
    # win. And note NO shimlibs, even though that is where libasound.so.2 is
    # BUILT — putting the directory on the path would bring libdl.so.0 with it,
    # which is the exact two-interceptor case above. The library is copied into
    # shimlibs-egl by `make shimegl` instead, and that is what gives a Qt device
    # sound: without it Brio's dlopen of libAudio.so bound to LeapFrog's real
    # libasound in runtime/libs, which honoured our null sink and threw every
    # sample away.
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

# The command line as typed, kept before the parser shifts it away, so an error
# message can hand back something that can actually be pasted.
ORIG_ARGS=("$@")
use_viewer=1; debug=0; mode=front; scale=2; rotate=""; prog=""; appname=""; declare -a progargs=()
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
        -r)          shift; rotate="${1:-0}" ;;   # 270 = the portrait UI
        --)          shift; progargs=("$@"); break ;;
        *)           progargs+=("$1") ;;
    esac
    shift || true
done

# NO VIEWER MEANS NO HOST GPU, AND THEREFORE NO RENDERING WORTH TRUSTING.
#
# The replayer lives in the viewer, so `--no-viewer` cannot do host-GPU replay
# by construction. It used to fall back to the software rasteriser silently,
# which is how every compat verdict in this repository came to be recorded on a
# path that cannot multitexture and ignores the blend factors — the reason two
# rendering bugs this month were invisible to the sweep that was supposed to
# find them.
#
# So say it here, once, in the launcher, rather than letting the guest abort a
# few seconds later with less context. Modes that never render are exempt:
# --list and --shell draw nothing.
#
# ALSO EXEMPT: TADPOLE_SUPERVISED. tadpole-view sets it when IT is the one
# invoking us — "Run System Menu" and friends run us with --no-viewer because
# the window we would otherwise open is the one already on screen, not
# because nobody is watching. Real host-GPU replay happens there, same as it
# would through our own viewer. Without this, the front end's own boot path
# hit the same refusal meant for genuinely headless callers and could not
# start the guest at all.
if [ "$use_viewer" = 0 ] && [ "$mode" != list ] && [ "$mode" != shell ] \
   && [ -z "${TADPOLE_SUPERVISED:-}" ] \
   && { [ -z "${TADPOLE_GL_SOFTWARE:-}" ] || [ "${TADPOLE_GL_SOFTWARE}" = 0 ]; }; then
    echo "tadpole: --no-viewer has no host GPU to replay to, and the software" >&2
    echo "  rasteriser is deprecated — it cannot express multitexturing or the" >&2
    echo "  blend factors, so what it draws is not what the device draws." >&2
    echo "" >&2
    echo "  To render for real, drop --no-viewer." >&2
    echo "  To use the software rasteriser deliberately, ask for it:" >&2
    echo "      TADPOLE_GL_SOFTWARE=1 $0 ${ORIG_ARGS[*]}" >&2
    exit 1
fi

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
    # SYSFS IS VOLATILE TOO, AND ONE NODE IN IT IS A LOADED GUN.
    #
    # Same argument as /tmp, and it took a day to notice because the symptom
    # arrives one boot LATER than the cause. /sys is a kernel filesystem: its
    # attributes are re-created at every boot from whatever the driver
    # defaults to, and writing to one is a COMMAND, not a record. Ours is a
    # real directory, so a command the guest issued once is still sitting
    # there being obeyed on the next twenty boots.
    #
    # lf2000-power/shutdown is the one that matters. Powering off — the idle
    # timeout does it after four minutes, and it is meant to — writes 1 there.
    # From then on the device read its own old instruction at startup and shut
    # down again immediately: AppServer got as far as LockOrientation, played
    # the shutdown movie and left, about a second in. It reads like the shell
    # refusing to start, and every plausible cause (the theme, the installed
    # packages, the idle timeout itself) can be ruled out one slow boot at a
    # time without getting near it, because none of them is it.
    #
    # Reset the volatile ones by hand rather than wiping /sys: unlike /tmp,
    # nearly everything under it is OUR fake hardware description from
    # setup-sysroot.sh, and re-creating all of that here would duplicate the
    # one place that is supposed to own it.
    for node in "$SYSROOT/sys/devices/platform"/*-power/shutdown; do
        [ -e "$node" ] && printf '0' > "$node"
    done
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

# WHERE CRASHES ARE KEPT.
#
# One dated directory per run, outside /tmp, so a crash is still there tomorrow
# and two of them can be compared. The shim writes crash.log into it; the
# viewer drops the tail of the guest log beside it when a guest dies having
# written one. Overridable so a soak run can point every iteration at one place.
#
# XDG_STATE_HOME is the right variable for this: it is for data that should
# persist between restarts but is not something the user edits or would miss.
# The no-auto-power-off flag, for sysroots built before it existed.
# setup-sysroot.sh writes this, but an existing install never re-runs it, and
# an update that only helps people who reinstall is not much of an update.
[ -d "$SYSROOT/flags" ] && [ ! -e "$SYSROOT/flags/poweron" ] && \
    : > "$SYSROOT/flags/poweron" 2>/dev/null

CRASHDIR="${TADPOLE_CRASHDIR:-${XDG_STATE_HOME:-$HOME/.local/state}/tadpole/crashes/$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$CRASHDIR" 2>/dev/null || CRASHDIR="$TADPOLE_DIR"
export TADPOLE_CRASHDIR="$CRASHDIR"
# Per-device guest environment (DEV_ENV in the profile), as qemu -E arguments.
# The device sets these in /etc/profile; we exec the shell binary directly and
# never source one, so they have to be handed over explicitly.
# TSLIB: OFF FOR BRIO, ON FOR Qt.
#
# The LeapPad2 does not want it — Brio has its own touchscreen path, says so
# ("Falling back on touchscreen interface"), and tslib's chain crashes on the
# first tap. Pointing TSLIB_CONFFILE at a nonexistent file makes its init fail
# cleanly and Brio use its own path.
#
# The Ultra has no such choice: Qt's only mouse driver here is the tslib
# plugin, so tslib has to work. It does, now that the shim answers to
# /dev/input/touchscreen0 — the name /etc/profile tells tslib to open, and the
# absence of which was the actual cause of that "tslib crash".
#
# AN ARRAY, NOT NESTED ${:-} EXPANSIONS. The first attempt wrote
# ${TADPOLE_TSLIB:-${DEV_HAS_QT:--E TSLIB_CONFFILE=...}}, which reads as "use
# DEV_HAS_QT if it is set" — so with DEV_HAS_QT=1 it expanded to a bare `1` on
# qemu's command line and the guest never started.
declare -a TSLIB_ARGS=()
if [ -n "${TADPOLE_TSLIB:-}" ] || [ "${DEV_HAS_QT:-0}" = 1 ]; then
    TSLIB_ARGS+=(-E TSLIB_REAL=1)
else
    TSLIB_ARGS+=(-E TSLIB_CONFFILE=/nonexistent-ts.conf)
fi

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
      # NO CORE DUMPS BY DEFAULT.
      #
      # A crashing guest drops a qemu core into its cwd, which is the sysroot.
      # 87 of them had accumulated here — 7.6 GB — and none had ever been
      # opened, because the shim writes its own symbolised report and gdb
      # cannot read these anyway: every r-x segment comes out with
      # p_filesz == 0 and there is no NT_FILE note to map them back (see
      # tadpole_crash.c). Running many instances at once would multiply that
      # by the worker count.
      #
      # TADPOLE_CORES=1 restores them for anyone who does want to try.
      [ -n "${TADPOLE_CORES:-}" ] || ulimit -c 0 2>/dev/null || true
      exec "$QEMU" -s 67108864 -L "$SYSROOT" ${TADPOLE_STRACE:+-strace} ${TADPOLE_STRACE:+-E TADPOLE_STRACE=1} \
           ${TSLIB_ARGS[@]+"${TSLIB_ARGS[@]}"} \
           -E LD_LIBRARY_PATH="$LIBS" \
           -E TADPOLE_DIR="$TADPOLE_DIR" \
           -E TADPOLE_SYSROOT="$SYSROOT" \
           -E TADPOLE_W="${DEV_LCD%x*}" -E TADPOLE_H="${DEV_LCD#*x}" \
           -E TADPOLE_QEMU="$QEMU" \
           ${DEV_HAS_QT:+-E TADPOLE_ABS_PANEL=1} \
           ${DEV_ENV_ARGS[@]+"${DEV_ENV_ARGS[@]}"} \
           $([ "$debug" = 1 ] && echo "-E TADPOLE_DEBUG=1") \
           ${TADPOLE_LOG:+-E TADPOLE_LOG="$TADPOLE_LOG"} \
           -E TADPOLE_CRASHDIR="$CRASHDIR" \
           "$bin" "$@" )
}

# THIS USED TO DRAW THE BOOT LOGO HERE, and the only reason nobody minded is
# that it never worked.
#
# The shim creates the framebuffer and state files on first open, and the
# viewer once had to map them before it started — so this ran imager-fb on the
# logo to bring them into being, and the picture was a bonus splash. Two things
# have changed since. The viewer retries try_map() every frame until it
# succeeds (see the note by that call), so nothing needs the arena to exist
# before a guest does; and imager-fb could not resolve __aeabi_uidiv against
# our shim's libz, so for as long as this line has existed it drew nothing,
# created nothing, and was ignored by the `|| true`.
#
# Fixing the shim made it work — which meant the front end sat at rest showing
# LeapFrog branding instead of its own idle screen, and a cold Run System Menu
# flashed a logo at people who have Fast Boot ticked. Neither is something to
# fix twice, so the vestigial step is gone: the first guest creates the arena,
# the viewer maps it when it appears, and the boot logo is drawn by the one
# thing that is supposed to draw it — viewer/tadpole_boot.c, when asked.

# WHICH WAY UP THE SYSTEM UI IS DRAWN — A PROPERTY OF THE DEVICE.
#
# The viewer turned every non-title screen by 270, which is right for the
# LeapPad2 (portrait panel, so AppManager stores its home screen sideways) and
# wrong for a Qt device, which draws landscape-native and upright. Getting it
# wrong presents as BROKEN TOUCH rather than as a rotated window: event_to_fb()
# maps the click through the same quarter turn, so every tap lands a quarter
# turn from where it was aimed.
export TADPOLE_UI_ROTATE="${DEV_UI_ROTATE:-270}"

viewer_pid=""
if [ "$use_viewer" = 1 ] && [ -x "$VIEWER" ]; then
    # -r ONLY IF ASKED FOR. It used to be passed always, defaulting to 0,
    # which meant starting from this script silently overrode the orientation
    # saved in ui.cfg — and now would also override the window's own idea of
    # which way up the guest is drawing before the guest has said anything.
    "$VIEWER" -s "$scale" ${rotate:+-r "$rotate"} -d "$TADPOLE_DIR" &
    viewer_pid=$!
    trap 'kill $viewer_pid 2>/dev/null' EXIT INT TERM
    sleep 0.5
fi

# THE CONNECT NAG, ANSWERED THE WAY THE DEVICE ANSWERS IT.
#
# "Connect to LeapFrog Connect to get the most out of your LeapPad" goes up on
# the way to the home screen, every boot, and there is nothing behind it here:
# the service it wants closed years ago, and it is one more tap between a user
# and their games. The device's own switch for it lives in Parent Settings ->
# connection nag, and all that switch does is
#
#     _global._uiData._allProfileUIData.ConnectionReminders = <bool>
#
# which lands in the ALL-profiles UIData.json. HomePickerState::CheckForConnectNag
# reads it and, when it is false, returns without pushing ConnectNag.swf —
# measured, headless, by booting with the field set and finding the trace line
# for the check present and the push absent.
#
# So Tadpole writes the same field the device would, at every launch, from the
# front end's setting. Default off; Options -> System Settings turns it back on
# for anyone who wants the device's behaviour exactly.
#
# THE PACKAGE DIRECTORY IS READ, NOT GUESSED: the LPAD UI names itself in
# /LF/Base/LPAD/meta.inf (PackageID="PAD2-0x1F1E0002-100000" on a LeapPad2),
# and that is the directory its UI data sits in. On a device that has never
# booted there is no such file yet, so it is created — and if this ever met a
# system whose id we read wrongly, AppManager would simply make its own and the
# nag would show once more before the next launch caught it.
connect_nag="$(awk '$1=="connect_nag"{print $2}' "$UICFG" 2>/dev/null | tail -1)"
: "${connect_nag:=0}"
[ "$connect_nag" = 0 ] && want_reminders=false || want_reminders=true
set_connect_reminders() {
    local dir f pkg
    dir="$SYSROOT/LF/Bulk/Data/Local/All"
    [ -d "$SYSROOT/LF/Base/LPAD" ] || return 0     # no firmware installed yet
    for f in "$dir"/*/UIData.json; do
        [ -f "$f" ] || continue
        if grep -q '"ConnectionReminders"' "$f"; then
            sed -i "s/\"ConnectionReminders\"[[:space:]]*:[[:space:]]*[a-z]*/\"ConnectionReminders\": $want_reminders/" "$f"
        elif grep -q '^[[:space:]]*{[[:space:]]*}[[:space:]]*$' "$f"; then
            printf '{"ConnectionReminders": %s}\n' "$want_reminders" > "$f"
        else
            sed -i "s/}[[:space:]]*$/,\"ConnectionReminders\": $want_reminders}/" "$f"
        fi
        found=1
    done
    [ -n "${found:-}" ] && return 0
    pkg="$(grep -oE 'PackageID="[^"]*"' "$SYSROOT/LF/Base/LPAD/meta.inf" 2>/dev/null |
           head -1 | cut -d\" -f2)"
    [ -n "$pkg" ] || return 0
    mkdir -p "$dir/$pkg" 2>/dev/null || return 0
    printf '{"ConnectionReminders": %s}\n' "$want_reminders" > "$dir/$pkg/UIData.json"
}
set_connect_reminders

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
                # A NATIVE TITLE. It cannot be exec'd — CreateApp has to be
                # called by CAppManager — so hand it to AppManager, which takes
                # the app as argv[1] and the player as argv[2].
                echo "=== $name — $guestpath (direct) ==="
                # SIGN A PLAYER IN, the way the home screen would.
                #
                # CAppManager::Run reads argv[2] as a player ID and calls
                # CSystem::SetCurrentPlayerID with it; with no argument there
                # is no current player, and anything that reads profile data
                # gets nothing back. That is not a hypothetical: GalleryWidget
                # dereferenced the empty result and segfaulted in
                # LTM::Value::asMap(), and it stops doing so the moment a
                # player is set. Launching straight into a title should differ
                # from the home screen in the steps it skips, not in who is
                # playing.
                #
                # The first real profile directory, since a device that has
                # been set up has at least one. TADPOLE_PLAYER overrides.
                pid="${TADPOLE_PLAYER:-}"
                if [ -z "$pid" ]; then
                    for cand in "$SYSROOT"/LF/Bulk/Data/Local/[0-9]*; do
                        [ -d "$cand" ] || continue
                        pid="$(basename "$cand")"; break
                    done
                fi
                : "${pid:=0}"
                echo "    player $pid"
                # AppManager TAKES THE APP AS argv[1] and the player as argv[2].
                # CAppManager::Run only falls back to pushing LPAD/main.swf
                # when it is given nothing to run, so passing a path both
                # launches the title and skips the home screen — no shim hook,
                # no substitution, no reading anyone else's ABI.
                guest /LF/Base/bin/AppManager "$guestpath" "$pid" ;;
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
        guest /usr/bin/imager-fb /dev/fb0 "$BOOTLOGO"
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
            # A SYSTEM BUS AS WELL — rcS STARTS TWO, AND WE STARTED ONE.
            #
            # rcS runs `/etc/init.d/dbus-1 start` (system) AND
            # `/etc/init.d/dbus-session start`. Only the session bus was ever
            # started here, and for the package manager that was enough, so the
            # gap went unnoticed.
            #
            # It is not enough for the network stack: libQtRioConnman.so.1 asks
            # for QDBusConnection::systemBus() — the symbol is right there in
            # its strings, next to net.connman.Manager/Service/Technology — so
            # with no system bus at all, every ConnMan call fails before it
            # reaches anything. What that looks like from the outside is
            #
            #     Can't get: wifi technology is NULL
            #
            # and an out-of-box WiFi page that cannot work out its own state, so
            # Skip returns to it and the setup flow loops there for ever.
            #
            # AN ABSTRACT ADDRESS, AND NOT THE DEFAULT PATH — THE DEFAULT PATH
            # IS THE DEVELOPER'S OWN SYSTEM BUS.
            #
            # The guest's dbus is configured to listen on
            # /var/run/dbus/system_bus_socket. bind() is not one of the calls
            # the shim translates, and qemu's -L only redirects paths that
            # ALREADY EXIST — so a guest binding a NEW socket at that path hands
            # it to the host kernel unchanged, where it names the HOST's system
            # bus. It fails, loudly and luckily:
            #
            #     Failed to bind socket "/var/run/dbus/system_bus_socket":
            #     Address already in use
            #
            # "Luckily" because the failure is the desktop's own bus being there
            # already. On a machine without one running, that same call would
            # have succeeded and put a guest's D-Bus where the host expects its
            # own. Not a risk worth leaving open for the sake of a default.
            #
            # --address overrides the config's <listen>, and an abstract socket
            # needs no path in the sysroot at all — exactly what the session bus
            # above already does. DBUS_SYSTEM_BUS_ADDRESS is what Qt's
            # systemBus() checks before falling back to the compiled-in path.
            rm -rf "$SYSROOT/var/run/dbus"
            mkdir -p "$SYSROOT/var/run/dbus"
            # OUR OWN CONFIG, BECAUSE THE DEVICE'S CANNOT WORK HERE. Three
            # things in /etc/dbus-1/system.conf stop it dead, and none of them
            # can be turned off from the command line on a dbus this old
            # (--nopidfile does not exist; it prints its usage and exits):
            #
            #   <pidfile>/var/run/dbus/pid</pidfile>
            #       dbus-daemon links none of the shim's vectors, so it runs
            #       UNSHIMMED — and an unshimmed guest cannot create files,
            #       because qemu's -L only redirects paths that already exist.
            #       The create falls through to the HOST's /var/run/dbus/pid:
            #           Failed to open "/var/run/dbus/pid": Permission denied
            #   <user>messagebus</user>
            #       a user this guest has no business becoming, and we are not
            #       root to do it with.
            #   <listen>unix:path=/var/run/dbus/system_bus_socket</listen>
            #       the host's own system bus — see above.
            #
            # The policy is permissive rather than the device's default-deny
            # plus per-service holes. This is an emulator on a developer's
            # desktop, on a private bus that exists for one guest; a denial here
            # buys nothing and costs an afternoon.
            cat > "$SYSROOT/var/run/dbus/tadpole-system.conf" <<XML
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>system</type>
  <keep_umask/>
  <listen>unix:abstract=tadpole-sysbus-$$</listen>
  <standard_system_servicedirs/>
  <policy context="default">
    <allow own="*"/>
    <allow send_destination="*" eavesdrop="true"/>
    <allow receive_sender="*" eavesdrop="true"/>
  </policy>
</busconfig>
XML
            sys_addr="$(guest /usr/bin/dbus-daemon \
                        --config-file=/var/run/dbus/tadpole-system.conf \
                        --print-address --fork 2>/dev/null | grep '^unix:' | head -1)"
            if [ -n "$sys_addr" ]; then
                DEV_ENV_ARGS+=(-E "DBUS_SYSTEM_BUS_ADDRESS=$sys_addr")
                echo "=== dbus system bus: $sys_addr"
            else
                echo "tadpole: no d-bus system bus — WiFi setup will loop" >&2
            fi
            # THE NETWORK STACK, FAKED RATHER THAN RUN — AND THAT IS A REVERSAL.
            #
            # rcS runs /etc/init.d/connman and the daemon is in the rootfs, so
            # this used to start the real /usr/sbin/connmand: the device ships
            # the thing, it is guest ARM, and reimplementing Manager, Technology,
            # Service and Agent looked like a lot of surface to keep honest.
            #
            # WHAT THAT ARGUMENT MISSED IS THAT connmand IS NOT SANDBOXED HERE.
            # qemu-user forwards its netlink socket to the host kernel, so the
            # "device" it manages is the DEVELOPER'S MACHINE. Straight off a
            # boot of this branch, with nothing asked of it:
            #
            #     connmand: Could not clear IPv4 address index 2
            #     connmand: enp5s0 {newlink} index 2 address 00:E0:23:B3:70:09
            #     connmand: Adding interface enp5s0 [ ethernet ]
            #     connmand: enp5s0 {add} address 192.168.1.3/24 label enp5s0
            #     connmand: virbr1 {add} route 10.0.2.0 gw 0.0.0.0 scope 253
            #
            # index 2 is the host's real NIC holding the host's real address,
            # and the clear failed only because we are not root. Everything else
            # it wants to do — DHCP, resolv.conf, routes — is aimed at the same
            # place. Having done all that it STILL reported no wifi, because
            # there is no wireless device to find, so the shell got nothing at
            # all out of the risk. That is not a trade worth making for a status
            # icon.
            #
            # So the fake now owns net.connman: runtime/tadpole-connman, built
            # by tadpole/Makefile, guest-side ARM on the guest's own libdbus.
            # THE TWO CANNOT COEXIST ANYWAY — one D-Bus name has one owner, and
            # whichever lost the race would sit unusable — so this is a choice
            # that had to be made either way, and it is made in favour of the
            # one that cannot touch the host.
            #
            # It reports one wifi technology, powered and connected, and one
            # open network in state "online". That is a description, not a
            # connection: the guest's actual internet comes from qemu handing
            # its sockets to the host, and has worked for as long as that has.
            #
            # TADPOLE_REAL_CONNMAN=1 puts the real daemon back for anyone
            # investigating it. READ THE PARAGRAPH ABOVE FIRST — it will go
            # looking at your interfaces, and running it as root would let it
            # succeed.
            [ "${DEV_HAS_WIFI:-0}" = 1 ] && [ -n "$sys_addr" ] && {
                if [ "${TADPOLE_REAL_CONNMAN:-0}" = 1 ]; then
                    echo "tadpole: TADPOLE_REAL_CONNMAN=1 — starting the REAL connmand." >&2
                    echo "  It is not sandboxed: it will enumerate and try to reconfigure" >&2
                    echo "  this machine's own network interfaces. Never run it as root." >&2
                    # -n keeps it in the foreground of its own qemu (no
                    # daemonising to lose), and its chatter is prefixed so it
                    # does not read as the shell's.
                    guest /usr/sbin/connmand -n 2>&1 | sed -u 's/^/[net] /' &
                    sleep 1
                elif [ -x "$HERE/runtime/tadpole-connman" ]; then
                    # THE ADDRESS IS PASSED, NOT INHERITED. The fake is linked
                    # -nostdlib with its own _start, so uClibc's __environ is
                    # never populated and a getenv() inside libdbus would come
                    # back NULL — at which point libdbus falls back to its
                    # compiled-in /var/run/dbus/system_bus_socket, which is the
                    # HOST's system bus (same trap as the <listen> above). It
                    # takes --address and refuses to start without one.
                    guest "$HERE/runtime/tadpole-connman" --address "$sys_addr" \
                        2>&1 | sed -u 's/^/[net] /' &
                    sleep 1
                else
                    echo "tadpole: runtime/tadpole-connman is not built — the shell" >&2
                    echo "  will believe it is offline. Build it with: cd tadpole && make connman" >&2
                fi
                # wpa_supplicant. Brio's libWireless is a D-Bus client for it —
                # its strings carry the whole generated proxy:
                # fi.w1.wpa_supplicant1, .Interface, .Network, InterfaceUnknown,
                # and the interface name wlan0.
                #
                # -u is the D-Bus control interface and needs NO hardware and no
                # interface: the daemon owns the name and waits to be told about
                # interfaces, which is exactly how connman drives it on the
                # device. Nothing here touches a radio, because there is none.
                #
                # THIS BLOCK ONCE CLAIMED TO BE WHY NO BRIO TITLE STARTS. It is
                # not, and the claim survived several rounds because starting
                # the daemon is obviously *necessary*. It is not sufficient:
                # with wpa_supplicant running and fi.w1.wpa_supplicant1 owned on
                # the bus, every title still died in CWirelessMPI's constructor.
                # The real cause is avahi, below.
                if [ -x "$ROOTFS/usr/sbin/wpa_supplicant" ]; then
                    guest /usr/sbin/wpa_supplicant -u 2>&1 | sed -u 's/^/[wpa] /' &
                    sleep 1
                fi
                # avahi, AND THIS IS THE ONE THAT DECIDES WHETHER ANY BRIO TITLE
                # RUNS AT ALL.
                #
                # CWirelessModule's constructor makes a blocking D-Bus call to
                # org.freedesktop.Avahi. Nothing owns that name unless the
                # daemon is running, so the bus tries to ACTIVATE it — and the
                # firmware's own service file exists purely to make that fail:
                #
                #     # This service should not be bus activated if systemd
                #     # isn't running, so that activation won't conflict with
                #     # the init script startup.
                #     Exec=/bin/false
                #
                # /bin/false exits 1, the daemon returns
                # org.freedesktop.DBus.Error.Spawn.ChildExited, dbus-c++ turns a
                # non-reply into a thrown DBus::Error, and the constructor has
                # no handler. The unwinder then runs a cleanup that stores
                # through a pointer never assigned, so the process dies at
                # `str r3, [r4]` with r4 = 0 — a NULL WRITE THAT IS A SYMPTOM OF
                # AN UNCAUGHT EXCEPTION, NOT OF A MISSING WIRELESS DEVICE.
                #
                # That distinction cost most of a week. The faulting instruction
                # is in wireless code, so every round of guessing looked for
                # something wireless that was missing — and each fix (fake
                # ConnMan, wpa_supplicant, P2P off, /LF/System/Wireless) was
                # plausible, applied cleanly, and changed nothing. Reading the
                # thrown object took one run and named the service outright.
                # tools/gdb-dbus-error.py is that reader; keep it.
                #
                # dbus-monitor could never have shown this: it does not report
                # the bus daemon's own replies to calls addressed to the daemon,
                # and an activation failure is exactly such a reply. "No error
                # on the bus" was evidence of nothing.
                #
                # rcS says why the device starts it, on line 246:
                #     #Start avahi (needed for P2P gaming)
                # LeapPads find each other over mDNS. So this is the Pet Chat /
                # peer-to-peer stack, and EVERY title pays for it at startup
                # whether or not it has multiplayer.
                #
                # THE REAL /usr/sbin/avahi-daemon IS IN THE ROOTFS AND IS NOT
                # USED, for the same reason connmand is not: under qemu-user it
                # is not sandboxed, and the paths it cares about are the
                # DEVELOPER'S.
                #
                # It was tried. qemu's -L only redirects a path that ALREADY
                # EXISTS in the sysroot, and /var/run/avahi-daemon/pid did not,
                # so the daemon read the HOST's and found the developer's own:
                #
                #     [mdns] Daemon already running on PID 4806
                #
                # Pre-creating that file fixes that much. What cannot be fixed
                # from here is the socket: the shim does not intercept bind(),
                # so /var/run/avahi-daemon/socket resolves to the host's — and
                # avahi unlink()s it before binding. On a machine where we had
                # the rights, booting the emulator would quietly break the
                # desktop's own mDNS. TADPOLE_REAL_AVAHI=1 is the way back for
                # anyone who wants it on a machine with no avahi of its own.
                if [ -n "${TADPOLE_REAL_AVAHI:-}" ] &&
                   [ -x "$ROOTFS/usr/sbin/avahi-daemon" ]; then
                    echo "tadpole: TADPOLE_REAL_AVAHI=1 — starting the REAL avahi-daemon." >&2
                    echo "  It will use the HOST's /var/run/avahi-daemon. If this machine" >&2
                    echo "  runs its own avahi, expect one of the two to lose." >&2
                    mkdir -p "$SYSROOT/var/run/avahi-daemon"
                    : > "$SYSROOT/var/run/avahi-daemon/pid"
                    guest /usr/sbin/avahi-daemon \
                          --no-rlimits --no-drop-root --no-chroot \
                          2>&1 | sed -u 's/^/[mdns] /' &
                    sleep 1
                elif [ -x "$HERE/runtime/tadpole-avahi" ]; then
                    guest "$HERE/runtime/tadpole-avahi" --address "$sys_addr" \
                        2>&1 | sed -u 's/^/[mdns] /' &
                    sleep 1
                else
                    echo "tadpole: runtime/tadpole-avahi is not built — NO BRIO TITLE" >&2
                    echo "  WILL START. Build it with: cd tadpole && make avahi" >&2
                fi
            }
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
