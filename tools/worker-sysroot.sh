#!/bin/bash
# Tadpole — build a throwaway sysroot so many guests can run at once.
#
#   ./tools/worker-sysroot.sh /tmp/tadpole-w3       make (or refresh) one
#   ./tools/worker-sysroot.sh --rm /tmp/tadpole-w3  throw it away
#
# WHY ONE SYSROOT IS NOT ENOUGH. Instances do not just read the guest
# filesystem, they write to it, and they write to the SAME PLACES:
#
#   /tmp/...            the guest's /tmp is redirected into the sysroot by the
#                       shim, so /tmp/splash, /tmp/ui_ready, /tmp/vdaemon_play
#                       and friends are shared state between "separate" runs
#   /LF/Bulk/Data       save data, per player
#   /flags              volume, pointercal, poweron
#
# Two guests sharing those do not fail cleanly — they interleave, and the
# result is a test run whose verdicts depend on what the other worker happened
# to be doing. That is worse than being slow.
#
# WHAT IS SHARED ANYWAY. ProgramFiles is 6.1 GB of installed titles and is
# read-only in practice, so it is symlinked rather than copied; twenty workers
# would otherwise want 122 GB. Only the writable parts are real, and those come
# to about 3 MB each.
#
# tadpole.sh already honours TADPOLE_SYSROOT, so nothing else has to change:
#
#   TADPOLE_SYSROOT=/tmp/tadpole-w3 TADPOLE_DIR=/tmp/tad-w3 ./tadpole.sh --app X

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
SRC="${TADPOLE_SYSROOT_SRC:-$PROJ/runtime/sysroot}"

if [ "${1:-}" = "--rm" ]; then
    [ -n "${2:-}" ] || { echo "usage: worker-sysroot.sh --rm DIR" >&2; exit 2; }
    case "$2" in /tmp/*) rm -rf "$2"; exit 0 ;;
        *) echo "refusing to remove outside /tmp: $2" >&2; exit 1 ;;
    esac
fi

DEST="${1:?usage: worker-sysroot.sh DIR}"
[ -d "$SRC" ] || { echo "no sysroot at $SRC — run the setup wizard first" >&2; exit 1; }

rm -rf "$DEST"
mkdir -p "$DEST/LF/Bulk" || exit 1

# Read-only: symlink straight at the master copy.
for d in bin boot etc Firmware lib linuxrc mnt sbin usr erootfs.md5; do
    [ -e "$SRC/$d" ] && ln -sfn "$SRC/$d" "$DEST/$d"
done
# WHOSE DEVICE THIS WORKER IS. A copy rather than a link, because it is one
# line and because a worker tree outliving its master should still be able to
# say what it was built from. Nothing boots differently without it — the shell
# reads the master's marker — but a stray /tmp/tadpole-w3 that cannot name its
# own device is a puzzle nobody needs.
[ -e "$SRC/.tadpole-device" ] && cp -f "$SRC/.tadpole-device" "$DEST/.tadpole-device"
ln -sfn "$SRC/LF/Base" "$DEST/LF/Base"
for d in ProgramFiles Downloads LanguagePack LanguagePack_en Music settings.cfg; do
    [ -e "$SRC/LF/Bulk/$d" ] && ln -sfn "$SRC/LF/Bulk/$d" "$DEST/LF/Bulk/$d"
done

# Writable: a real copy per worker. Small, and the whole point.
cp -a "$SRC/LF/Bulk/Data" "$DEST/LF/Bulk/Data" 2>/dev/null || mkdir -p "$DEST/LF/Bulk/Data"
for d in flags dev sys proc tmp LF/Cart; do
    if [ -d "$SRC/$d" ]; then cp -a "$SRC/$d" "$DEST/$d" 2>/dev/null || mkdir -p "$DEST/$d"
    else mkdir -p "$DEST/$d"; fi
done
mkdir -p "$DEST/tmp" "$DEST/LF/Cart"

# The no-auto-power-off flag, same as setup-sysroot.sh writes.
: > "$DEST/flags/poweron" 2>/dev/null || true

echo "$DEST"
