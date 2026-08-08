#!/bin/bash
# Tadpole — launch every installed title in turn and record what it did.
#
#   ./tools/compat-sweep.sh                 every installed title
#   ./tools/compat-sweep.sh LST3-0x0018 …   only ones matching a prefix/name
#   ./tools/compat-sweep.sh --resume        skip titles already in results.tsv
#
# Produces build/compat/<date>/ with, per title, two screenshots and a row in
# results.tsv. tools/compat-report.py turns that into the HTML table.
#
# WHY TWO SCREENSHOTS. One at 12 s and one at 35 s. A single early capture
# marks a healthy-but-slow title blank; a single late one cannot tell "took a
# while" from "instant". Two says which, and the pair is what makes a verdict
# of "blank" trustworthy rather than a timing artefact.
#
# WHAT COUNTS AS DRAWING SOMETHING. Not layer occupancy — that counts non-zero
# BYTES, and an opaque black frame is 25% non-zero from its alpha alone, which
# reads as "97% full" while showing nothing at all. `fbshot.py --stat` reports
# the fraction of pixels that are not near-black and how many distinct colours
# are present; a flat fill has a handful, a title screen has thousands.
#
# Titles are launched STRAIGHT IN, with no home screen and no profile sign-in
# (see TADPOLE_LAUNCH in tadpole/shim/tadpole_shim.c). That is what makes a
# hundred launches practical, and it is also a caveat worth carrying into the
# results: a title that wants a signed-in player may behave differently here.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
PF="$PROJ/runtime/sysroot/LF/Bulk/ProgramFiles"

EARLY="${COMPAT_EARLY:-12}"
LATE="${COMPAT_LATE:-35}"

RESUME=0; FILTER=()
for a in "$@"; do
    case "$a" in
        --resume) RESUME=1 ;;
        -h|--help) sed -n '2,28p' "$0"; exit 0 ;;
        *) FILTER+=("$a") ;;
    esac
done

OUT="${COMPAT_OUT:-$PROJ/build/compat/$(date +%Y%m%d-%H%M%S)}"
[ "$RESUME" = 1 ] && OUT="$(ls -d "$PROJ"/build/compat/*/ 2>/dev/null | tail -1)"
[ -n "$OUT" ] || { echo "nothing to resume" >&2; exit 1; }
mkdir -p "$OUT/shots" || exit 1
TSV="$OUT/results.tsv"
[ -f "$TSV" ] || printf 'pkg\tname\tkind\tverdict\tearly_lit\tearly_col\tlate_lit\tlate_col\talive\tsignal\tsite\tnote\n' > "$TSV"

reap() {
    local p d
    for p in $(pgrep -x qemu-arm 2>/dev/null); do
        d=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
            grep '^TADPOLE_DIR=' | cut -d= -f2)
        case "$d" in /tmp/tadpole-compat-*) kill -9 "$p" 2>/dev/null ;; esac
    done
}
trap 'reap; exit 130' INT TERM

stat_of() {                                   # $1 = TADPOLE_DIR -> "lit col"
    "$HERE/fbshot.py" -d "$1" --stat 2>/dev/null |
        sed -n 's/lit \([0-9]*\)% colours \([0-9]*\)/\1 \2/p'
}

want() {                                      # $1 = pkg  $2 = name
    [ ${#FILTER[@]} -eq 0 ] && return 0
    local f
    for f in "${FILTER[@]}"; do
        case "$1" in *"$f"*) return 0 ;; esac
        case "$2" in *"$f"*) return 0 ;; esac
    done
    return 1
}

total=0
for d in "$PF"/*/; do
    m="$d/meta.inf"; [ -f "$m" ] || continue
    pkg="$(basename "$d")"
    name=$(grep -oE 'Name="[^"]*"' "$m" | head -1 | cut -d\" -f2)
    entry=$(grep -oE 'AppSo="[^"]*"' "$m" | head -1 | cut -d\" -f2)
    [ -n "$entry" ] || continue
    case "$entry" in *.so) kind=native ;; *.swf) kind=flash ;; *) continue ;; esac
    want "$pkg" "$name" || continue
    if [ "$RESUME" = 1 ] && cut -f1 "$TSV" | grep -qx "$pkg"; then continue; fi

    total=$((total + 1))
    TDIR="/tmp/tadpole-compat-$$"
    cd="$OUT/shots/$pkg"; mkdir -p "$cd"
    reap; rm -rf "$TDIR"; mkdir -p "$TDIR"

    TADPOLE_DIR="$TDIR" TADPOLE_CRASHDIR="$cd" setsid \
        "$PROJ/tadpole.sh" --app "$pkg" --no-viewer \
        > "$cd/run.log" 2>&1 < /dev/null &

    sleep "$EARLY"
    read -r el ec <<<"$(stat_of "$TDIR")"
    "$HERE/fbshot.py" -d "$TDIR" "$cd/early.png" >/dev/null 2>&1
    if [ -s "$cd/crash.log" ]; then
        ll=0; lc=0
    else
        sleep $((LATE - EARLY))
        read -r ll lc <<<"$(stat_of "$TDIR")"
        "$HERE/fbshot.py" -d "$TDIR" "$cd/late.png" >/dev/null 2>&1
    fi
    reap; rm -rf "$TDIR"

    sig=""; site=""; alive=""; note=""
    if [ -s "$cd/crash.log" ]; then
        verdict=crash
        sig=$(sed -n 's/^  signal   *\([A-Z]*\).*/\1/p' "$cd/crash.log" | head -1)
        site=$(sed -n 's/^  pc .*  \(.*\)$/\1/p' "$cd/crash.log" | head -1)
        alive=$(sed -n 's/^  alive    *\([0-9]*\)s.*/\1/p' "$cd/crash.log" | head -1)
        # The verbose terminate handler prints what() before aborting, and that
        # single line is usually the whole diagnosis. Keep it.
        note=$(grep -a -m1 -A1 "terminate called" "$cd/run.log" |
               tr '\n' ' ' | sed 's/  */ /g' | cut -c1-160)
    elif [ "${lc:-0}" -ge 64 ] && [ "${ll:-0}" -ge 3 ]; then
        verdict=ok
    elif [ "${ec:-0}" -ge 64 ] && [ "${el:-0}" -ge 3 ]; then
        verdict=partial; note="drew early, blank later"
    else
        verdict=blank
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$pkg" "$name" "$kind" "$verdict" "${el:-0}" "${ec:-0}" \
        "${ll:-0}" "${lc:-0}" "$alive" "$sig" "$site" "$note" >> "$TSV"
    printf '%-28s %-8s %-8s lit %3s%% / %-5s colours  %s\n' \
        "${name:0:28}" "$kind" "$verdict" "${ll:-0}" "${lc:-0}" "$sig"
done

reap
echo
echo "$total title(s) -> $TSV"
