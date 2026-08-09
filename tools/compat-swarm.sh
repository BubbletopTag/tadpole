#!/bin/bash
# Tadpole — run the compatibility sweep across many guests at once.
#
#   ./tools/compat-swarm.sh            as many workers as the machine suits
#   ./tools/compat-swarm.sh 24         exactly 24
#   ./tools/compat-swarm.sh 24 Trolls "PJ Masks"   only matching titles
#
# Sequentially the sweep is about 45 seconds per title, so 110 titles is over
# an hour. The work is embarrassingly parallel — each title is an independent
# process — and the only thing that ever stopped it was shared state, not CPU.
#
# WHAT HAD TO CHANGE TO MAKE THIS SAFE
#
#   * Each worker gets its own sysroot (tools/worker-sysroot.sh). Guests write
#     to /tmp, /flags and LF/Bulk/Data inside it, and the shim redirects the
#     guest's /tmp there — so sharing one sysroot does not fail cleanly, it
#     interleaves, and verdicts start depending on what another worker was
#     doing. The read-only 6.1 GB of ProgramFiles is symlinked, so a worker
#     costs under a megabyte of disk.
#   * The sweep's reaper matches its own TADPOLE_DIR exactly rather than by
#     prefix. By prefix, every worker killed every other worker's guests.
#   * The sweep re-samples a title that still looks blank at the deadline.
#     Contention slows a guest down without slowing the clock, so a fixed
#     deadline turns load into false "blank" verdicts — four workers already
#     put Clam Prix at 3445 colours where it reaches 8637 alone.
#
# NOT CLAUDE AGENTS. The parallelism that matters here is qemu processes, not
# assistants: an agent per title would add minutes of model latency and cost to
# something a background job does instantly. Agents earn their keep on judgement
# — reading the crashes this produces — not on running a loop.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
PF="$PROJ/runtime/sysroot/LF/Bulk/ProgramFiles"

# EACH GUEST IS A qemu-arm WITH A 64 MB STACK plus the mapped title, measured
# at roughly 120-180 MB RSS. Default to something the machine can hold with
# room to spare rather than to the thread count: the limit here is memory
# bandwidth and disk, and past a point more workers only lengthens every
# title's load without finishing sooner.
cpus=$(nproc 2>/dev/null || echo 8)
memgb=$(awk '/MemAvailable/{printf "%d", $2/1048576}' /proc/meminfo 2>/dev/null || echo 8)
auto=$(( cpus / 3 )); [ "$auto" -lt 1 ] && auto=1
memcap=$(( memgb * 1024 / 250 )); [ "$memcap" -lt 1 ] && memcap=1
[ "$auto" -gt "$memcap" ] && auto=$memcap
[ "$auto" -gt 32 ] && auto=32

N="${1:-$auto}"
case "$N" in ''|*[!0-9]*) N="$auto" ;; *) shift ;; esac
[ "$N" -lt 1 ] && N=1

OUT="${COMPAT_OUT:-$PROJ/build/compat/swarm-$(date +%Y%m%d-%H%M%S)}"
mkdir -p "$OUT/shots" || exit 1

# ---- the title list, sharded round-robin ----------------------------------
# Round-robin rather than in blocks: crashes are clustered by engine and
# directory order tracks that, so contiguous blocks would give one worker every
# instant-crash and another every slow-loading storybook.
list=()
for d in "$PF"/*/; do
    m="$d/meta.inf"; [ -f "$m" ] || continue
    grep -qE 'AppSo="[^"]*\.(so|swf)"' "$m" || continue
    pkg="$(basename "$d")"
    if [ $# -gt 0 ]; then
        name=$(grep -oE 'Name="[^"]*"' "$m" | head -1 | cut -d\" -f2)
        keep=0
        for f in "$@"; do
            case "$pkg" in *"$f"*) keep=1 ;; esac
            case "$name" in *"$f"*) keep=1 ;; esac
        done
        [ "$keep" = 1 ] || continue
    fi
    list+=("$pkg")
done
total=${#list[@]}
[ "$total" -gt 0 ] || { echo "no titles matched" >&2; exit 1; }
[ "$N" -gt "$total" ] && N=$total

echo "swarm: $total titles across $N workers  (${cpus} threads, ${memgb} GB free)"
echo "  -> $OUT"

cleanup() {
    local i
    for i in $(seq 1 "$N"); do
        pkill -9 -f "tadpole-swarm-$$-$i" 2>/dev/null
        "$HERE/worker-sysroot.sh" --rm "/tmp/tadpole-swarm-$$-$i" 2>/dev/null
    done
}
trap 'cleanup; exit 130' INT TERM

start=$(date +%s)
for i in $(seq 1 "$N"); do
    (
        sr="/tmp/tadpole-swarm-$$-$i"
        "$HERE/worker-sysroot.sh" "$sr" >/dev/null || exit 1
        mine=()
        j=0
        for pkg in "${list[@]}"; do
            j=$((j + 1))
            [ $(( (j - 1) % N + 1 )) = "$i" ] && mine+=("$pkg")
        done
        [ ${#mine[@]} -gt 0 ] || exit 0
        COMPAT_SYSROOT="$sr" COMPAT_OUT="$OUT/w$i" \
            "$HERE/compat-sweep.sh" "${mine[@]}" > "$OUT/w$i.log" 2>&1
        "$HERE/worker-sysroot.sh" --rm "$sr" 2>/dev/null
    ) &
done
wait
took=$(( $(date +%s) - start ))

# ---- merge -----------------------------------------------------------------
head -1 "$OUT"/w1/results.tsv > "$OUT/results.tsv" 2>/dev/null
for i in $(seq 1 "$N"); do
    [ -f "$OUT/w$i/results.tsv" ] && tail -n +2 "$OUT/w$i/results.tsv" >> "$OUT/results.tsv"
    [ -d "$OUT/w$i/shots" ] && cp -a "$OUT/w$i/shots/." "$OUT/shots/" 2>/dev/null
done
done_n=$(( $(wc -l < "$OUT/results.tsv") - 1 ))

echo
printf '%s of %s titles in %dm %02ds\n' "$done_n" "$total" $((took / 60)) $((took % 60))
cut -f4 "$OUT/results.tsv" | tail -n +2 | sort | uniq -c
echo
echo "$OUT/results.tsv"
