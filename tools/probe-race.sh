#!/bin/bash
# Tadpole — drive a title to a chosen screen and capture what happens.
#
#   ./tools/probe-race.sh steps/clamprix-drivingschool.txt [outdir]
#   ./tools/probe-race.sh --calibrate            boot, sign in, shoot, stop
#
# WHY THIS EXISTS. Reaching Clam Prix's Driving School by hand is a boot, a
# sign-in, a nag to dismiss, a tile tap and four menu taps — two minutes of
# clicking before a single frame of 3D exists, repeated for every one-line
# change to the GL shim. Worse, the interesting evidence (`hle:` diagnostics)
# goes to STDERR from the VIEWER process, so it is easy to run the whole thing
# and end up with a log that does not contain the one thing being looked for.
#
# THE STEP FILE. One instruction per line, so a route can be edited without
# touching this script:
#
#     tap X Y             touch the framebuffer point (X,Y), 0.8s hold
#     wait REGEX [SECS]   wait for REGEX in the log (default 60s)
#     sleep SECS          unconditional pause
#     shot NAME           write NAME.png from the shared framebuffer
#     burst -n N -i SECS  sample rapidly, report how much frames differ
#     key NAME            send a button (a, b, l, r, home, back)
#
# Blank lines and # comments are ignored. Every step also gets an automatic
# screenshot, numbered, because a route that goes wrong is only debuggable if
# you can see WHERE it went wrong.
#
# TAP COORDINATES ARE FRAMEBUFFER COORDINATES, not window coordinates: taps go
# straight into the shim's event FIFO via tap.py, bypassing the viewer's window
# mapping entirely. The panel is 480x272 and portrait, so on-screen text in a
# Leapster title reads sideways.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
# Which INSTALL to drive. The project tree has the code but only the stock
# widgets; the AppImage's data directory has the firmware, the profile and the
# actual games. TADPOLE_ROOT points the run at whichever has the content, while
# the helper tools (tap.py, key.py, fbshot.py) still come from this checkout.
ROOT="${TADPOLE_ROOT:-$PROJ}"
STEPS="${1:-}"
OUT="${2:-/tmp/tadpole-race}"
# A RUNTIME DIR PER RUN. Sharing one between runs means the EXIT trap of a
# finishing run reaps the guests of a starting one — two consecutive runs then
# fail in two different ways and it reads as emulator instability rather than
# as the harness attacking itself.
export TADPOLE_DIR="${TADPOLE_DIR:-/tmp/tadpole-race-$$}"
# The whole point is the hle: diagnostics; turning them on here means a capture
# can never be missing them because someone forgot the variable.
export TADPOLE_HLE_DEBUG="${TADPOLE_HLE_DEBUG:-1}"
# HLE IS OPT-IN. Without this the viewer starts, announces "HLE replay on ...",
# and then sits idle while the guest quietly software-rasterises — a run that
# looks like it exercised the GPU path and did not. Nothing in the log says so
# except the ABSENCE of `[hle] encoding to host GPU`.
export TADPOLE_GL_HLE="${TADPOLE_GL_HLE:-1}"
# SILENT BY DEFAULT. A capture run is not something anyone is sitting and
# watching — it is minutes of a title playing itself while you do something
# else, in another room, possibly asleep. It has no business making noise, and
# a guest that outlives its run keeps making it. SDL_AUDIODRIVER=alsa (or
# anything) overrides this if a run is specifically about audio.
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-dummy}"

CALIBRATE=0
[ "$STEPS" = "--calibrate" ] && { CALIBRATE=1; STEPS=""; }

mkdir -p "$OUT"
LOG="$OUT/run.log"
: > "$LOG"

# ---- reaping ---------------------------------------------------------------
# tadpole.sh execs qemu as a GRANDCHILD, so killing the launcher leaves
# AppManager and VideoDaemon alive holding the event FIFO. Two readers split the
# event stream between them and every tap appears to be ignored at random.
#
# EXCLUDE OUR OWN ANCESTORS. `pgrep -f "$TADPOLE_DIR"` matches the shell that
# invoked this script too, and killing a parent kills the pipeline — a run that
# dies with no output and no explanation.
is_ancestor() {
    local p=$$
    while [ "${p:-0}" -gt 1 ]; do
        [ "$p" = "$1" ] && return 0
        p=$(awk '{print $4}' "/proc/$p/stat" 2>/dev/null) || return 1
    done
    return 1
}
reap() {
    local pid
    for pid in $(pgrep -f "TADPOLE_DIR=$TADPOLE_DIR" 2>/dev/null); do
        is_ancestor "$pid" && continue
        kill -9 "$pid" 2>/dev/null
    done
}

shot_n=0
shoot() {                      # $1 = label
    shot_n=$((shot_n+1))
    local f w
    f="$(printf '%s/%02d-%s.png' "$OUT" "$shot_n" "$1")"
    # THE WINDOW FIRST, the arena second.
    #
    # fbshot.py composites the guest's shared framebuffers, and that was the
    # whole picture until the game layer stopped being squeezed back into
    # them: at a render scale above 1 the arena holds a stale copy of the
    # game and only the viewer's window has the real one. Ask the viewer, and
    # fall back to the arena when there is no viewer to ask (--no-viewer runs,
    # or a build without the trigger).
    w="$f"
    echo "$w" > "$TADPOLE_DIR/shot.req" 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        [ -e "$TADPOLE_DIR/shot.req" ] || break
        sleep 0.2
    done
    if [ -s "$f" ]; then
        echo "    shot $f (window)"
    else
        rm -f "$TADPOLE_DIR/shot.req"
        "$HERE/fbshot.py" "$f" >/dev/null 2>&1 && echo "    shot $f (arena)"
    fi
}

waitfor() {                    # $1 = regex  $2 = seconds
    local i n="${2:-60}"
    for i in $(seq 1 "$n"); do
        grep -qaE "$1" "$LOG" && return 0
        sleep 1
    done
    echo "    TIMEOUT waiting for: $1" >&2
    return 1
}

reap
rm -rf "$TADPOLE_DIR"
trap 'reap' EXIT INT TERM

echo "==> booting (log: $LOG)"
# WITH THE VIEWER BY DEFAULT, even though this is an automated run and opens a
# window. HLE replay happens INSIDE the viewer, so --no-viewer does not merely
# hide the picture: it silently switches the guest to the software rasteriser
# and the `hle:` diagnostics this script exists to collect are never produced at
# all. A headless run is still useful — it isolates whether a fault is in the
# shared GL core or only in the replay — so TADPOLE_NO_VIEWER=1 asks for it
# explicitly.
if [ "${TADPOLE_NO_VIEWER:-0}" = 1 ]; then
    echo "    (headless: software rasteriser, no hle: diagnostics)"
    "$ROOT/tadpole.sh" --no-viewer --boot >> "$LOG" 2>&1 &
else
    "$ROOT/tadpole.sh" --boot >> "$LOG" 2>&1 &
fi

# SIGN-IN IS NOT GUARANTEED TO HAPPEN. Some boots go straight to the home
# picker with the profile already chosen — measured: 0 SignIn_mc, 104
# LoadIconImage — and gating only on sign-in aborted a boot that had in fact
# succeeded, with "never reached sign-in" on a log that shows the picker fully
# drawn. Accept either, for the same reason the nag loop below drives toward an
# observable end state rather than modelling the sequence: the loops that follow
# already exit immediately when the picker is up.
waitfor 'onLoadInit\( _level0.mcContent.SignIn_mc \)|LoadIconImage' 150 || {
    echo "never reached sign-in OR the home picker — see $LOG" >&2; exit 1; }
sleep 10
shoot signin

echo "==> signing in"
for try in 1 2 3; do
    "$HERE/tap.py" 355 57 >/dev/null 2>&1
    for i in $(seq 1 15); do
        sleep 1
        grep -qaE 'PushState-+ ConnectNag\.swf|ReplaceTopState-+ HomePicker\.swf' \
             "$LOG" && break 2
    done
done

# GET TO A DRAWN HOME SCREEN, however many obstacles are in the way.
#
# The Connect nag appears on some boots and not others, and waiting on its
# onLoadInit is unreliable — when that wait timed out the script tapped anyway,
# every later tap landed on a screen still covered by the nag, and the run
# failed in a way that looked like the emulator ignoring input. Rather than
# model the sequence, drive toward the OBSERVABLE END STATE: the picker loading
# its icons. Tap the nag's dismiss button, check, repeat.
echo "==> reaching the home screen"
# ASK THE SCREEN, NOT THE LOG.
#
# This loop used to break as soon as `LoadIconImage` appeared. That marker is
# the picker loading its icons — which it does BEHIND the Connect nag, within
# a second of signing in. So the loop exited satisfied while a full-screen
# dialog still covered everything, every later tap landed on the dialog, and
# the run died a minute later at "the tile tap did nothing", which reads as a
# broken emulator rather than a harness that never looked.
#
# The nag's dismiss button is a red rounded square at 65,207 40x40 — measured
# off a capture, not guessed. While that red is on screen, the nag is up.
nag_up() { "$HERE/fbshot.py" --probe 65,207,40,40,C83232 -d "$TADPOLE_DIR" >/dev/null 2>&1; }

for attempt in 1 2 3 4 5 6; do
    if nag_up; then
        "$HERE/tap.py" 83 228 >/dev/null 2>&1     # Connect nag: dismiss
        sleep 5
        nag_up || { echo "    nag dismissed"; break; }
    elif grep -qa 'LoadIconImage' "$LOG"; then
        break
    else
        "$HERE/tap.py" 355 57 >/dev/null 2>&1     # still on sign-in
        sleep 5
    fi
    echo "    attempt $attempt"
done

# PushState is logged long before the movie has pixels, so waiting on it shows a
# half-drawn screen. The icon load is the last thing the picker does.
waitfor 'LoadIconImage' 40
sleep 6
shoot home

if [ "$CALIBRATE" = 1 ]; then
    echo
    echo "Calibration stop. The home screen is in $OUT."
    echo "Read tile positions off it, then write a step file:"
    echo "    tap X Y"
    echo "    wait 'SomeLogMarker'"
    echo "    shot after-launch"
    exit 0
fi

[ -n "$STEPS" ] && [ -f "$STEPS" ] || {
    echo "usage: $0 <stepfile> [outdir]   (or --calibrate)" >&2; exit 2; }

echo "==> running $STEPS"
n=0
waitsecs=90
# READ THE VERB AND THE WHOLE REMAINDER. Splitting into fixed fields breaks
# every pattern containing a space — `wait LoadNewApp path = ...` arrived as
# three fragments and waited for a regex that could never match, then timed out
# ninety seconds later looking exactly like the guest had hung.
while read -r verb rest; do
    case "$verb" in
        ''|'#'*) continue ;;
    esac
    n=$((n+1))
    case "$verb" in
        taptil) # tap until a NEW match appears: X Y REGEX
               set -- $rest
               _x="$1"; _y="$2"; shift 2; _re="$*"
               # A NEW OCCURRENCE, NOT ANY OCCURRENCE.
               #
               # `wait` and the old taptil both asked "does the log contain
               # this?", which is only a usable question for markers that fire
               # once. The home picker logs `ChangePage( 1 )` for every page
               # turn — the argument is the DIRECTION, not the page — so the
               # second page-down matched the first one's line, returned
               # instantly, and the route carried on one page short. It then
               # tapped an empty grid slot and timed out waiting for a launch,
               # which reads exactly like the emulator ignoring input.
               #
               # Counting from the step's own start makes a repeated marker as
               # usable as a unique one, and changes nothing for unique ones:
               # they start at zero either way.
               _seen=$(grep -caE "$_re" "$LOG" 2>/dev/null)
               echo "  [$n] taptil $_x $_y until /$_re/ (seen $_seen so far)"
               # A SINGLE TAP IS NOT RELIABLE. Whether the Connect nag appears
               # shifts the picker's timing by seconds, so a tap can land during
               # an animation and be swallowed — the run then continues past a
               # launch that never happened and every later step is nonsense.
               for _try in 1 2 3 4 5; do
                   "$HERE/tap.py" "$_x" "$_y" >/dev/null 2>&1
                   for _i in $(seq 1 12); do
                       sleep 1
                       [ "$(grep -caE "$_re" "$LOG" 2>/dev/null)" \
                         -gt "$_seen" ] && break 2
                   done
                   echo "      retry $_try"
               done
               shoot "taptil-$_x-$_y" ;;
        tap)   set -- $rest
               echo "  [$n] tap $1 $2"
               "$HERE/tap.py" "$1" "$2" >/dev/null 2>&1
               sleep 2; shoot "tap-$1-$2" ;;
        key)   echo "  [$n] key $rest"
               "$HERE/key.py" $rest >/dev/null 2>&1
               sleep 2; shoot "key-$(echo "$rest" | tr ' ' '-')" ;;
        wait)  echo "  [$n] wait /$rest/ (${waitsecs}s)"
               waitfor "$rest" "$waitsecs" || true
               shoot "wait" ;;
        waitsecs) waitsecs="$rest"; echo "  [$n] timeout now ${waitsecs}s" ;;
        sleep) echo "  [$n] sleep $rest"; sleep "$rest" ;;
        shot)  echo "  [$n] shot $rest";  shoot "$rest" ;;
        burst) echo "  [$n] burst $rest"
               # Standing still, consecutive frames should barely differ. This
               # is the measurable form of "the textures look like they roll".
               "$HERE/burst.py" $rest -d "$TADPOLE_DIR" | tee "$OUT/burst-$n.txt" ;;
        *)     echo "  [$n] unknown verb '$verb'" >&2 ;;
    esac
done < "$STEPS"

echo
echo "==> done. Frames and log in $OUT"
echo "    hle diagnostics:"
grep -aE 'hle: (frame|GL error|drawelements)' "$LOG" | tail -12
