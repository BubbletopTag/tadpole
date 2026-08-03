#!/bin/bash
# Sample the guest's framebuffer while YOU play, then summarise what rendered.
#
#   ./tools/watch-race.sh OUTDIR [seconds] [interval]
#
# Deliberately does NOT drive the emulator. Navigating menus from a script kept
# costing more than it was worth — the selection ring is animation-dependent and
# a race is several levels deep — while a person reaches Driving School in
# seconds. This just watches.
#
# It copies the raw framebuffer arena rather than encoding PNGs inline: a
# pure-Python PNG encode每 second starved the viewer of CPU and dropped it to
# ~1 fps, which then looked like an emulator problem. Copying 512 KB is free.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: watch-race.sh OUTDIR [seconds] [interval]}"
SECS="${2:-60}"
IVAL="${3:-2}"
D="${TADPOLE_DIR:-/tmp/tadpole-hle}"

mkdir -p "$OUT"
[ -e "$D/state.bin" ] || { echo "no guest at $D — start the emulator first" >&2; exit 1; }

echo "sampling $D every ${IVAL}s for ${SECS}s — play now"
n=0
end=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$end" ]; do
    n=$((n+1))
    cp "$D/state.bin" "$OUT/s$(printf %03d $n).bin" 2>/dev/null
    cp "$D/fb0.bin"   "$OUT/f$(printf %03d $n).bin" 2>/dev/null
    sleep "$IVAL"
done
echo "captured $n samples -> $OUT"
echo "now run:  ./tools/framestats.py --raw $OUT"
