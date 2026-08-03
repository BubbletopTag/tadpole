#!/bin/bash
# Tadpole — boot WITH the viewer, sign in, launch a title, capture, report HLE.
#
#   ./tools/probe-hle.sh OUTDIR [extra probe-seq style steps...]
#
# probe-launch.sh and probe-seq.sh run `--no-viewer`, which cannot exercise
# host-GPU replay at all: the replayer lives in the viewer, so with no viewer
# there is no heartbeat and the guest correctly falls back to software. This
# boots the real front end instead and drives it through the same FIFOs, which
# tap.py and key.py write to directly regardless of who is displaying.
#
# The window IS visible while this runs. That is unavoidable — a GL context needs
# a display — but the replay context itself is a hidden window.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
OUT="${1:?usage: probe-hle.sh OUTDIR [steps...]}"
shift || true
STEPS="${*:-}"        # same vocabulary as probe-seq.sh: X,Y | key:N | g:DIR |
                      # find:REF@KEYS#DELAY | wait:SECS
export TADPOLE_DIR="${TADPOLE_DIR:-/tmp/tadpole-hle}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-x11}"

mkdir -p "$OUT"
LOG="$OUT/run.log"

is_ancestor() {
    local p=$$
    while [ "${p:-0}" -gt 1 ]; do
        [ "$p" = "$1" ] && return 0
        p=$(awk '{print $4}' "/proc/$p/stat" 2>/dev/null) || return 1
    done
    return 1
}
reap() {
    for pid in $(pgrep -f "TADPOLE_DIR=$TADPOLE_DIR" 2>/dev/null); do
        is_ancestor "$pid" && continue
        kill -9 "$pid" 2>/dev/null
    done
    for pid in $(pgrep -x tadpole-view 2>/dev/null); do kill "$pid" 2>/dev/null; done
    for pid in $(pgrep -x qemu-arm 2>/dev/null); do kill -9 "$pid" 2>/dev/null; done
    rm -f "$TADPOLE_DIR/.lock"
}
reap; sleep 1
rm -rf "$TADPOLE_DIR"; mkdir -p "$TADPOLE_DIR"

echo "booting viewer + guest -> $LOG"
"$PROJ/tadpole/viewer/tadpole-view" -d "$TADPOLE_DIR" --boot > "$LOG" 2>&1 &
trap 'reap' EXIT

WID=""
for i in $(seq 1 40); do
    WID=$(xdotool search --name '^Tadpole$' 2>/dev/null | tail -1)
    [ -n "$WID" ] && break
    sleep 0.5
done
[ -n "$WID" ] && echo "window $WID" || echo "no window (capture will be skipped)"

for i in $(seq 1 150); do
    sleep 1
    grep -qa "SignIn_mc )" "$LOG" && break
done
grep -qa "SignIn_mc )" "$LOG" || { echo "never reached SignIn" >&2; tail -5 "$LOG" >&2; exit 1; }
sleep 12

echo "sign in"
for try in 1 2 3; do
    "$HERE/tap.py" 355 57 >/dev/null 2>&1
    for i in $(seq 1 15); do
        sleep 1
        grep -qaE "PushState-+ ConnectNag\.swf|ReplaceTopState-+ HomePicker\.swf" "$LOG" && break 2
    done
done

if grep -qa "ConnectNag" "$LOG"; then
    echo "dismiss Connect nag"
    for i in $(seq 1 40); do
        sleep 1
        grep -qa "onLoadInit( _level0.mcContent.ConnectNag_mc )" "$LOG" && break
    done
    sleep 10
    for try in 1 2 3; do
        "$HERE/tap.py" 85 228 >/dev/null 2>&1
        for i in $(seq 1 12); do
            sleep 1
            grep -qa "UIPetLPAD::EnableButtons" "$LOG" && break 2
        done
    done
fi

for i in $(seq 1 40); do
    sleep 1
    grep -qa "LoadIconImage----------icon6" "$LOG" && break
done
sleep 6

echo "launch the installed title"
"$HERE/tap.py" 160 130 >/dev/null 2>&1

# BURST-CAPTURE THE STARTUP SEQUENCE.
#
# A single screenshot 30s after launch only ever shows the steady-state menu, so
# the boot-logo frames — which are the ones rendering black under HLE — were
# never in any capture. Take a rapid series instead: the logos are a few seconds
# of transient frames and they are exactly what needs looking at.
# Run whatever navigation the caller asked for, before the burst capture.
for step in $STEPS; do
    case "$step" in
        wait:*) sleep "${step#wait:}" ;;
        key:*)  spec="${step#key:}"; K="${spec%%,*}"
                if [ "$spec" = "$K" ]; then D=6; else D="${spec#*,}"; fi
                echo "  key $K"; "$HERE/key.py" "$K" >/dev/null 2>&1; sleep "$D" ;;
        g:*)    spec="${step#g:}"; K="${spec%%,*}"
                if [ "$spec" = "$K" ]; then D=6; else D="${spec#*,}"; fi
                echo "  game-key $K"; "$HERE/key.py" -g "$K" >/dev/null 2>&1; sleep "$D" ;;
        find:*) spec="${step#find:}"; REF="${spec%%@*}"; rest="${spec#*@}"
                K="${rest%%#*}"
                if [ "$rest" = "$K" ]; then D=8; else D="${rest#*#}"; fi
                echo "  navigate to $(basename "$REF" .png)"
                "$HERE/nav-label.py" --ref "$REF" --keys "$K" --max 16 \
                                     --then a -d "$TADPOLE_DIR" || true
                sleep "$D" ;;
        *)      X="${step%%,*}"; rest="${step#*,}"; Y="${rest%%,*}"
                if [ "$rest" = "$Y" ]; then D=10; else D="${rest#*,}"; fi
                echo "  tap fb($X,$Y)"; "$HERE/tap.py" "$X" "$Y" >/dev/null 2>&1
                sleep "$D" ;;
    esac
done

# CAPTURE FROM THE FRAMEBUFFER FILE, NOT THE X WINDOW.
#
# `import -window` grabs the X window and blocks the viewer for about a second
# each time: 54 stalls and 31 rendered frames in a whole run, i.e. the harness
# throttled the emulator to ~1 Hz and then reported it as slow. fbshot.py reads
# $TADPOLE_DIR/fb0.bin — the same shared arena the compositor uses — so it costs
# the emulator nothing and shows exactly what the host replayed.
#
# HLE_NOCAP=1 skips captures entirely.
for i in $(seq -w 1 24); do
    [ -z "${HLE_NOCAP:-}" ] && "$HERE/fbshot.py" "$OUT/t$i.png" >/dev/null 2>&1
    sleep 1
done
cp "$OUT/t24.png" "$OUT/screen.png" 2>/dev/null
[ -n "$WID" ] && import -window "$WID" "$OUT/window.png" 2>/dev/null
"$HERE/fbshot.py" "$OUT/fb.png" >/dev/null 2>&1
echo "captured $OUT/t01..t24.png"

echo
echo "=== HLE ==="
grep -aE "^\[hle\]|hle:|HLE replay|DESYNC|bad packet" "$LOG" | sort | uniq -c | head -20
python3 - <<PY
import struct
try:
    d = open("$TADPOLE_DIR/glcmd.bin","rb").read(40)
    m,v,rb,hd,tl,fs,fd,alive = struct.unpack("<8I", d[:32])
    print("  ring: head %d tail %d  frames sent %d done %d  alive %d" % (hd,tl,fs,fd,alive))
    print("  %s" % ("head==tail, clean" if hd==tl else "MISMATCH: host did not consume everything"))
except Exception as e:
    print("  no ring:", e)
PY
echo "=== crashes ==="
grep -acE "Segmentation|uncaught|ASSERT" "$LOG" || true
exit 0
