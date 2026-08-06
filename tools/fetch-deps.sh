#!/bin/bash
# Tadpole — fetch the third-party runtime pieces that get bundled into the
# AppImage.
#
#   ./tools/fetch-deps.sh              fetch whatever is missing
#   ./tools/fetch-deps.sh --force      fetch again even if it is already there
#   ./tools/fetch-deps.sh --clean      throw the whole lot away
#
# WHY THIS EXISTS
# ---------------
# Setting Tadpole up used to mean installing, by hand, on a distribution whose
# package names you had to know:
#
#     qemu-user  ubi_reader  lzallright  cryptography  zstandard  unzip  bzip2
#
# Six of those seven exist only to unpack the firmware ONCE, and two of them are
# AUR-only on Arch. Every one is a place a first-time user stops. So they are
# fetched here, once, on the machine that BUILDS the AppImage, and shipped
# inside it — the person who runs Tadpole installs nothing.
#
# WHAT IS FETCHED, AND WHY THAT PARTICULAR THING
# ----------------------------------------------
# qemu-arm — from Ubuntu's qemu-user-static package, because it is the last
#   widely-published build that is genuinely STATIC:
#       ELF 64-bit LSB pie executable, x86-64, static-pie linked
#   Debian 13 and Ubuntu 25.04 turned qemu-user-static into a transitional
#   package whose "binaries" are symlinks to the dynamically-linked ones (their
#   .deb is 70 KB), and a dynamic qemu drags in glib, gnutls, libdw and a dozen
#   more, each of which must then match the user's glibc. Static side-steps all
#   of it: one 4 MB file that runs anywhere.
#
# CPython — a relocatable interpreter from python-build-standalone, plus
#   ubi_reader and its compression backends installed into it.
#
#   Bundling the interpreter rather than the wheels alone is deliberate.
#   ubi_reader imports lzallright, zstandard AND cryptography at module scope,
#   so all three must load or nothing does; of those, zstandard publishes no
#   abi3 wheel, meaning a wheel picked here would only import on a host running
#   the exact same Python minor version. Shipping the interpreter too makes the
#   question moot — and the whole firmware toolchain becomes one directory that
#   either works or is absent.
#
# WHAT IS NOT FETCHED: OpenGL, X11/Wayland and the audio stack. Those belong to
# the user's machine and their driver; bundling them is how AppImages break.
#
# LICENCES. qemu is GPLv2, ubi_reader GPLv3, CPython PSF, lzallright MIT,
# cryptography Apache-2.0/BSD. They are aggregated here, not linked together —
# but if you redistribute the AppImage, you are redistributing them, so keep
# build/deps/manifest.txt with it: it records exactly which build of each.

set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
DEPS="$PROJ/build/deps"
CACHE="$DEPS/cache"

# Pinned, with hashes. A dependency that changes under you is a bug you get to
# debug twice — once in the emulator and once in the download.
QEMU_URL="http://archive.ubuntu.com/ubuntu/pool/universe/q/qemu/qemu-user-static_8.2.2+ds-0ubuntu1.18_amd64.deb"
QEMU_SHA="5bb397f66063efa349f6fd5cb3b68cd96f29edd0994e4ba5115cf0859a716bf0"
QEMU_VER="8.2.2 (Ubuntu 1:8.2.2+ds-0ubuntu1.18)"

PY_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20260805/cpython-3.12.13+20260805-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
PY_SHA="f04a55ae95e8bd352cdff8da11c344fe609ec84795d106fa91b6620366d786fe"
PY_VER="3.12.13 (python-build-standalone 20260805)"

# SDL2, FROM UBUNTU RATHER THAN FROM THIS MACHINE.
#
# Bundling the host's libSDL2-2.0.so.0 is the obvious thing and it is wrong on
# any modern rolling distribution. Arch — and others — no longer ship SDL2 at
# all: that file belongs to sdl2-compat, a shim implementing the SDL2 ABI on
# top of SDL3, which it dlopen()s BY NAME at run time. It appears in no NEEDED
# entry, so ldd reports a complete dependency list and the AppImage looks fine.
# It then works on every machine that has SDL3 installed — which is every
# machine that could have built it — and aborts on everyone else's with:
#
#     Failed loading SDL3.so
#
# Bundling that SDL3 too does not help either: the one here wants glibc 2.43,
# newer than any released distribution, so the image would fail to start
# everywhere instead of just somewhere.
#
# Ubuntu 22.04's SDL2 is the real library, needs glibc 2.34, and its remaining
# dependencies are the ordinary desktop X11/Wayland/audio set that any machine
# running a graphical session already has.
SDL2_URL="http://archive.ubuntu.com/ubuntu/pool/main/libs/libsdl2/libsdl2-2.0-0_2.0.20+dfsg-2ubuntu1.22.04.1_amd64.deb"
SDL2_SHA="ac3cea9ea66df71445541b2cfd5e07f554ba0f83e3ce4a9dacc311c358100c47"
SDL2_VER="2.0.20 (Ubuntu 22.04)"

# Pinned versions, so two builds of the same Tadpole ship the same extractor.
#
# ONLY ubi_reader is pinned by us. Its own metadata pins the three compression
# backends to ranges it has been tested against (cryptography <49, lzallright
# <0.3, zstandard <0.26), and naming a version of one of those here just
# creates a conflict pip cannot solve — which is exactly what happened with
# cryptography 50.
PY_PKGS="ubi_reader==0.8.14"

FORCE=0
case "${1:-}" in
    --clean) echo "removing $DEPS"; rm -rf "$DEPS"; exit 0 ;;
    --force) FORCE=1 ;;
    "")      ;;
    *)       echo "usage: $0 [--force|--clean]" >&2; exit 2 ;;
esac

die() { echo "error: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

for t in curl ar tar sha256sum; do
    have "$t" || die "need '$t' to fetch dependencies"
done

mkdir -p "$CACHE" "$DEPS/bin"

# Download to a .part file and rename only after the hash matches, so an
# interrupted fetch never leaves something that looks complete.
fetch() {                           # $1=url  $2=sha256  $3=dest
    local url="$1" want="$2" dest="$3" got
    if [ -f "$dest" ]; then
        got="$(sha256sum "$dest" | cut -d' ' -f1)"
        [ "$got" = "$want" ] && { echo "  cached  $(basename "$dest")"; return 0; }
        echo "  cached copy of $(basename "$dest") has the wrong hash - refetching"
        rm -f "$dest"
    fi
    echo "  fetch   $(basename "$dest")"
    curl -# -L --retry 3 --retry-delay 2 --fail -o "$dest.part" "$url" \
        || die "download failed: $url"
    got="$(sha256sum "$dest.part" | cut -d' ' -f1)"
    if [ "$got" != "$want" ]; then
        rm -f "$dest.part"
        die "checksum mismatch for $url
  expected $want
  got      $got"
    fi
    mv "$dest.part" "$dest"
}

# ---- 1. qemu-arm ----------------------------------------------------------
if [ "$FORCE" = 1 ] || [ ! -x "$DEPS/bin/qemu-arm" ]; then
    echo "==> qemu-arm $QEMU_VER"
    fetch "$QEMU_URL" "$QEMU_SHA" "$CACHE/qemu-user-static.deb"

    tmp="$(mktemp -d "${TMPDIR:-/tmp}/tadpole-deb.XXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    ( cd "$tmp" && ar x "$CACHE/qemu-user-static.deb" ) || die "ar failed on the .deb"
    # Debian compresses the payload with xz, Ubuntu with zstd. Take whichever
    # is there rather than assuming, and say which tool is missing if tar
    # cannot read it.
    data="$(ls "$tmp"/data.tar.* 2>/dev/null | head -1)"
    [ -n "$data" ] || die "no data.tar.* inside the .deb"
    case "$data" in
        *.zst) have zstd || die "the payload is zstd-compressed - install 'zstd'" ;;
        *.xz)  have xz   || die "the payload is xz-compressed - install 'xz'" ;;
    esac
    tar xf "$data" -C "$tmp" ./usr/bin/qemu-arm-static \
        || die "qemu-arm-static is not in this package"
    cp "$tmp/usr/bin/qemu-arm-static" "$DEPS/bin/qemu-arm"
    chmod +x "$DEPS/bin/qemu-arm"
    rm -rf "$tmp"; trap - EXIT

    # A dynamic binary here would run on THIS machine and fail on someone
    # else's, which is the exact failure this script exists to prevent — and it
    # would do it after the AppImage had shipped. Check now.
    if head -c 4096 "$DEPS/bin/qemu-arm" | grep -qa 'ld-linux\|GLIBC_'; then
        echo "  WARNING: $DEPS/bin/qemu-arm looks dynamically linked."
        echo "           It will need the user's glibc to match this machine's."
    fi
    "$DEPS/bin/qemu-arm" -version | head -1 | sed 's/^/  /'
else
    echo "==> qemu-arm already staged"
fi

# ---- 1b. SDL2 -------------------------------------------------------------
if [ "$FORCE" = 1 ] || [ ! -f "$DEPS/lib/libSDL2-2.0.so.0" ]; then
    echo "==> SDL2 $SDL2_VER"
    fetch "$SDL2_URL" "$SDL2_SHA" "$CACHE/libsdl2.deb"
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/tadpole-sdl.XXXXXX")"
    trap 'rm -rf "$tmp"' EXIT
    ( cd "$tmp" && ar x "$CACHE/libsdl2.deb" ) || die "ar failed on the SDL2 .deb"
    data="$(ls "$tmp"/data.tar.* 2>/dev/null | head -1)"
    [ -n "$data" ] || die "no data.tar.* inside the SDL2 .deb"
    tar xf "$data" -C "$tmp" || die "could not unpack the SDL2 .deb"
    real="$(find "$tmp" -name 'libSDL2-2.0.so.0.*' | head -1)"
    [ -n "$real" ] || die "no libSDL2 in that package"
    mkdir -p "$DEPS/lib"
    cp "$real" "$DEPS/lib/libSDL2-2.0.so.0"
    rm -rf "$tmp"; trap - EXIT
    echo "  glibc floor: $(objdump -T "$DEPS/lib/libSDL2-2.0.so.0" 2>/dev/null |
                           grep -oE 'GLIBC_2\.[0-9]+' | sort -V -u | tail -1)"
else
    echo "==> SDL2 already staged"
fi

# ---- 2. python + the firmware toolchain -----------------------------------
# THE TEST IS "CAN IT EXTRACT FIRMWARE", not "does an interpreter exist".
# Those came apart the first time this ran: pip failed on a dependency
# conflict, leaving a perfectly good python3 with no ubi_reader in it, and the
# next run saw the interpreter, said "already staged" and skipped the install
# it had just failed to do.
py_ready() {
    [ -x "$DEPS/python/bin/python3" ] &&
    "$DEPS/python/bin/python3" -c "import ubireader.ubifs.misc" >/dev/null 2>&1
}
if [ "$FORCE" = 1 ] || ! py_ready; then
    echo "==> CPython $PY_VER"
    fetch "$PY_URL" "$PY_SHA" "$CACHE/cpython.tar.gz"
    rm -rf "$DEPS/python"
    mkdir -p "$DEPS/python"
    # The tarball unpacks a single top-level python/ directory.
    tar xzf "$CACHE/cpython.tar.gz" -C "$DEPS" || die "could not unpack CPython"
    [ -x "$DEPS/python/bin/python3" ] || die "unexpected CPython layout"

    echo "==> installing the firmware toolchain into it"
    echo "    $PY_PKGS"
    # shellcheck disable=SC2086
    "$DEPS/python/bin/python3" -m pip install --quiet --no-input \
        --no-warn-script-location $PY_PKGS \
        || die "pip install failed"
else
    echo "==> CPython already staged"
fi

# ---- 3. prune -------------------------------------------------------------
# Everything here rides in a squashfs the user downloads, so it is worth being
# ruthless: as fetched, this is 126 MB, and 63 MB of that is either a test
# suite, a GUI toolkit, or the same interpreter twice.
if [ -d "$DEPS/python" ]; then
    PYLIB="$(echo "$DEPS"/python/lib/python3.*)"
    for junk in test idlelib tkinter turtledemo lib2to3 ensurepip \
                config-3.12-x86_64-linux-gnu; do
        rm -rf "${PYLIB:?}/$junk"
    done
    # pip did its job at build time and is dead weight at run time.
    rm -rf "$PYLIB/site-packages/pip" "$PYLIB/site-packages/pip-"*
    rm -rf "$DEPS/python/include" "$DEPS/python/share"
    rm -f  "$DEPS/python/lib/"*.a
    # tkinter is gone; its 8 MB of Tcl/Tk has nothing left to serve.
    rm -rf "$DEPS/python/lib/tcl"* "$DEPS/python/lib/tk"* \
           "$DEPS/python/lib/libtcl"* "$DEPS/python/lib/libtk"* \
           "$DEPS/python/lib/itcl"* "$DEPS/python/lib/thread"*
    # THE INTERPRETER IS IN THERE TWICE. bin/python3.12 links libpython
    # STATICALLY — ldd shows only libc and friends — and lib/libpython3.12.so
    # is a second 29 MB copy kept for programs that EMBED Python. Nothing here
    # embeds it; we run a script with an executable.
    rm -f "$DEPS/python/lib/libpython3."*.so.*
    find "$DEPS/python" -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
    find "$DEPS/python/bin" -maxdepth 1 -type f \
         ! -name 'python3*' -delete 2>/dev/null || true
    # Wheels ship unstripped: cryptography's Rust extension alone drops from
    # 15 MB to 11, zstandard's from 23 MB to under 1.
    if have strip; then
        find "$DEPS/python" -type f -name '*.so*' \
             -exec strip --strip-unneeded {} + 2>/dev/null || true
    fi
fi

# ---- 4. prove it works ----------------------------------------------------
# ubi_reader imports its three compression backends at module scope, so this
# one line is the whole test: if it prints, firmware extraction will work on
# any machine the AppImage lands on.
echo "==> checking"
"$DEPS/python/bin/python3" - <<'PY' || die "the bundled Python cannot run ubi_reader"
import sys
import ubireader.ubifs.misc          # pulls in lzallright, zstandard, cryptography
from ubireader.scripts import ubireader_extract_files
print("  ubi_reader ready on Python %d.%d" % sys.version_info[:2])
PY
"$DEPS/bin/qemu-arm" -version | head -1 | sed 's/^/  /'

# ---- 5. manifest ----------------------------------------------------------
{
    echo "Tadpole bundled dependencies"
    echo "generated $(date -u '+%Y-%m-%d %H:%M UTC')"
    echo
    echo "qemu-arm        $QEMU_VER            GPLv2"
    echo "  $QEMU_URL"
    echo "CPython         $PY_VER              PSF"
    echo "  $PY_URL"
    echo
    echo "Python packages:"
    "$DEPS/python/bin/python3" - <<'PY'
import importlib.metadata as m
for p in ("ubi_reader", "lzallright", "zstandard", "cryptography"):
    try:
        print("  %-14s %s" % (p, m.version(p)))
    except Exception:
        print("  %-14s MISSING" % p)
PY
    echo
    echo "ubi_reader is GPLv3, lzallright MIT, cryptography Apache-2.0/BSD-3."
    echo "Keep this file alongside anything you redistribute."
} > "$DEPS/manifest.txt"

echo
echo "Staged in $DEPS  ($(du -sh "$DEPS" | cut -f1), of which $(du -sh "$CACHE" | cut -f1) is the download cache)"
echo "Now run: ./tools/build-appimage.sh"
