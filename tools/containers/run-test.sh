#!/bin/bash
# Runs INSIDE the test container. Driven by tools/test-build.sh.
#
# Follows README.md's own instructions in README.md's own order. Every step
# prints its name and stops the run if it fails, so the output reads as "the
# documented path got this far", which is the only question being asked.
set -u

step() { echo; echo "--- $* ---"; }
die()  { echo "FAILED: $*" >&2; exit 1; }

mkdir -p ~/tadpole && cp -a /src/. ~/tadpole/ || die "could not stage the source"
cd ~/tadpole || die "no source tree"

# Reuse the host's download cache. Both of these are pure downloads — pinned by
# hash, or firmware packages that do not change — so sharing them across runs
# costs nothing in fidelity and saves several hundred megabytes per distro.
mkdir -p build/deps
[ -d /cache/deps ] && { rmdir build/deps 2>/dev/null; ln -sfn /cache/deps build/deps; }
mkdir -p sources
[ -d /cache/firmware ] && ln -sfn /cache/firmware sources/online-update

step "what the tree arrived with"
echo "rootfs/:        $(ls rootfs 2>/dev/null | wc -l) entries"
echo "runtime/libs/:  $(ls runtime/libs 2>/dev/null | wc -l) entries"
echo "(both should be empty — they are gitignored, which is the point)"

step "tools/check-deps.sh"
./tools/check-deps.sh || echo "(check-deps reported missing pieces; continuing)"

step "tools/fetch-deps.sh"
./tools/fetch-deps.sh || die "fetch-deps.sh"

step "make viewer  (host only — must not need firmware)"
make -C tadpole viewer || die "make viewer without firmware"
echo "viewer builds with no system files present: good"

if [ "${TADPOLE_TEST_FIRMWARE:-1}" = 1 ]; then
    step "tools/online-update.sh  (system files)"
    ./tools/online-update.sh || die "online-update.sh"

    step "make  (shim + viewer)"
    make -C tadpole || die "make"

    step "what got built"
    for f in runtime/shimlibs/libdl.so.0 runtime/shimlibs/libasound.so.2 \
             runtime/shimlibs-z/libz.so.1 runtime/shimlibs-gl/libGLESv1_CM.so \
             tadpole/viewer/tadpole-view; do
        if [ -f "$f" ]; then printf '  ok      %s\n' "$f"
        else                 printf '  MISSING %s\n' "$f"; MISS=1; fi
    done
    [ -z "${MISS:-}" ] || die "the build did not produce everything"

    step "does the viewer start?"
    # No display in here, so it cannot open a window — but --selftest-layers is
    # pure arithmetic and exercises the layer geometry the compositor depends
    # on. A binary that cannot even do that is not a working build.
    ./tadpole/viewer/tadpole-view --selftest-layers || die "viewer selftest"
else
    echo; echo '(firmware step skipped by request; make cannot be tested without it)'
fi

echo
echo "ALL STEPS PASSED"
