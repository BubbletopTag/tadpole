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

tad_qemu() {
    if [ -n "${TADPOLE_QEMU:-}" ] && [ -x "${TADPOLE_QEMU}" ]; then
        printf '%s' "$TADPOLE_QEMU"; return 0
    fi
    if [ -x "$TADPOLE_DEPS/bin/qemu-arm" ]; then
        printf '%s' "$TADPOLE_DEPS/bin/qemu-arm"; return 0
    fi
    command -v qemu-arm 2>/dev/null && return 0
    return 1
}

# A Python that can run ubi_reader, NOT just any Python. The distinction is the
# whole point: the host may well have python3 and not have ubi_reader, and
# picking it because it exists is how firmware installs used to die halfway
# through with ModuleNotFoundError.
tad_python() {
    local cand
    for cand in "${TADPOLE_PYTHON:-}" \
                "$TADPOLE_DEPS/python/bin/python3" \
                "$(command -v python3 2>/dev/null)"; do
        [ -n "$cand" ] && [ -x "$cand" ] || continue
        printf '%s' "$cand"; return 0
    done
    return 1
}

tad_python_with_ubireader() {
    local cand
    for cand in "${TADPOLE_PYTHON:-}" \
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
tad_have_bundle() {
    [ -x "$TADPOLE_DEPS/bin/qemu-arm" ] &&
    [ -x "$TADPOLE_DEPS/python/bin/python3" ]
}
