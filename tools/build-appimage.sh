#!/bin/bash
# Tadpole — build an AppImage.
#
#   ./tools/build-appimage.sh
#
# Produces Tadpole-x86_64.AppImage: one file, no install step, no build tools
# needed by whoever runs it.
#
# WHAT GOES IN, AND WHAT DELIBERATELY DOES NOT
# --------------------------------------------
# In:  the viewer, the cross-compiled ARM shim libraries, tadpole.sh, tools/,
#      SDL2 and zlib (self-contained and version-sensitive), and — from
#      build/deps, staged by tools/fetch-deps.sh — a static qemu-arm and a
#      private Python carrying ubi_reader.
#
#      qemu-arm USED to be left out, on the grounds that it is large and every
#      distribution packages it. That was true and it was still the wrong call:
#      it made "install qemu-user, and work out what your distribution calls
#      it" the first step of using a one-file emulator. The static build is
#      4 MB and depends on nothing.
#
# Out: OpenGL and X11 — bundling those breaks against the user's graphics
#      driver, and every AppImage relies on the host for them. Also out:
#      runtime/libs and runtime/sysroot, which are GENERATED (runtime/libs is a
#      directory of absolute symlinks into wherever the firmware was extracted,
#      so shipping it would bake in this machine's paths).
#
# A LIMIT WORTH KNOWING. The libraries copied off THIS machine (SDL2, zlib)
# carry this machine's glibc floor with them, so an AppImage built on a
# bleeding-edge distribution may refuse to start on an older one. The two
# bundles from fetch-deps.sh are exempt: qemu-arm is static, and the Python is
# a manylinux build good back to glibc 2.17.
#
# WHY THERE IS A DATA DIRECTORY
# -----------------------------
# An AppImage is a read-only squashfs, but Tadpole must write: the extracted
# firmware, the sysroot, saved settings, installed games. So the read-only parts
# are seeded into $XDG_DATA_HOME/tadpole on first run and Tadpole runs from
# there, with TADPOLE_PROJECT pointing at it.

set -eu
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
APPDIR="$PROJ/build/Tadpole.AppDir"
# Overridable, because you cannot write an AppImage that is currently RUNNING:
# mksquashfs stops with "Could not open regular file for writing as
# destination: Text file busy", which reads like a corrupt build rather than
# "close the emulator first".
#
#   TADPOLE_APPIMAGE_OUT=build/Tadpole-next.AppImage ./tools/build-appimage.sh
OUT="${TADPOLE_APPIMAGE_OUT:-$PROJ/build/Tadpole-x86_64.AppImage}"

die() { echo "error: $*" >&2; exit 1; }

# ---- 1. everything must be built first ----------------------------------
[ -x "$PROJ/tadpole/viewer/tadpole-view" ] || die "not built — run: cd tadpole && make"
for d in shimlibs shimlibs-gl shimlibs-z; do
    [ -d "$PROJ/runtime/$d" ] || die "missing runtime/$d — run: cd tadpole && make"
done

echo "==> assembling $APPDIR"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/lib" "$APPDIR/app"

# ---- 2. the application ---------------------------------------------------
cp "$PROJ/tadpole/viewer/tadpole-view" "$APPDIR/usr/bin/"
cp "$PROJ/tadpole.sh"                  "$APPDIR/app/"
cp "$PROJ/tadpole.png"                 "$APPDIR/app/"
cp -r "$PROJ/tools"                    "$APPDIR/app/"
# Byte-compiled leftovers from running the Python tools in the source tree.
# They are the WRONG Python's cache once this ships, and they feed the content
# hash below, so an unchanged Tadpole would look like a new build.
find "$APPDIR/app/tools" -name '__pycache__' -prune -exec rm -rf {} + 2>/dev/null || true
mkdir -p "$APPDIR/app/runtime" "$APPDIR/app/tadpole/viewer"
for d in shimlibs shimlibs-gl shimlibs-z; do
    cp -r "$PROJ/runtime/$d" "$APPDIR/app/runtime/"
done
cp "$PROJ/runtime/setup-sysroot.sh" "$APPDIR/app/runtime/"
# tadpole.sh looks for the viewer at tadpole/viewer/tadpole-view relative to
# itself, so give it one there rather than teaching it a second layout.
cp "$PROJ/tadpole/viewer/tadpole-view" "$APPDIR/app/tadpole/viewer/"
[ -f "$PROJ/README.md" ] && cp "$PROJ/README.md" "$APPDIR/app/"
[ -f "$PROJ/LICENSE" ]   && cp "$PROJ/LICENSE"   "$APPDIR/app/"

# ---- 3. bundle SDL2 and zlib ----------------------------------------------
# Version-sensitive and self-contained. GL and X11 stay on the host.
for lib in libSDL2-2.0.so.0 libz.so.1; do
    p="$(ldd "$PROJ/tadpole/viewer/tadpole-view" | awk -v l="$lib" '$1==l {print $3}')"
    [ -n "$p" ] && [ -f "$p" ] && cp -L "$p" "$APPDIR/usr/lib/" && echo "  bundled $lib"
done

# ---- 3b. the staged runtime dependencies ----------------------------------
# These are NOT copied into the user's data directory later: they are read-only
# and large, so AppRun points at them where they sit in the mounted image.
if [ -d "$PROJ/build/deps/bin" ] || [ -d "$PROJ/build/deps/python" ]; then
    echo "==> bundling the staged dependencies"
    mkdir -p "$APPDIR/deps"
    [ -x "$PROJ/build/deps/bin/qemu-arm" ] && {
        mkdir -p "$APPDIR/deps/bin"
        cp -a "$PROJ/build/deps/bin/qemu-arm" "$APPDIR/deps/bin/"
        echo "  qemu-arm       $("$PROJ/build/deps/bin/qemu-arm" -version | head -1 | awk '{print $3}')"
    }
    [ -d "$PROJ/build/deps/python" ] && {
        cp -a "$PROJ/build/deps/python" "$APPDIR/deps/"
        echo "  python + ubi_reader ($(du -sh "$PROJ/build/deps/python" | cut -f1))"
    }
    [ -f "$PROJ/build/deps/manifest.txt" ] && \
        cp "$PROJ/build/deps/manifest.txt" "$APPDIR/deps/"
else
    cat <<'MSG'

  NOTE: build/deps is empty, so this AppImage will NOT be self-contained —
        whoever runs it must install qemu-user and ubi_reader themselves.
        Run ./tools/fetch-deps.sh first to bundle them.

MSG
fi

# ---- 4. desktop integration ----------------------------------------------
cat > "$APPDIR/tadpole.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Tadpole
GenericName=LeapPad2 Emulator
Comment=Run LeapPad2 software on your computer
Exec=tadpole
Icon=tadpole
Categories=Game;Emulator;
Terminal=false
DESKTOP
cp "$PROJ/tadpole.png" "$APPDIR/tadpole.png"
mkdir -p "$APPDIR/usr/share/icons/hicolor/256x256/apps"
cp "$PROJ/tadpole.png" "$APPDIR/usr/share/icons/hicolor/256x256/apps/tadpole.png"

# ---- 5. AppRun -------------------------------------------------------------
cat > "$APPDIR/AppRun" <<'APPRUN'
#!/bin/bash
# Tadpole AppImage entry point.
#
# An AppImage is read-only, and Tadpole writes a great deal: extracted firmware,
# the generated sysroot, installed games, saved settings. So the read-only parts
# are copied into a data directory on first run and Tadpole runs from there.
set -u
HERE="$(dirname "$(readlink -f "$0")")"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/tadpole"

export LD_LIBRARY_PATH="$HERE/usr/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$HERE/usr/bin:$PATH"

# THE BUNDLE STAYS IN THE IMAGE. It is read-only and about 70 MB; copying it
# into the user's data directory on every upgrade would waste both the disk and
# the wait. tools/lib-deps.sh reads TADPOLE_DEPS, so pointing at the mount is
# all it takes.
if [ -d "$HERE/deps" ]; then
    export TADPOLE_DEPS="$HERE/deps"
fi

# Neither bundled nor installed is the only remaining way to have no qemu, and
# it should say so in a window as well as a terminal — someone launching from a
# desktop icon never sees stderr.
if [ ! -x "${TADPOLE_DEPS:-}/bin/qemu-arm" ] && ! command -v qemu-arm >/dev/null 2>&1; then
    MSG="This build of Tadpole does not carry qemu-arm, and this machine has none.

Install your distribution's qemu-user package:

  Arch:    sudo pacman -S qemu-user
  Debian:  sudo apt install qemu-user
  Fedora:  sudo dnf install qemu-user"
    echo "$MSG" >&2
    command -v zenity >/dev/null && zenity --error --no-wrap --text="$MSG" 2>/dev/null
    exit 1
fi

# Seed or refresh the data directory. Compare a stamp so an upgraded AppImage
# replaces the program parts WITHOUT touching firmware, games or settings.
STAMP="$DATA/.appimage-stamp"
WANT="$(cat "$HERE/app/.build-id" 2>/dev/null || echo unknown)"
if [ ! -d "$DATA" ] || [ "$(cat "$STAMP" 2>/dev/null || true)" != "$WANT" ]; then
    echo "Tadpole: setting up $DATA"
    mkdir -p "$DATA"
    # Program parts only. rootfs/, runtime/sysroot/, runtime/libs/ and games/
    # belong to the user and are never overwritten.
    cp -a "$HERE/app/tadpole.sh"   "$DATA/"
    cp -a "$HERE/app/tadpole.png"  "$DATA/"
    cp -a "$HERE/app/tools"        "$DATA/"
    cp -a "$HERE/app/tadpole"      "$DATA/"
    mkdir -p "$DATA/runtime"
    for d in shimlibs shimlibs-gl shimlibs-z; do
        rm -rf "$DATA/runtime/$d"
        cp -a "$HERE/app/runtime/$d" "$DATA/runtime/"
    done
    cp -a "$HERE/app/runtime/setup-sysroot.sh" "$DATA/runtime/"
    # SOMEWHERE TO PUT A KEY. .lf3 packages — LFConnect's digital purchases —
    # are the one encrypted thing in the set, and Tadpole ships no key for
    # them. The folder is made empty so the installer's message ("put key in
    # keys/lf3.keys") names a place that already exists.
    mkdir -p "$DATA/keys"
    [ -f "$DATA/keys/README" ] || cat > "$DATA/keys/README" <<'KEYS'
Put lf3.keys here — one line, 32 hex characters — to install .lf3 packages
(LFConnect digital purchases) during a firmware install.

Everything else works without it. Firmware, cartridge backups and the emulator
itself do not use this at all; without a key those files are simply skipped.
KEYS
    [ -f "$HERE/app/README.md" ] && cp -a "$HERE/app/README.md" "$DATA/"
    chmod +x "$DATA/tadpole.sh" "$DATA/tools/"*.sh 2>/dev/null || true
    echo "$WANT" > "$STAMP"
fi

export TADPOLE_PROJECT="$DATA"
cd "$DATA"
exec ./tadpole.sh "$@"
APPRUN
chmod +x "$APPDIR/AppRun"
# A CONTENT HASH, not a timestamp. The stamp decides whether AppRun refreshes an
# existing data directory, and a timestamp only changes when this script runs —
# so editing a tool and forgetting to rebuild leaves users on the old copy. That
# is exactly what happened with setup-sysroot.sh: the fix was in the tree, the
# AppImage still shipped the version with the hardcoded rootfs path, and "Build
# sysroot" kept failing with "no rootfs" on a machine that plainly had one.
find "$APPDIR/app" -type f ! -name .build-id -print0 \
    | sort -z | xargs -0 cat 2>/dev/null | md5sum | cut -c1-16 \
    > "$APPDIR/app/.build-id"

# ---- 5b. what glibc does this image demand? -------------------------------
#
# THE ONE THING THAT MAKES AN APPIMAGE NOT RUN ELSEWHERE, and it is invisible
# on the machine that built it. Every binary copied off this host carries the
# glibc it was linked against, and the newest requirement across the whole set
# is the oldest distribution the image can run on.
#
# It bit us: built on Arch with a compiler defaulting to C23, four symbols
# (__isoc23_strtol and friends) pushed the floor to glibc 2.38, so the image
# refused to start on Linux Mint 21 — and the failure named a library rather
# than a version, which reads as "SDL is missing" to anyone who has not seen
# it before. Print it rather than find out from a user.
if command -v objdump >/dev/null; then
    FLOOR=$(for f in "$APPDIR/usr/bin/tadpole-view" "$APPDIR/usr/lib/"*.so* \
                     "$APPDIR/deps/python/bin/python3."*; do
                [ -f "$f" ] || continue
                objdump -T "$f" 2>/dev/null | grep -oE 'GLIBC_2\.[0-9]+'
            done | sort -V -u | tail -1)
    case "$FLOOR" in
        GLIBC_2.3[0-4]|GLIBC_2.2*|GLIBC_2.1*) NOTE="Ubuntu 22.04 / Mint 21 and newer" ;;
        GLIBC_2.3[5-7])                       NOTE="Ubuntu 22.10+ — older Mint will NOT run it" ;;
        *)                                    NOTE="NEWER THAN Ubuntu 23.04 — most users cannot run this" ;;
    esac
    echo "==> needs ${FLOOR:-unknown}: $NOTE"
fi

echo "==> AppDir ready: $APPDIR"
echo "    it is runnable as-is:  $APPDIR/AppRun"

# ---- 6. pack it ------------------------------------------------------------
TOOL=""
for t in appimagetool appimagetool-x86_64.AppImage; do
    command -v "$t" >/dev/null && { TOOL="$t"; break; }
done
[ -z "$TOOL" ] && [ -x "$PROJ/build/appimagetool-x86_64.AppImage" ] && \
    TOOL="$PROJ/build/appimagetool-x86_64.AppImage"

if [ -z "$TOOL" ]; then
    cat <<'MSG'

The AppDir is complete, but appimagetool is not installed, so it has not been
packed into a single file yet.

    Arch:    yay -S appimagetool-bin
    Any:     download appimagetool-x86_64.AppImage from
             github.com/AppImage/AppImageKit/releases
             chmod +x it and put it in build/ or on your PATH

appimagetool also needs squashfs-tools (mksquashfs).

Then run this script again.
MSG
    exit 0
fi

echo "==> packing with $TOOL"
ARCH=x86_64 "$TOOL" "$APPDIR" "$OUT" || die "appimagetool failed"
echo
echo "Built: $OUT"
ls -lh "$OUT"
