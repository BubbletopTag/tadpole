#!/bin/bash
# Tadpole — boot, sign in, launch a game, then tap a SEQUENCE of points.
#
#   ./tools/probe-seq.sh OUTDIR "X,Y[,delay] X,Y[,delay] ..."
#
# probe-launch.sh only gets as far as a title's first screen. Anything deeper —
# Options, Credits, a minigame — needs several taps, and each one has to be
# given time to load before the next. A screenshot is captured after every tap
# so a wrong guess is visible instead of silently derailing the run.
#
# Coordinates are FRAMEBUFFER pixels (480x272), same as tools/tap.py.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
OUT="${1:?usage: probe-seq.sh OUTDIR \"x,y[,delay] ...\"}"
SEQ="${2:-}"
export TADPOLE_DIR="${TADPOLE_DIR:-/tmp/tadpole-seq}"

mkdir -p "$OUT"
LOG="$OUT/launch.log"

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

for i in $(seq 1 40); do
    sleep 1
    grep -qa "LoadIconImage----------icon6" "$LOG" && break
done
sleep 6

n=0
"$HERE/fbshot.py" "$OUT/step$(printf %02d $n)-home.png" >/dev/null 2>&1 || true

# A step is either a tap "X,Y[,delay]" or a button "key:NAME[,delay]".
# Leapster titles are button driven, so most navigation is the latter.
for step in $SEQ; do
    n=$((n+1))
    case "$step" in
        # Just hold still and capture — useful after a find: step, whose capture
        # happens before the screen has finished transitioning.
        wait:*)
            D="${step#wait:}"
            echo "step $n: hold ${D}s"
            ;;
        key:*)
            spec="${step#key:}"
            K="${spec%%,*}"
            if [ "$spec" = "$K" ]; then D=6; else D="${spec#*,}"; fi
            echo "step $n: key $K then wait ${D}s"
            "$HERE/key.py" "$K" >/dev/null
            ;;
        # Closed-loop: press KEY until the on-screen label matches REF, then A.
        # "find:<ref.png>@<key>[,delay]" — see tools/nav-label.py for why
        # counting presses does not work on these menus.
        # "find:<ref.png>@<key[,key...]>#<delay>" — the key LIST is comma
        # separated, so the delay uses '#'. Splitting on ',' fed the second key
        # to sleep and broke the step.
        find:*)
            spec="${step#find:}"
            REF="${spec%%@*}"; rest="${spec#*@}"
            K="${rest%%#*}"
            if [ "$rest" = "$K" ]; then D=8; else D="${rest#*#}"; fi
            echo "step $n: navigate to $(basename "$REF" .png) with $K"
            "$HERE/nav-label.py" --ref "$REF" --keys "$K" --max 16 \
                                  --then a -d "$TADPOLE_DIR" || true
            ;;
        # Game-space direction: key.py rotates it to the physical button.
        # Leapster titles are played with the device on its side, so physical
        # "up" is the game's "left". See the note in key.py.
        g:*)
            spec="${step#g:}"
            K="${spec%%,*}"
            if [ "$spec" = "$K" ]; then D=6; else D="${spec#*,}"; fi
            echo "step $n: game-key $K then wait ${D}s"
            "$HERE/key.py" -g "$K" >/dev/null
            ;;
        *)
            X="${step%%,*}"; rest="${step#*,}"
            Y="${rest%%,*}"
            if [ "$rest" = "$Y" ]; then D=14; else D="${rest#*,}"; fi
            echo "step $n: tap fb($X,$Y) then wait ${D}s"
            "$HERE/tap.py" "$X" "$Y" >/dev/null
            ;;
    esac
    sleep "$D"
    "$HERE/fbshot.py" "$OUT/step$(printf %02d $n).png" >/dev/null 2>&1 || true
done

echo
echo "=== GL warnings file ==="
cat "$TADPOLE_DIR/gl-warnings.log" 2>/dev/null | sort | uniq -c | sort -rn | head -20 || echo "  (none)"
echo
echo "=== GL diagnostics ==="
grep -aoE "glGenTextures EXHAUSTED[^\n]*|UNTEXTURED DRAW[^\n]*|UNSUPPORTED compressed format [0-9]+|paletted OK[^\n]*" \
     "$LOG" | sort | uniq -c | sort -rn | head -25
echo
echo "=== crashes ==="
# `grep -c` exits 1 when the count is zero, which would make a clean run look
# like a failed script. Report the number and always succeed.
grep -acE "Segmentation|uncaught|ASSERT" "$LOG" || true
exit 0
