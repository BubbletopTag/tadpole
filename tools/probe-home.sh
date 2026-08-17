#!/bin/bash
# Boot Tadpole headlessly, sign in, dismiss the Connect nag, and screenshot the
# home screen. Lets us test "does this app appear on the home screen?" in one
# command instead of clicking through the SDL window each time.
#
#   ./tools/probe-home.sh [outdir]
#
# Writes <outdir>/home.png and <outdir>/probe.log, and prints the picker's own
# account of what it decided to show.
#
# Tap targets are in framebuffer coordinates (480x272, panel is portrait so the
# on-screen text reads sideways):
#   (355,57)  the rightmost profile pod on SignIn.swf
#   (85,228)  the red X on ConnectNag.swf
# Both come from reading tools/fbshot.py output, not from guessing.

set -u

# NO VIEWER MEANS NO HOST GPU. Host-GPU replay is the only supported rendering
# path now, so a run that deliberately has no window has to ask for the
# deprecated software rasteriser by name — tadpole.sh refuses otherwise, rather
# than quietly rendering with something that cannot express multitexturing or
# the blend factors. Whatever this script measures, it measures on that path.
export TADPOLE_GL_SOFTWARE=1

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
OUT="${1:-/tmp/tadpole-probe-out}"
export TADPOLE_DIR="${TADPOLE_DIR:-/tmp/tadpole-probe}"

mkdir -p "$OUT"
LOG="$OUT/probe.log"

# Reap every guest bound to OUR dir, not just the launcher.
#
# tadpole.sh execs qemu-arm as a grandchild, so killing the launcher leaves the
# guest running -- and a leftover AppManager keeps reading $TADPOLE_DIR/ev2.
# Two readers on one FIFO split the event stream between them, so taps land in
# whichever process happens to win the read and the probe looks randomly flaky.
# Always start and finish with none of ours alive.
#
# Kill by PID from pgrep, never `pkill -f "$TADPOLE_DIR"` -- that pattern also
# matches this script's own command line and kills the shell running it.
reap() {
    for pid in $(pgrep -f "TADPOLE_DIR=$TADPOLE_DIR" 2>/dev/null); do
        [ "$pid" = "$$" ] && continue
        kill -9 "$pid" 2>/dev/null
    done
    rm -f "$TADPOLE_DIR/.lock"
}
reap
sleep 1

echo "booting (headless) -> $LOG"
# --boot AS WELL AS --no-viewer. tadpole.sh's default became front-end-only —
# it opens the window and waits rather than starting a guest — so `--no-viewer`
# on its own now asks for a front end with no front end, and the script says
# "viewer not built", which is both untrue and a long way from the real
# problem. The probe wants a guest and no window: that is --boot --no-viewer.
"$PROJ/tadpole.sh" --boot --no-viewer ${PROBE_DEBUG:+--debug} > "$LOG" 2>&1 &
BOOT=$!
trap 'kill $BOOT 2>/dev/null; reap' EXIT

# Wait for the movie to LOAD, not merely for the state to be pushed —
# PushState is logged long before SignIn_mc has any pixels on screen.
for i in $(seq 1 120); do
    sleep 1
    grep -qa "onLoadInit( _level0.mcContent.SignIn_mc )" "$LOG" && break
done
grep -qa "onLoadInit( _level0.mcContent.SignIn_mc )" "$LOG" ||
    { echo "never reached SignIn — see $LOG" >&2; exit 1; }

# SignIn.swf appearing in the log only means the state was pushed; the movie
# still has to load and finish its intro before it will accept a tap. Settle,
# then retry the tap until the state actually changes.
sleep 30
"$HERE/fbshot.py" "$OUT/signin.png" >/dev/null
echo "sign in"
for try in 1 2 3; do
    "$HERE/tap.py" 355 57 >/dev/null
    for i in $(seq 1 15); do
        sleep 1
        # Match the STATE TRANSITION, not the word "HomePicker" — the
        # underlying HomePickerState logs ProcessMouseDown for the tap that
        # is still only selecting a profile on SignIn.swf.
        grep -qaE "PushState-+ ConnectNag\.swf|ReplaceTopState-+ HomePicker\.swf" \
             "$LOG" && break 2
    done
    echo "  no response, retrying tap ($try)"
done

if grep -qa "ConnectNag" "$LOG"; then
    echo "dismiss Connect nag"
    for i in $(seq 1 40); do
        sleep 1
        grep -qa "onLoadInit( _level0.mcContent.ConnectNag_mc )" "$LOG" && break
    done
    sleep 10
    for try in 1 2 3; do
        "$HERE/tap.py" 85 228 >/dev/null
        for i in $(seq 1 12); do
            sleep 1
            # GetAllIcons fires when HomePicker loads, BEFORE the nag goes up,
            # so it is not evidence the nag closed. EnableButtons only runs
            # once the picker is back on top.
            grep -qa "UIPetLPAD::EnableButtons" "$LOG" && break 2
        done
        echo "  nag still up, retrying ($try)"
    done
fi
sleep 6

"$HERE/fbshot.py" "$OUT/home.png"

echo
echo "=== picker decisions ==="
grep -aE "getProgramFileApps|numPages|LoadIconData|SetIconLabelText" "$LOG" |
    sed 's/^/  /' | sort -u
