#!/bin/sh
# Glasspole — fetch dynarmic.
#
# Not vendored: it is 44 MB of source and Tadpole's history is already 167 MB.
# Pinned to a commit rather than a branch, because "it worked last week" is not
# a thing you want to have to establish about a JIT.
#
# UPSTREAM IS GONE. github.com/merryhime/dynarmic returns 404 — the project was
# taken down. The yuzu mirror is the same code and carries the same 0BSD
# licence; if it disappears too, Vita3K, PabloMK7 and Borked3DS all keep forks.
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="${GLASSPOLE_DYNARMIC_REPO:-https://github.com/yuzu-mirror/dynarmic.git}"
PIN="${GLASSPOLE_DYNARMIC_PIN:-9d4582339990d4eae53f1dc7160686920fc2075c}"
DEST="$HERE/deps/dynarmic"

if [ -d "$DEST/.git" ]; then
    have="$(git -C "$DEST" rev-parse HEAD)"
    if [ "$have" = "$PIN" ]; then
        echo "dynarmic: already at $PIN"
        exit 0
    fi
    echo "dynarmic: at $have, wanted $PIN — refetching"
    rm -rf "$DEST"
fi

mkdir -p "$HERE/deps"
git clone --recursive "$REPO" "$DEST"
git -C "$DEST" checkout --detach "$PIN"
git -C "$DEST" submodule update --init --recursive
echo "dynarmic: $PIN"
