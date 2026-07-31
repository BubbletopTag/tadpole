#!/bin/bash
# Boot Tadpole headlessly, sign in, and launch a home-screen app by tapping its
# tile. Exists to make native (Brio .so) app launches reproducible — they cannot
# be started from tadpole.sh --app, which only handles Flash entry points.
#
#   ./tools/probe-launch.sh music [outdir]
#
# Tile positions in framebuffer coordinates, read off tools/fbshot.py output of
# the 7-tile home screen (the panel is portrait, so the on-screen text reads
# sideways):
#
#     SneakPeeks(160,45)  MyStuff(245,45)  Cartridge(335,45)
#                         MyBooks(245,130) PetPad(335,130)
#                         Camera(245,215)  Music(335,215)

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
APP="${1:-music}"
OUT="${2:-/tmp/tadpole-launch-out}"
export TADPOLE_DIR="${TADPOLE_DIR:-/tmp/tadpole-probe}"

case "$APP" in
    game)       TX=160; TY=130 ;;   # installed cart game, row 2 col 1
    music)      TX=335; TY=215 ;;
    camera)     TX=245; TY=215 ;;
    petpad)     TX=335; TY=130 ;;
    mybooks)    TX=245; TY=130 ;;
    mystuff)    TX=245; TY=45  ;;
    sneakpeeks) TX=160; TY=45  ;;
    cartridge)  TX=335; TY=45  ;;
    *) echo "unknown app '$APP'" >&2; exit 2 ;;
esac

mkdir -p "$OUT"
LOG="$OUT/launch.log"

# Reap every guest bound to OUR dir. tadpole.sh execs qemu as a grandchild, so
# killing the launcher leaves AppManager holding the input FIFO, and two readers
# split the event stream — which looks exactly like flaky touch.
# Reap every guest bound to OUR dir.
#
# CAREFUL: `pgrep -f "TADPOLE_DIR=$TADPOLE_DIR"` also matches the shell that
# invoked this script, because that string is in ITS command line too. Killing a
# parent kills the pipeline, and the symptom is a run that produces an empty log
# and exit 1 for no visible reason. Guarding with `$$` is not enough — the
# ancestor chain has to be excluded.
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
    rm -f "$TADPOLE_DIR/.lock"
}
reap; sleep 1

echo "booting -> $LOG"
"$PROJ/tadpole.sh" --no-viewer > "$LOG" 2>&1 &
BOOT=$!
trap 'kill $BOOT 2>/dev/null; reap' EXIT

for i in $(seq 1 120); do
    sleep 1
    grep -qa "onLoadInit( _level0.mcContent.SignIn_mc )" "$LOG" && break
done
grep -qa "SignIn_mc )" "$LOG" || { echo "never reached SignIn" >&2; exit 1; }
sleep 12

echo "sign in"
for try in 1 2 3; do
    "$HERE/tap.py" 355 57 >/dev/null
    for i in $(seq 1 15); do
        sleep 1
        grep -qaE "PushState-+ ConnectNag\.swf|ReplaceTopState-+ HomePicker\.swf" \
             "$LOG" && break 2
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
        "$HERE/tap.py" 85 228 >/dev/null
        for i in $(seq 1 12); do
            sleep 1
            grep -qa "UIPetLPAD::EnableButtons" "$LOG" && break 2
        done
    done
fi

# Wait for the picker to finish drawing its tiles before aiming at one.
for i in $(seq 1 40); do
    sleep 1
    grep -qa "LoadIconImage----------icon6" "$LOG" && break
done
sleep 6
"$HERE/fbshot.py" "$OUT/before.png" >/dev/null

echo "launching '$APP' — tapping fb($TX,$TY)"
"$HERE/tap.py" "$TX" "$TY" >/dev/null
sleep 20
"$HERE/fbshot.py" "$OUT/after.png" >/dev/null 2>&1 || true

echo
echo "=== launch trace ==="
grep -aE "LaunchApp|LoadNewApp|ReplaceTopApp|UnloadModule|caught signal|Segmentation|uncaught" \
     "$LOG" | tail -20
