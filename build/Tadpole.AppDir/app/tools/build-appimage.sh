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
#      and SDL2 (self-contained and version-sensitive).
#
# Out: OpenGL and X11 — bundling those breaks against the user's graphics
#      driver, and every AppImage relies on the host for them. Also out:
#      runtime/libs and runtime/sysroot, which are GENERATED (runtime/libs is a
#      directory of absolute symlinks into wherever the firmware was extracted,
#      so shipping it would bake in this machine's paths).
#
# Out for a different reason: qemu-arm. It is a large dependency with its own
# library tail, and every distribution packages it. The AppImage checks for it
# and says exactly what to install.
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
OUT="$PROJ/build/Tadpole-x86_64.AppImage"

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

# ---- 3. bundle SDL2 only ---------------------------------------------------
# Version-sensitive and self-contained. GL and X11 stay on the host.
for lib in libSDL2-2.0.so.0; do
    p="$(ldd "$PROJ/tadpole/viewer/tadpole-view" | awk -v l="$lib" '$1==l {print $3}')"
    [ -n "$p" ] && [ -f "$p" ] && cp -L "$p" "$APPDIR/usr/lib/" && echo "  bundled $lib"
done

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

# qemu-arm is not bundled: it is large, it has its own library tail, and every
# distribution packages it. Say precisely what to install rather than failing
# somewhere deeper with a confusing message.
if ! command -v qemu-arm >/dev/null 2>&1; then
    MSG="Tadpole needs qemu-arm (package: qemu-user).

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
