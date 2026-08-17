#!/bin/bash
# Tadpole — launch a title over and over and keep every crash.
#
#   ./tools/crash-soak.sh 'Clam Prix'          20 launches
#   ./tools/crash-soak.sh LST3-0x00180025-000000 50
#   ./tools/crash-soak.sh --all 5              every native title, 5 each
#   ./tools/crash-soak.sh --list               what is installed
#
# FOR TITLES THAT SEGFAULT IMMEDIATELY. Those are the cheap ones: a crash that
# costs one launch is worth repeating fifty times, and fifty reports from one
# title say whether the fault site is stable, whether it moves with address
# layout, and whether other titles die in the same place. One crash says none
# of that.
#
# Each launch is its own process, straight into the title — no home screen, no
# sign-in, no tapping coordinates that move when the library changes. That is
# `tadpole.sh --app`, which for a native title now means AppManager with
# TADPOLE_LAUNCH set; see tadpole/shim/tadpole_shim.c for how the substitution
# works. It also means one crash directory per launch, so the count is exact.
#
# HEADLESS by default: a title that dies in its first second dies before the
# window matters, and leaving the viewer out makes a run much faster. Use
# --viewer when chasing something that needs the real display path.

set -u


HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
export TADPOLE_DIR="${TADPOLE_DIR:-/tmp/tadpole-soak}"
GLOG="${XDG_STATE_HOME:-$HOME/.local/state}/tadpole/tadpole.log"

VIEWER=0; ALL=0; ARGS=()
PROJ="$(dirname "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)")"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PF="$PROJ/runtime/sysroot/LF/Bulk/ProgramFiles"

for a in "$@"; do
    case "$a" in
        --viewer) VIEWER=1 ;;
        --all)    ALL=1 ;;
        --list)   ls "$PF" 2>/dev/null; exit 0 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) ARGS+=("$a") ;;
    esac
done

STATE="${XDG_STATE_HOME:-$HOME/.local/state}/tadpole/crashes"
SOAK="$STATE/soak-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$SOAK" || { echo "cannot write $SOAK" >&2; exit 1; }

TITLES=()
if [ "$ALL" = 1 ]; then
    for d in "$PF"/*/; do
        grep -q 'AppSo="[^"]*\.so"' "$d/meta.inf" 2>/dev/null &&
            TITLES+=("$(basename "$d")")
    done
    RUNS="${ARGS[0]:-5}"
else
    [ ${#ARGS[@]} -ge 1 ] || { echo "usage: crash-soak.sh <title|PackageID> [runs]" >&2; exit 2; }
    TITLES=("${ARGS[0]}")
    RUNS="${ARGS[1]:-20}"
fi

# Reap by /proc/<pid>/environ, never `pkill -f`: that pattern also matches the
# shell running this script, and killing your own parent mid-loop looks exactly
# like the emulator dying.
reap() {
    local p d
    for p in $(pgrep -x qemu-arm 2>/dev/null); do
        d=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
            grep '^TADPOLE_DIR=' | cut -d= -f2)
        [ "$d" = "$TDIR" ] && kill -9 "$p" 2>/dev/null
    done
}

echo "soak -> $SOAK"
total=0; crashed=0; blank=0
for t in "${TITLES[@]}"; do
    echo
    echo "=== $t"
    for i in $(seq 1 "$RUNS"); do
        n="$(printf '%03d' "$i")"
        out="$SOAK/$(echo "$t" | tr -c 'A-Za-z0-9._-' '_')-$n"
        TDIR="/tmp/tadpole-soak-$$-$i"
        mkdir -p "$out"; rm -rf "$TDIR"; mkdir -p "$TDIR"

        # VIEWER=1 runs with a window and therefore with host-GPU replay; the
        # default has no window, and host-GPU replay lives in the viewer. Since
        # replay became the only supported path, tadpole.sh refuses --no-viewer
        # unless the deprecated software rasteriser is asked for by name — so
        # ask, but ONLY on the branch that actually has no viewer. Setting it
        # globally would force software on the VIEWER=1 runs too.
        v="--no-viewer"; sw="TADPOLE_GL_SOFTWARE=1"
        [ "$VIEWER" = 1 ] && { v=""; sw=""; }

        # RUN IT IN THE BACKGROUND AND WATCH, rather than `timeout` and hope.
        #
        # Two things have to happen before the guest is killed: a crash has to
        # be noticed as soon as it happens (so a title that dies in two seconds
        # does not hold the sweep for forty), and a title that DOES survive has
        # to be photographed while it is still up. "Did not crash" is a weak
        # claim on its own — a black screen does not crash either — and the
        # screenshot is what turns the sweep into evidence that a title
        # actually launched.
        env $sw TADPOLE_DIR="$TDIR" TADPOLE_CRASHDIR="$out" setsid \
            "$PROJ/tadpole.sh" --app "$t" $v > "$out/run.log" 2>&1 < /dev/null &
        deadline=$(( $(date +%s) + ${SOAK_TIMEOUT:-45} ))
        while [ "$(date +%s)" -lt "$deadline" ]; do
            sleep 2
            [ -s "$out/crash.log" ] && break
            running=0
            for q in $(pgrep -x qemu-arm 2>/dev/null); do
                dd=$(tr '\0' '\n' < "/proc/$q/environ" 2>/dev/null |
                     grep '^TADPOLE_DIR=' | cut -d= -f2)
                [ "$dd" = "$TDIR" ] && { running=1; break; }
            done
            [ "$running" = 0 ] && break
        done

        # Photograph before reaping — after the kill there is nothing to see.
        TADPOLE_DIR="$TDIR" "$HERE/fbshot.py" "$out/screen.png" >/dev/null 2>&1
        bright=$("$HERE/fbshot.py" -d "$TDIR" --layers 2>/dev/null |
                 sed -n 's/.*bytes non-zero \([0-9]*\)%.*/\1/p' | sort -rn | head -1)
        reap; rm -rf "$TDIR"
        total=$((total + 1))

        if [ -s "$out/crash.log" ]; then
            crashed=$((crashed + 1))
            sig=$(sed -n 's/^  signal   *\([A-Z]*\).*/\1/p' "$out/crash.log" | head -1)
            pc=$(sed -n 's/^  pc .*  \(.*\)$/\1/p' "$out/crash.log" | head -1)
            al=$(sed -n 's/^  alive    *\([0-9]*\)s.*/\1/p' "$out/crash.log" | head -1)
            printf '  %s  CRASH %-8s %-32s %ss\n' "$n" "${sig:-?}" "${pc:-}" "${al:-?}"
        elif [ "${bright:-0}" -gt 2 ]; then
            printf '  %s  ok        drew something (%s%% of a layer)\n' "$n" "$bright"
            [ "${SOAK_KEEP:-0}" = 1 ] || rm -rf "$out"
        else
            # Alive and blank is its own failure, and the one a crash-only
            # sweep would have called a pass.
            blank=$((blank + 1))
            printf '  %s  BLANK     no crash, nothing on screen\n' "$n"
        fi
    done
done

echo
echo "$total launch(es): $crashed crashed, $blank blank, $((total-crashed-blank)) drew something."
echo
"$HERE/crash-triage.py" "$SOAK"/*/crash.log 2>/dev/null || true
echo
echo "kept in $SOAK"
