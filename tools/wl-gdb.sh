#!/bin/bash
# Attach to the BrioWrapper the shim parked on a gdb port, and read the
# DBus::Error that libWireless throws. See tools/gdb-dbus-error.py.
#
#   TADPOLE_GDB_MATCH=BrioWrapper TADPOLE_GDB_PORT=1234 ./tadpole.sh --boot
#   ./tools/wl-gdb.sh [port]
#
# SYMBOLS, WHICH IS THE PART THAT WASTED TWO EARLIER RUNS. The process mixes
# host-path libraries (runtime/libs/...) with guest-path ones (/LF/Base/...).
# `set sysroot <guest tree>` resolves the guest half and loses all 84 host ones;
# `set sysroot /` does the reverse. Neither is right, and a breakpoint on a
# library gdb could not open fails SILENTLY -- it reads as "this function is
# never called", which is a lie that costs a whole round to catch.
#
# The fix is to stop using sysroot to do two jobs: keep it at `/` so host paths
# resolve literally, and hand the guest directories to `solib-search-path`,
# which gdb consults by BASENAME. Every guest library here has a unique
# basename, so both halves resolve at once.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
PORT="${1:-1234}"
GUEST="$PROJ/rootfs/lp3-6.2.0.654/emmc_rfs"

if [ ! -d "$GUEST" ]; then
    echo "no guest tree at $GUEST" >&2; exit 2
fi

# Every directory under the guest tree that actually holds a .so, joined with
# ':'. Enumerated rather than hardcoded because Brio scatters modules across
# /LF/Base/lib, /LF/Base/Brio/Module and several app-private directories.
SEARCH="$(find "$GUEST" -name '*.so*' -printf '%h\n' 2>/dev/null | sort -u | paste -sd:)"

exec gdb -q -batch \
    -ex "set confirm off" \
    -ex "set pagination off" \
    -ex "set architecture arm" \
    -ex "set sysroot /" \
    -ex "set solib-search-path $SEARCH" \
    -ex "target remote :$PORT" \
    -ex "source $HERE/gdb-dbus-error.py" \
    -ex "continue" \
    -ex "continue" \
    -ex "continue"
