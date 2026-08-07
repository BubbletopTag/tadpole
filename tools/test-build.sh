#!/bin/bash
# Tadpole — build the project from scratch in a clean container.
#
#   ./tools/test-build.sh                 all three distributions
#   ./tools/test-build.sh ubuntu          just one
#   ./tools/test-build.sh arch fedora
#   ./tools/test-build.sh --shell ubuntu  drop into it instead of testing
#   ./tools/test-build.sh --no-firmware   skip the firmware download
#
# WHY THIS EXISTS
# ---------------
# "It builds for me" is worthless here, because the machine it builds on is the
# machine that already has everything: an extracted rootfs, a populated
# runtime/libs, a shimlibs/ full of objects built months ago. None of that is in
# git. A fresh clone on a fresh machine hits a completely different program.
#
# So the tree that goes into the container is `git archive HEAD` — exactly what
# a clone gets, nothing else. If a step needs something that is neither in git
# nor installed by the documented commands, it fails HERE.
#
# WHAT IT RUNS is the sequence README.md tells a user to run, in that order:
#
#     tools/check-deps.sh
#     tools/fetch-deps.sh
#     tools/online-update.sh      <- system files; the build needs the guest libc
#     cd tadpole && make
#
# THE FIRMWARE STEP IS NOT OPTIONAL and that is the whole point. The shim links
# against the guest's own libc.so.0, so `make` cannot work on a machine that has
# never installed system files. That ordering was wrong in the README for a long
# time, and no amount of building on a developer's machine could have shown it.
#
# Downloads are cached on the HOST (build/deps/cache and the online-update
# staging directory) and bind-mounted in, so the first run is slow and the rest
# are not. Pass --fresh to ignore the cache.

set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
CACHE="$PROJ/build/test-cache"

ENGINE=""
for e in podman docker; do command -v "$e" >/dev/null 2>&1 && { ENGINE="$e"; break; }; done
[ -n "$ENGINE" ] || { echo "need podman or docker" >&2; exit 1; }

DISTROS=()
SHELL_MODE=0
FIRMWARE=1
FRESH=0
for a in "$@"; do
    case "$a" in
        --shell)       SHELL_MODE=1 ;;
        --working)     ;;   # handled below, after SRC is staged
        --no-firmware) FIRMWARE=0 ;;
        --fresh)       FRESH=1 ;;
        -h|--help)     sed -n '2,30p' "$0"; exit 0 ;;
        -*)            echo "unknown option $a" >&2; exit 2 ;;
        *)             DISTROS+=("$a") ;;
    esac
done
[ ${#DISTROS[@]} -gt 0 ] || DISTROS=(ubuntu arch fedora)

[ "$FRESH" = 1 ] && rm -rf "$CACHE"
mkdir -p "$CACHE/deps" "$CACHE/firmware"

# THE TREE, AS A CLONE WOULD SEE IT.
#
# Not a bind mount of the working tree: that would carry rootfs/, runtime/libs
# and every previously built object straight into the "clean" test and make it
# pass for reasons that do not exist anywhere else.
SRC="$(mktemp -d "${TMPDIR:-/tmp}/tadpole-src.XXXXXX")"
trap 'rm -rf "$SRC"' EXIT
if ! git -C "$PROJ" archive HEAD 2>/dev/null | tar x -C "$SRC"; then
    echo "error: 'git archive HEAD' failed — is this a git checkout with a commit?" >&2
    exit 1
fi
# Uncommitted work is the usual case while fixing the build itself. Say so
# rather than silently testing yesterday's code.
if [ -n "$(git -C "$PROJ" status --porcelain 2>/dev/null)" ]; then
    echo "NOTE: you have uncommitted changes; testing HEAD, not the working tree."
    echo "      (tools/test-build.sh --working to use the working tree instead)"
fi
case " $* " in *" --working "*)
    rm -rf "$SRC"; mkdir -p "$SRC"
    tar -C "$PROJ" --exclude=./build --exclude=./rootfs --exclude=./games \
        --exclude=./sources --exclude=./runtime/sysroot --exclude=./runtime/libs \
        --exclude=./runtime/shimlibs --exclude=./runtime/shimlibs-z \
        --exclude=./.git -cf - . | tar x -C "$SRC"
    echo "NOTE: testing the WORKING TREE (build products excluded)." ;;
esac

run_one() {                                   # $1 = distro
    local d="$1" img="tadpole-build-$1" df="$HERE/containers/Dockerfile.$1" rc
    [ -f "$df" ] || { echo "no $df"; return 2; }

    echo
    echo "==================================================================="
    echo "  $d"
    echo "==================================================================="
    "$ENGINE" build -q -t "$img" -f "$df" "$HERE/containers" >/dev/null || {
        echo "  IMAGE BUILD FAILED"; return 1; }

    local args=(--rm
        -v "$SRC:/src:ro,Z"
        -v "$CACHE/deps:/cache/deps:Z"
        -v "$CACHE/firmware:/cache/firmware:Z"
        -e "TADPOLE_TEST_FIRMWARE=$FIRMWARE")
    if [ "$SHELL_MODE" = 1 ]; then
        "$ENGINE" run -it "${args[@]}" "$img" bash -lc \
            'mkdir -p ~/tadpole && cp -a /src/. ~/tadpole/ && cd ~/tadpole && exec bash'
        return $?
    fi
    "$ENGINE" run "${args[@]}" -v "$HERE/containers/run-test.sh:/run-test.sh:ro,Z" \
        "$img" bash /run-test.sh
    rc=$?
    if [ "$rc" = 0 ]; then echo "  $d: PASS"; else echo "  $d: FAIL (exit $rc)"; fi
    return $rc
}

declare -A RESULT
fail=0
for d in "${DISTROS[@]}"; do
    if run_one "$d"; then RESULT[$d]=PASS; else RESULT[$d]=FAIL; fail=1; fi
done

echo
echo "==================================================================="
for d in "${DISTROS[@]}"; do printf '  %-10s %s\n' "$d" "${RESULT[$d]:-SKIP}"; done
echo "==================================================================="
exit $fail
