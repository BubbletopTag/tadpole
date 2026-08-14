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
# A worker gets its own sysroot so concurrent guests do not share /tmp,
# /flags and save data — see tools/worker-sysroot.sh for why that matters.
SYSROOT="${COMPAT_SYSROOT:-$PROJ/runtime/sysroot}"
PF="$SYSROOT/LF/Bulk/ProgramFiles"
TDIR="/tmp/tadpole-compat-$$"

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
[ -f "$TSV" ] || printf 'pkg\tname\tkind\tverdict\tearly_lit\tearly_col\tlate_lit\tlate_col\talive\tsignal\tsite\tnote\temu\tended\temufault\n' > "$TSV"

# WHICH EMULATOR IS UNDER TEST. Whatever tad_qemu() would run — glasspole in
# any checkout that has one built — so a sweep with no arguments measures what
# users actually get. Set COMPAT_EMU to sweep the other one:
#
#   COMPAT_EMU="$(command -v qemu-arm)" ./tools/compat-sweep.sh
#
# tadpole.sh chooses through TADPOLE_QEMU, so the whole front end comes along
# unchanged and the two runs differ in one variable — which is the only way the
# numbers they produce are comparable. The engine's name is recorded in the
# `emu` column of every row, so an old run is never ambiguous about which one
# it was: nothing here depends on remembering what the default was that week.
EMU="${COMPAT_EMU:-}"
if [ -n "$EMU" ]; then
    [ -x "$EMU" ] || { echo "no emulator at $EMU" >&2; exit 1; }
else
    EMU="$(PROJ="$PROJ"; . "$HERE/lib-deps.sh"; tad_qemu || true)"
    [ -n "$EMU" ] || { echo "no emulator found" >&2; exit 1; }
fi
# The reaper matches on the process NAME, so it has to be this binary's name
# and not a hardcoded qemu-arm — sweeping glasspole with a reaper that only
# knows about qemu leaves every guest running, and 110 abandoned emulators
# take the machine down long before the sweep ends.
EMU_PROC="$(basename "$EMU")"

reap() {
    local p d
    for p in $(pgrep -x "$EMU_PROC" 2>/dev/null); do
        d=$(tr '\0' '\n' < "/proc/$p/environ" 2>/dev/null |
            grep '^TADPOLE_DIR=' | cut -d= -f2)
        # EXACT match, not a prefix. A prefix would make every worker reap
        # every other worker's guests, which is the one way a parallel sweep
        # can quietly produce nonsense instead of failing.
        [ "$d" = "$TDIR" ] && kill -9 "$p" 2>/dev/null
    done
}
trap 'reap; exit 130' INT TERM

stat_of() {                                   # $1 = TADPOLE_DIR -> "lit col"
    "$HERE/fbshot.py" -d "$1" --stat 2>/dev/null |
        sed -n 's/lit \([0-9]*\)% colours \([0-9]*\)/\1 \2/p'
}

# WHAT THE EMULATOR ITSELF SAID WENT WRONG.  $1 = run.log -> one line, or ""
#
# THE SHIM'S crash.log DOES NOT APPEAR UNDER GLASSPOLE, and assuming it did
# would have quietly wrecked the comparison. That file is written by a SIGSEGV
# handler inside the guest; glasspole's on_guest_fault logs the fault and
# returns rather than delivering a guest signal, so the handler never runs. A
# sweep that only looks for crash.log therefore scores every glasspole crash
# as "blank" — the emulator would have come out looking less crashy than qemu
# while actually being worse, which is the exact wrong answer.
#
# So read what glasspole prints instead. Each of these is a distinct failure
# with a distinct fix, and keeping them apart is most of the value of the run:
#
#   GUEST FAULT           unmapped memory — the segfault equivalent
#   HOST FAULT            outside the guest entirely: an emulator bug
#   could not translate   an ARM instruction dynarmic does not implement
#   exception N at pc     the CPU refused the instruction
#   WATCHDOG              200M instructions with no syscall: spinning
#   clone without CLONE_THREAD    fork(), which glasspole does not do
#   could not load        never got as far as running
#   abort()               the guest called abort and nothing caught it
#   unimplemented syscall a syscall glasspole does not have
#
# BY PRIORITY, NOT BY POSITION IN THE FILE. Searching for "first line that
# matches anything" reports whichever failure happened EARLIEST, and the
# earliest is routinely the least interesting: a title that logs an
# unimplemented syscall at startup and then dies on unmapped memory thirty
# seconds later gets filed under the syscall, and the fault that actually
# killed it never appears in the counts.
emu_fault() {
    local f pat
    for pat in \
        'HOST FAULT at [^ ]+' \
        'GUEST FAULT: [0-9a-f]+' \
        'could not translate [0-9]+ instruction\(s\) at [0-9a-f]+' \
        'exception [0-9]+ at pc=[0-9a-f]+' \
        'could not load .*' \
        'clone without CLONE_THREAD' \
        'guest exited with status 127' \
        'unimplemented syscall [0-9]+' \
        'WATCHDOG tid [0-9]+'
    do
        f=$(grep -a -m1 -oE "$pat" "$1" 2>/dev/null | head -1)
        [ -n "$f" ] && { printf '%s' "$f"; return 0; }
    done
    return 0
}

# COMPAT_EXACT=1 matches the whole PackageID instead of any substring of it.
#
# Substring matching is right for a human typing "Trolls", and wrong for the
# swarm, which passes each worker its shard as a list of PackageIDs: one ID or
# name containing another makes two workers run the same title. It did —
# "GalleryWidget" is a substring of "StoryGalleryWidget", so 110 titles came
# back as 111 rows, one of them tested twice and the other worker's slot
# wasted.
want() {                                      # $1 = pkg  $2 = name
    [ ${#FILTER[@]} -eq 0 ] && return 0
    local f
    for f in "${FILTER[@]}"; do
        if [ -n "${COMPAT_EXACT:-}" ]; then
            [ "$1" = "$f" ] && return 0
        else
            case "$1" in *"$f"*) return 0 ;; esac
            case "$2" in *"$f"*) return 0 ;; esac
        fi
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
    cd="$OUT/shots/$pkg"; mkdir -p "$cd"
    reap; rm -rf "$TDIR"; mkdir -p "$TDIR"

    # TADPOLE_GL_SOFTWARE=1 IS NOT A DETAIL, IT IS THE CAVEAT ABOVE MADE
    # EXECUTABLE. --no-viewer means no host GPU, and the host GPU is now the
    # only supported way to render: asking for the software rasteriser by name
    # is the price of a sweep that needs no window. Every verdict this script
    # produces is therefore a SOFTWARE-PATH verdict, and the software path
    # samples one texture unit and ignores the blend factors — so it cannot see
    # a whole class of bug. Two were found by hand this month that every sweep
    # here had scored "ok". Read the results with that in mind.
    TADPOLE_QEMU="$EMU" TADPOLE_DIR="$TDIR" TADPOLE_CRASHDIR="$cd" \
    TADPOLE_SYSROOT="$SYSROOT" TADPOLE_GL_SOFTWARE=1 setsid \
        "$PROJ/tadpole.sh" --app "$pkg" --no-viewer \
        > "$cd/run.log" 2>&1 < /dev/null &
    guest_pid=$!

    sleep "$EARLY"
    read -r el ec <<<"$(stat_of "$TDIR")"
    "$HERE/fbshot.py" -d "$TDIR" "$cd/early.png" >/dev/null 2>&1
    if [ -s "$cd/crash.log" ]; then
        ll=0; lc=0
    else
        sleep $((LATE - EARLY))
        read -r ll lc <<<"$(stat_of "$TDIR")"
        "$HERE/fbshot.py" -d "$TDIR" "$cd/late.png" >/dev/null 2>&1

        # GIVE A SLOW TITLE MORE TIME BEFORE CALLING IT BLANK.
        #
        # The deadline is wall-clock, but how far a title has got by then is
        # not: running four guests at once put Clam Prix at 3445 colours where
        # it reaches 8637 on its own — still loading, not blank. A fixed clock
        # therefore turns contention into false verdicts, and the more workers
        # the worse it reads. So only when the picture still looks empty, wait
        # again and re-sample; a title that was already drawing costs nothing.
        if [ "${lc:-0}" -lt 64 ] || [ "${ll:-0}" -lt 3 ]; then
            local_wait=$(( (LATE - EARLY) * 2 ))
            sleep "$local_wait"
            read -r ll2 lc2 <<<"$(stat_of "$TDIR")"
            if [ "${lc2:-0}" -gt "${lc:-0}" ]; then
                ll="$ll2"; lc="$lc2"
                "$HERE/fbshot.py" -d "$TDIR" "$cd/late.png" >/dev/null 2>&1
            fi
        fi
    fi
    reap; rm -rf "$TDIR"

    sig=""; site=""; alive=""; note=""
    fault="$(emu_fault "$cd/run.log")"
    # Did the guest still exist at the deadline? A title that is spinning and a
    # title that is gone both draw nothing, and only this tells them apart.
    if kill -0 "$guest_pid" 2>/dev/null; then ended=running; else ended=exited; fi

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
        # DREW A REAL SCREEN, and that stays "ok" even if the emulator
        # grumbled. A WATCHDOG line from one spinning worker thread while the
        # title renders happily is a lead worth keeping, not a failure — the
        # fault is recorded in its own column either way.
        verdict=ok
    elif [ "${ec:-0}" -ge 64 ] && [ "${el:-0}" -ge 3 ]; then
        verdict=partial; note="drew early, blank later"
    elif [ -n "$fault" ]; then
        # Nothing on screen AND the emulator said why. That is a crash, however
        # it was reported — this is the branch that keeps glasspole's failures
        # from being scored as the milder "blank".
        verdict=crash; site="$fault"
        [ -n "$note" ] || note="reported by the emulator, not by a guest signal"
    else
        verdict=blank
    fi

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$pkg" "$name" "$kind" "$verdict" "${el:-0}" "${ec:-0}" \
        "${ll:-0}" "${lc:-0}" "$alive" "$sig" "$site" "$note" \
        "$EMU_PROC" "$ended" "$fault" >> "$TSV"
    printf '%-28s %-8s %-8s lit %3s%% / %-5s colours  %s\n' \
        "${name:0:28}" "$kind" "$verdict" "${ll:-0}" "${lc:-0}" "$sig"
done

reap
echo
echo "$total title(s) -> $TSV"
