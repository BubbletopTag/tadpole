#!/bin/bash
# Tadpole — launch a title over and over and keep every crash.
#
#   ./tools/crash-soak.sh 160,130            tap that tile 20 times
#   ./tools/crash-soak.sh 160,130 50         50 times
#   ./tools/crash-soak.sh --tiles            show the home screen and its grid
#   ./tools/crash-soak.sh 160,130 20 --viewer
#
# FOR TITLES THAT SEGFAULT IMMEDIATELY. Those are the cheap ones: a crash that
# costs a tap is worth repeating fifty times, and fifty reports from one title
# say whether the fault site is stable, whether it moves with address layout,
# and whether other titles die in the same place. One crash says none of that.
#
# WHY IT TAPS A TILE INSTEAD OF LAUNCHING THE BINARY. Native Brio titles cannot
# be started directly — `tadpole.sh --app` handles Flash entry points only, and
# a native one needs AppManager to dlopen its App.so and call CreateApp. There
# is no launcher binary in /LF/Base/bin that takes a package as an argument.
# So the home screen is the only door, and a tap is how you open it.
#
# IT BOOTS ONCE. A title crashing does not take AppManager with it — the guest
# survives, unwinds and returns to the home screen — so the next launch is
# another tap rather than another two-minute boot. That is the difference
# between fifty samples in four minutes and fifty in two hours. If the guest
# does die, this notices and boots again rather than tapping at nothing.
#
# TILE COORDINATES ARE HARDCODED, and that is a known weakness: they move when
# the library changes. `--tiles` prints the home screen so you can read the
# right numbers off it. (Fixing this properly is the find-tile item: match a
# package's own icon against the capture.)
#
#     SneakPeeks(160,45)  MyStuff(245,45)  Cartridge(335,45)
#     game 1    (160,130) MyBooks(245,130) PetPad  (335,130)
#     game 2    (160,215) Camera (245,215) Music   (335,215)

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
export TADPOLE_DIR="${TADPOLE_DIR:-/tmp/tadpole-soak}"
GLOG="${XDG_STATE_HOME:-$HOME/.local/state}/tadpole/tadpole.log"

VIEWER=0; TILES=0; ARGS=()
for a in "$@"; do
    case "$a" in
        --viewer) VIEWER=1 ;;
        --tiles)  TILES=1 ;;
        -h|--help) sed -n '2,35p' "$0"; exit 0 ;;
        *) ARGS+=("$a") ;;
    esac
done

TAP="${ARGS[0]:-160,130}"
RUNS="${ARGS[1]:-20}"
TX="${TAP%%,*}"; TY="${TAP##*,}"

STATE="${XDG_STATE_HOME:-$HOME/.local/state}/tadpole/crashes"
SOAK="$STATE/soak-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$SOAK" || { echo "cannot write $SOAK" >&2; exit 1; }

# Reap by /proc/<pid>/environ, never `pkill -f`: that pattern also matches the
# shell running this script, and killing your own parent mid-loop looks exactly
# like the emulator dying.
reap() {
    local p d
    for p in $(pgrep -x qemu-arm 2>/dev/null); do
        d=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
            grep '^TADPOLE_DIR=' | cut -d= -f2)
        [ "$d" = "$TADPOLE_DIR" ] && kill -9 "$p" 2>/dev/null
    done
    rm -f "$TADPOLE_DIR/.lock"
}
# Is one of OUR guests still running? Same /proc/environ match as reap().
alive() {
    local p d
    for p in $(pgrep -x qemu-arm 2>/dev/null); do
        d=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
            grep '^TADPOLE_DIR=' | cut -d= -f2)
        if [ "$d" = "$TADPOLE_DIR" ]; then
            return 0
        fi
    done
    return 1
}

boot() {
    reap; sleep 1
    rm -rf "$TADPOLE_DIR"; mkdir -p "$TADPOLE_DIR"
    : > "$GLOG" 2>/dev/null || true
    local v="--no-viewer"; [ "$VIEWER" = 1 ] && v=""
    TADPOLE_CRASHDIR="$SOAK/boot" setsid "$PROJ/tadpole.sh" --boot $v \
        > "$SOAK/boot.log" 2>&1 < /dev/null &
    disown
    local i
    for i in $(seq 1 150); do
        sleep 1
        grep -qa "SignIn_mc )" "$SOAK/boot.log" "$GLOG" 2>/dev/null && break
    done
    grep -qa "SignIn_mc )" "$SOAK/boot.log" "$GLOG" 2>/dev/null || return 1
    sleep 14
    local t
    for t in 1 2 3 4; do
        "$HERE/tap.py" 355 57 >/dev/null 2>&1
        for i in $(seq 1 12); do
            sleep 1
            grep -qaE "PushState-+ ConnectNag\.swf|ReplaceTopState-+ HomePicker\.swf" \
                 "$SOAK/boot.log" "$GLOG" 2>/dev/null && break 2
        done
    done
    # The nag is dismissed against the PICTURE, not a log marker:
    # UIPetLPAD::EnableButtons fires when the picker loads, which is BEFORE the
    # nag goes up, so a script that trusts it taps a dialog that is still
    # opening and every later tap lands on the dialog.
    for t in $(seq 1 12); do
        "$HERE/fbshot.py" --probe 70,212,32,32,CC2222 >/dev/null 2>&1 || break
        "$HERE/tap.py" 85 228 >/dev/null 2>&1
        sleep 3
    done
    sleep 6
    return 0
}

if [ "$TILES" = 1 ]; then
    boot || { echo "never reached the home screen — see $SOAK/boot.log" >&2; exit 1; }
    "$HERE/fbshot.py" "$SOAK/home.png" >/dev/null 2>&1
    echo "home screen -> $SOAK/home.png"
    echo "read the tile you want off it, then: crash-soak.sh X,Y [runs]"
    reap
    exit 0
fi

echo "soak -> $SOAK"
echo "tapping fb($TX,$TY) $RUNS times"
boot || { echo "never reached the home screen — see $SOAK/boot.log" >&2; reap; exit 1; }

crashed=0; reboots=0
for i in $(seq 1 "$RUNS"); do
    n="$(printf '%03d' "$i")"
    out="$SOAK/$n"; mkdir -p "$out"

    if ! alive; then
        reboots=$((reboots + 1))
        printf '  %s  guest gone — rebooting\n' "$n"
        boot || { echo "  reboot failed; stopping" >&2; break; }
    fi

    before=$(cat "$SOAK/boot/crash.log" 2>/dev/null | wc -l)
    "$HERE/tap.py" "$TX" "$TY" >/dev/null 2>&1
    sleep 12
    after=$(cat "$SOAK/boot/crash.log" 2>/dev/null | wc -l)

    if [ "$after" -gt "$before" ]; then
        crashed=$((crashed + 1))
        # Keep only what THIS launch added, so one file is one crash.
        tail -n +$((before + 1)) "$SOAK/boot/crash.log" > "$out/crash.log"
        sig=$(sed -n 's/^  signal   *\([A-Z]*\).*/\1/p' "$out/crash.log" | head -1)
        pc=$(sed -n 's/^  pc .*  \(.*\)$/\1/p' "$out/crash.log" | head -1)
        printf '  %s  CRASH %-8s %s\n' "$n" "${sig:-?}" "${pc:-}"
        # The guest log's tail is the other half of the story: what the title
        # was doing when it died. Cheap to keep, impossible to reconstruct.
        tail -300 "$GLOG" > "$out/guest-tail.log" 2>/dev/null || true
    else
        printf '  %s  no crash\n' "$n"
        rm -rf "$out"
    fi

    # Back to the home screen for the next tap, whether it crashed or not.
    "$HERE/key.py" esc >/dev/null 2>&1 || true
    sleep 4
done

reap
echo
echo "$crashed of $RUNS launch(es) crashed${reboots:+, $reboots reboot(s)}."
echo
"$HERE/crash-triage.py" "$SOAK"/*/crash.log 2>/dev/null || true
echo
echo "kept in $SOAK"
