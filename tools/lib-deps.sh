# Tadpole — where the third-party pieces are. Sourced, not run.
#
#   . "$HERE/lib-deps.sh"
#   "$(tad_python)" tools/pkgtool.py ...
#   "$(tad_qemu)" -L "$SYSROOT" ...
#
# There are three places a dependency can live and they are tried in this
# order, because each is a more specific statement of intent than the next:
#
#   1. TADPOLE_QEMU / TADPOLE_PYTHON   an explicit override, wins over all
#   2. $TADPOLE_DEPS or build/deps/    what tools/fetch-deps.sh staged, and
#                                      what the AppImage ships
#   3. the PATH                        the user's own install
#
# Falling back to the PATH matters as much as finding the bundle: a source
# checkout that has never run fetch-deps.sh still works if qemu-user and
# ubi_reader are installed, which is how this project was developed for months.

# The caller sets PROJ; if it did not, derive it from this file.
if [ -z "${PROJ:-}" ]; then
    PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fi
: "${TADPOLE_DEPS:=$PROJ/build/deps}"

# TADPOLE_QEMU/TADPOLE_PYTHON are read relative to the caller's cwd, same as
# any other shell command — but guest() in tadpole.sh does `cd "$SYSROOT"`
# before exec'ing whatever tad_qemu() returned. A relative override
# (TADPOLE_QEMU=glasspole/build/glasspole) would silently resolve against the
# sysroot instead, and fail with a path that looks nothing like what the user
# typed. Absolutize it here, once, so every caller gets a path that still
# works after an unrelated cd.
tad_abspath() {
    readlink -f -- "$1" 2>/dev/null || printf '%s/%s' "$PWD" "$1"
}

# WHICH ENGINE RUNS THE GUEST — GLASSPOLE FIRST, qemu-arm BEHIND IT.
#
# Glasspole is this project's own ARM JIT (see glasspole/). It started as the
# Windows-only option, because qemu-user is Linux-only and Windows had to run
# something; qemu-arm ran everything here. The most recent sweep puts the two
# one-to-one across 110 titles — 85 launches against 82, which is inside the
# run-to-run spread — so the argument for keeping qemu in front had run out.
# The engine we can fix is the one that should be in front of people, and a bug
# nobody hits because it is behind a flag never gets fixed.
#
# qemu-arm is NOT going anywhere: it stays the fallback for a checkout with no
# glasspole built, and it stays the reference the compatibility sweep diffs
# against. Nothing in the tree has stopped supporting it.
#
#   TADPOLE_QEMU="$(command -v qemu-arm)" ./tadpole.sh    for one run on qemu
#
# The name of the variable is now a little wrong — it selects an engine, not a
# qemu — but it is written into saved sweep runs, tools/compat-sweep.sh and
# every set of notes anyone has, so it keeps the name it has always had.
tad_qemu() {
    if [ -n "${TADPOLE_QEMU:-}" ] && [ -x "${TADPOLE_QEMU}" ]; then
        tad_abspath "$TADPOLE_QEMU"; return 0
    fi
    if [ -x "$TADPOLE_DEPS/bin/glasspole" ]; then
        printf '%s' "$TADPOLE_DEPS/bin/glasspole"; return 0
    fi
    # A source checkout builds it in place rather than staging it, so this is
    # where it is for everyone working on the project.
    if [ -x "$PROJ/glasspole/build/glasspole" ]; then
        printf '%s' "$PROJ/glasspole/build/glasspole"; return 0
    fi
    if [ -x "$TADPOLE_DEPS/bin/qemu-arm" ]; then
        printf '%s' "$TADPOLE_DEPS/bin/qemu-arm"; return 0
    fi
    command -v glasspole 2>/dev/null && return 0
    command -v qemu-arm 2>/dev/null && return 0
    return 1
}

# A Python that can run ubi_reader, NOT just any Python. The distinction is the
# whole point: the host may well have python3 and not have ubi_reader, and
# picking it because it exists is how firmware installs used to die halfway
# through with ModuleNotFoundError.
tad_python() {
    local cand
    for cand in "${TADPOLE_PYTHON:+$(tad_abspath "$TADPOLE_PYTHON")}" \
                "$TADPOLE_DEPS/python/bin/python3" \
                "$(command -v python3 2>/dev/null)"; do
        [ -n "$cand" ] && [ -x "$cand" ] || continue
        printf '%s' "$cand"; return 0
    done
    return 1
}

tad_python_with_ubireader() {
    local cand
    for cand in "${TADPOLE_PYTHON:+$(tad_abspath "$TADPOLE_PYTHON")}" \
                "$TADPOLE_DEPS/python/bin/python3" \
                "$(command -v python3 2>/dev/null)"; do
        [ -n "$cand" ] && [ -x "$cand" ] || continue
        "$cand" -c "import ubireader.ubifs.misc" >/dev/null 2>&1 || continue
        printf '%s' "$cand"; return 0
    done
    return 1
}

# Is this Tadpole self-contained? Used by check-deps.sh and the wizard to say
# "bundled" instead of asking for a package the user does not need.
#
# EITHER ENGINE COUNTS. An AppImage that carries glasspole and no qemu-arm is
# exactly as self-contained as one carrying qemu — asking that user to install
# qemu-user would be asking for something the build does not use.
tad_have_bundle() {
    { [ -x "$TADPOLE_DEPS/bin/glasspole" ] || [ -x "$TADPOLE_DEPS/bin/qemu-arm" ]; } &&
    [ -x "$TADPOLE_DEPS/python/bin/python3" ]
}
