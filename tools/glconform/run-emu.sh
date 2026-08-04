#!/bin/bash
# Run glconform inside the emulator against our shim.
#
#   ./tools/glconform/run-emu.sh [OUTFILE]      -> tools/glconform/emu.log
#   TADPOLE_GL_HLE=1 ./tools/glconform/run-emu.sh   (needs a viewer running)
#
# By default this runs against the SOFTWARE path, with no viewer. That is not a
# compromise: every test here is about state, error codes and readback, none of
# which the HLE replay is involved in, and a run that needs no window is a run
# that works over ssh and in a loop.
#
# STDOUT AND STDERR ARE KEPT APART, and that matters more than it looks.
# glconform's RESULT lines go to stdout through libc's buffered printf; the
# shim's own "[gl] UNIMPLEMENTED ..." lines go straight to fd 2, unbuffered.
# Merged with 2>&1 they interleave MID-LINE, which does not just reorder the log
# — it splits RESULT lines in half so a plain `grep '^RESULT'` silently loses
# them. Two files, joined only where a human reads them.

set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$(dirname "$HERE")")"
OUT="${1:-$HERE/emu.log}"
GLLOG="${OUT%.log}.gl.log"

BIN="$PROJ/runtime/glconform"
[ -x "$BIN" ] || { echo "$BIN not built — run ./tools/glconform/build.sh" >&2; exit 1; }

export TADPOLE_DIR="${TADPOLE_DIR:-/tmp/tadpole-glconform}"
mkdir -p "$TADPOLE_DIR"
rm -f "$TADPOLE_DIR/gl-warnings.log"

# TADPOLE_GL_DEBUG=2 WOULD BREAK THIS RUN, and would look like a glconform bug
# rather than a misconfiguration. Several tests deliberately raise GL errors and
# deliberately call unimplemented entry points; level 2 turns the first of those
# into an abort, so the log would stop after two lines. Recording those events
# is the whole job here.
case "${TADPOLE_GL_DEBUG:-}" in
	2|3|4|5|6|7|8|9)
		echo "note: TADPOLE_GL_DEBUG=$TADPOLE_GL_DEBUG would abort at the first" >&2
		echo "      deliberate error; forcing level 1 for this run." >&2
		export TADPOLE_GL_DEBUG=1 ;;
esac

LIBS="$PROJ/runtime/shimlibs-gl:$PROJ/runtime/shimlibs-z:$PROJ/runtime/shimlibs:$PROJ/runtime/libs"

( cd "$PROJ/runtime/sysroot"
  qemu-arm -s 67108864 -L "$PROJ/runtime/sysroot" \
      -E LD_LIBRARY_PATH="$LIBS" \
      -E TADPOLE_DIR="$TADPOLE_DIR" \
      ${TADPOLE_GL_HLE:+-E TADPOLE_GL_HLE=1} \
      ${TADPOLE_GL_DEBUG:+-E TADPOLE_GL_DEBUG=$TADPOLE_GL_DEBUG} \
      "$BIN" ) > "$OUT.raw" 2> "$GLLOG" || true

grep -E '^(META|EGLINIT|RESULT) ' "$OUT.raw" > "$OUT" || true
rm -f "$OUT.raw"

echo "wrote $OUT ($(wc -l < "$OUT") lines)"
echo "      $GLLOG (shim diagnostics: $(grep -c UNIMPLEMENTED "$GLLOG" 2>/dev/null || echo 0) unimplemented entry points hit)"
if ! grep -q '^META done=1' "$OUT"; then
	echo "NOTE: no 'META done=1' — the run did not reach the end. See $GLLOG." >&2
fi
