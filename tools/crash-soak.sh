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
total=0; crashed=0
for t in "${TITLES[@]}"; do
    echo
    echo "=== $t"
    for i in $(seq 1 "$RUNS"); do
        n="$(printf '%03d' "$i")"
        out="$SOAK/$(echo "$t" | tr -c 'A-Za-z0-9._-' '_')-$n"
        TDIR="/tmp/tadpole-soak-$$-$i"
        mkdir -p "$out"; rm -rf "$TDIR"; mkdir -p "$TDIR"

        v="--no-viewer"; [ "$VIEWER" = 1 ] && v=""
        # SECONDS, not minutes: this is for titles that die immediately, and a
        # title still alive after this long is not the one being hunted. Its
        # run is recorded as "no crash" and the loop moves on.
        TADPOLE_DIR="$TDIR" TADPOLE_CRASHDIR="$out" \
            timeout "${SOAK_TIMEOUT:-45}" "$PROJ/tadpole.sh" --app "$t" $v \
            > "$out/run.log" 2>&1
        rc=$?
        reap; rm -rf "$TDIR"
        total=$((total + 1))

        if [ -s "$out/crash.log" ]; then
            crashed=$((crashed + 1))
            sig=$(sed -n 's/^  signal   *\([A-Z]*\).*/\1/p' "$out/crash.log" | head -1)
            pc=$(sed -n 's/^  pc .*  \(.*\)$/\1/p' "$out/crash.log" | head -1)
            al=$(sed -n 's/^  alive    *\([0-9]*\)s.*/\1/p' "$out/crash.log" | head -1)
            printf '  %s  CRASH %-8s %-34s %ss\n' "$n" "${sig:-?}" "${pc:-}" "${al:-?}"
        elif [ "$rc" = 124 ]; then
            printf '  %s  no crash (still running at timeout)\n' "$n"
            rm -rf "$out"
        else
            # A non-zero exit with no report is its own finding: the guest died
            # without the handler firing, or never started. Keep the log.
            printf '  %s  no crash (exit %s)\n' "$n" "$rc"
            [ "$rc" = 0 ] && rm -rf "$out"
        fi
    done
done

echo
echo "$crashed of $total launch(es) crashed."
echo
"$HERE/crash-triage.py" "$SOAK"/*/crash.log 2>/dev/null || true
echo
echo "kept in $SOAK"
