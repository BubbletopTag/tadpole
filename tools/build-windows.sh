#!/bin/bash
# Tadpole/Glasspole — build the whole Windows product FROM LINUX.
#
#   ./tools/build-windows.sh                 build everything into build/win
#   ./tools/build-windows.sh --installer     ...and package Glasspole-Setup.exe
#   ./tools/build-windows.sh --version 09082026-0003
#
# WHY CROSS-COMPILE RATHER THAN BUILD ON WINDOWS. Releases are cut on the
# Linux box, from the same commit as the AppImage, by the same person, at the
# same moment. A release step that requires walking to another machine is a
# release step that will one day be skipped — and the two halves would drift.
# mingw-w64 builds all three binaries and NSIS packages them, both from apt.
#
#   apt install mingw-w64 nsis
#   apt install libsdl2-dev:i386   # NO — see the SDL note below
#
# SDL2 IS THE ONLY AWKWARD DEPENDENCY. There is no distro mingw SDL2 package
# worth relying on, so this fetches the official mingw development archive
# (the one SDL ships for exactly this purpose) into build/win/sdl2 and uses
# it. It is a download, not a vendored copy: nothing enters git.
#
# WHAT IT DOES NOT BUILD: the ARM shim. That is guest code, identical on every
# host, and it needs the LeapFrog libraries that cannot be redistributed — so
# the installer ships no shim and no firmware, exactly as the AppImage ships
# none. The wizard fetches those on first run.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJ="$(dirname "$HERE")"
OUT="$PROJ/build/win"
SDL_VER="${SDL_VER:-2.30.9}"
SDL_URL="https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL2-devel-$SDL_VER-mingw.tar.gz"
TRIPLE=x86_64-w64-mingw32
VERSION="dev"
WANT_INSTALLER=0

while [ $# -gt 0 ]; do
    case "$1" in
        --installer) WANT_INSTALLER=1 ;;
        --version)   shift; VERSION="${1:-dev}" ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

command -v "$TRIPLE-gcc" >/dev/null || {
    echo "no $TRIPLE-gcc — install mingw-w64" >&2; exit 1; }

mkdir -p "$OUT"

# ---- SDL2 -----------------------------------------------------------------
SDL="$OUT/sdl2/SDL2-$SDL_VER/$TRIPLE"
if [ ! -d "$SDL" ]; then
    echo "==> fetching SDL2 $SDL_VER (mingw development libraries)"
    mkdir -p "$OUT/sdl2"
    curl -fsSL "$SDL_URL" | tar xz -C "$OUT/sdl2"
fi
SDL_CFLAGS="-I$SDL/include -I$SDL/include/SDL2 -Dmain=SDL_main"
SDL_LIBS="-L$SDL/lib -lmingw32 -lSDL2main -lSDL2 -mwindows"

# ---- glasspole ------------------------------------------------------------
# The emulator. Needs dynarmic, which is a CMake project, so it gets a
# toolchain file rather than a hand-written link line.
echo "==> glasspole.exe"
[ -f "$PROJ/glasspole/deps/dynarmic/CMakeLists.txt" ] || "$PROJ/glasspole/fetch-deps.sh"
cat > "$OUT/mingw.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $TRIPLE-gcc)
set(CMAKE_CXX_COMPILER $TRIPLE-g++)
set(CMAKE_RC_COMPILER $TRIPLE-windres)
set(CMAKE_FIND_ROOT_PATH /usr/$TRIPLE)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
EOF
cmake -S "$PROJ/glasspole" -B "$OUT/glasspole" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$OUT/mingw.cmake" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$OUT/glasspole" glasspole

# ---- viewer and launcher --------------------------------------------------
# Compiled directly: the viewer's CMakeLists exists for MSYS2, where
# pkg-config answers; here the SDL paths are known and a toolchain file for
# three source files would be ceremony.
echo "==> tadpole-view.exe"
V="$PROJ/tadpole/viewer"
"$TRIPLE-gcc" -O2 -std=gnu17 -DTADPOLE_VERSION="\"$VERSION\"" \
    -o "$OUT/tadpole-view.exe" \
    "$V/tadpole_view.c" "$V/tadpole_ui.c" "$V/tadpole_hle.c" \
    $SDL_CFLAGS $SDL_LIBS -lopengl32 -lz -lshlwapi -static -mconsole

echo "==> tadpole.exe (launcher)"
"$TRIPLE-gcc" -O2 -o "$OUT/tadpole.exe" "$V/tadpole_launcher.c" \
    -static -municode -mwindows

# ---- staging --------------------------------------------------------------
STAGE="$OUT/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/tadpole/viewer/build" "$STAGE/glasspole/build" "$STAGE/tools" \
         "$STAGE/runtime"
cp "$OUT/tadpole.exe"                    "$STAGE/tadpole.exe"
cp "$OUT/tadpole-view.exe"               "$STAGE/tadpole/viewer/build/"
cp "$OUT/glasspole/glasspole.exe"        "$STAGE/glasspole/build/"
cp "$SDL/bin/SDL2.dll"                   "$STAGE/tadpole/viewer/build/" 2>/dev/null || true
# tadpole.sh is how every part of this finds the project root, so it ships
# even though nothing runs it on Windows.
cp "$PROJ/tadpole.sh" "$PROJ/glasspole.png" "$PROJ/tadpole.png" \
   "$PROJ/README.md" "$STAGE/"
cp -r "$PROJ/runtime/setup-sysroot.sh" "$STAGE/runtime/" 2>/dev/null || true
for t in install-game.py scan-games.py check-update.py fetch-firmware.py \
         pkgtool.py fix-perms.py lf3.py packagelists; do
    cp -r "$PROJ/tools/$t" "$STAGE/tools/" 2>/dev/null || true
done
echo "$VERSION" > "$STAGE/.tadpole-version"

echo "staged: $STAGE"
[ "$WANT_INSTALLER" = 1 ] || { echo "done (no installer asked for)"; exit 0; }

# ---- installer ------------------------------------------------------------
command -v makensis >/dev/null || {
    echo "no makensis — install nsis" >&2; exit 1; }
echo "==> Glasspole-Setup.exe"
makensis -NOCD -DVERSION="$VERSION" -DSTAGE="$STAGE" -DOUTFILE="$OUT/Glasspole-Setup.exe" \
         "$HERE/glasspole.nsi" >/dev/null
echo "built: $OUT/Glasspole-Setup.exe"
