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

# NATIVE OR CROSS, decided by where it is running. Under MSYS2 the compiler
# IS the target compiler and the two CMake trees are already there, so the
# script builds in place instead of cross-compiling — which is what makes it
# possible to test the installer on the machine that will run it, without
# waiting for a Linux release to be cut.
NATIVE=0
case "$(uname -s)" in MSYS*|MINGW*) NATIVE=1 ;; esac

if [ "$NATIVE" = 0 ]; then
    command -v "$TRIPLE-gcc" >/dev/null || {
        echo "no $TRIPLE-gcc — install mingw-w64" >&2; exit 1; }
fi

mkdir -p "$OUT"

if [ "$NATIVE" = 1 ]; then
    echo "==> native MSYS2 build"
    cmake -S "$PROJ/glasspole" -B "$PROJ/glasspole/build" -G Ninja >/dev/null
    ninja -C "$PROJ/glasspole/build" glasspole
    cmake -S "$PROJ/tadpole/viewer" -B "$PROJ/tadpole/viewer/build" -G Ninja \
          -DTADPOLE_VERSION="$VERSION" >/dev/null
    ninja -C "$PROJ/tadpole/viewer/build"
    mkdir -p "$OUT/glasspole"
    cp "$PROJ/glasspole/build/glasspole.exe"        "$OUT/glasspole/"
    cp "$PROJ/tadpole/viewer/build/tadpole-view.exe" "$OUT/"
    cp "$PROJ/tadpole/viewer/build/tadpole.exe"      "$OUT/"
    SDL=""            # everything is linked statically by the CMake build
fi

if [ "$NATIVE" = 0 ]; then

# ---- SDL2 -----------------------------------------------------------------
SDL="$OUT/sdl2/SDL2-$SDL_VER/$TRIPLE"
if [ ! -d "$SDL" ]; then
    echo "==> fetching SDL2 $SDL_VER (mingw development libraries)"
    mkdir -p "$OUT/sdl2"
    curl -fsSL "$SDL_URL" | tar xz -C "$OUT/sdl2"
fi
SDL_CFLAGS="-I$SDL/include -I$SDL/include/SDL2 -Dmain=SDL_main"
# ASK SDL, rather than writing the list out. A static SDL2 needs a dozen
# Windows import libraries — ole32, oleaut32, setupapi, dinput8 and the rest —
# and the failure when one is missing is a page of undefined COM symbols like
# CoCreateInstance, which names SDL nowhere. sdl2-config ships in the archive
# and knows the exact set for the version being used.
if [ -x "$SDL/bin/sdl2-config" ]; then
    SDL_LIBS="$("$SDL/bin/sdl2-config" --prefix="$SDL" --static-libs)"
else
    SDL_LIBS="-L$SDL/lib -lmingw32 -mwindows -lSDL2main -lSDL2 -lm -lkernel32 \
-luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 -lversion -luuid \
-ladvapi32 -lsetupapi -lshell32 -ldinput8"
fi

# ---- zlib -----------------------------------------------------------------
# The viewer decodes PNG with nothing but zlib, and there is no distribution
# mingw zlib worth relying on either — so it is fetched and cross-built here,
# for the same reason and in the same way as SDL2 above. It is about two
# hundred kilobytes of C and takes a couple of seconds.
ZLIB_VER="${ZLIB_VER:-1.3.1}"
ZLIB="$OUT/zlib/zlib-$ZLIB_VER"
if [ ! -f "$ZLIB/libz.a" ]; then
    echo "==> fetching and building zlib $ZLIB_VER for $TRIPLE"
    mkdir -p "$OUT/zlib"
    curl -fsSL "https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz" \
        | tar xz -C "$OUT/zlib"
    # win32/Makefile.gcc is zlib's own cross-compilation path; PREFIX is the
    # tool prefix, not an install location.
    make -C "$ZLIB" -f win32/Makefile.gcc PREFIX="$TRIPLE-" libz.a >/dev/null
fi
Z_CFLAGS="-I$ZLIB"
Z_LIBS="-L$ZLIB -lz"

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
# BOOST, AND WHY IT NEEDS A DIRECTORY OF ITS OWN.
#
# dynarmic does find_package(Boost 1.57 REQUIRED). There is no mingw Boost in
# most distributions, and the host's headers would do — Boost's use here is
# variant and type_index, both header-only and both perfectly portable, and
# Boost picks its platform config from the COMPILER's macros, so mingw gets the
# Windows one.
#
# The problem is not the headers, it is the directory they live in. Found at
# /usr/include, CMake adds /usr/include to the include path, and from that
# moment mingw's own <wchar.h> is shadowed by glibc's. The failure surfaces
# inside mingw's <cwchar> as "fwide has not been declared", which points
# nowhere near Boost.
#
# So the same headers are offered from a directory that contains NOTHING else.
BOOSTDIR="$OUT/boostinc"
if [ ! -e "$BOOSTDIR/boost" ]; then
    if [ -d /usr/include/boost ]; then
        mkdir -p "$BOOSTDIR"
        ln -sfn /usr/include/boost "$BOOSTDIR/boost"
    else
        echo "no Boost headers found — install boost (headers are enough)" >&2
        exit 1
    fi
fi

cmake -S "$PROJ/glasspole" -B "$OUT/glasspole" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="$OUT/mingw.cmake" \
      -DBoost_INCLUDE_DIR="$BOOSTDIR" \
      -DBoost_NO_SYSTEM_PATHS=ON \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$OUT/glasspole" glasspole

# ---- viewer and launcher --------------------------------------------------
# Compiled directly: the viewer's CMakeLists exists for MSYS2, where
# pkg-config answers; here the SDL paths are known and a toolchain file for
# three source files would be ceremony.
V="$PROJ/tadpole/viewer"

# ---- the icon --------------------------------------------------------------
# Both of these get the Glasspole logo, and tadpole.exe is the one that
# matters: the installer points the desktop and Start-menu shortcuts at
# "$INSTDIR\tadpole.exe" with icon index 0, so embedding it here is what makes
# the shortcut show the logo — no separate .ico to install and nothing to go
# stale. A shortcut the user makes by hand gets the same artwork.
#
# glasspole.ico is committed; regenerate it from glasspole.png with
# tools/make-icon.py when the logo changes. Building it here would put an
# image library between a Windows build and a working one.
echo "==> icon"
"$TRIPLE-windres" "$V/tadpole_win.rc" -O coff -o "$OUT/tadpole_icon.o" || {
    echo "windres failed — the executables will have no icon" >&2
    rm -f "$OUT/tadpole_icon.o"; }
ICON_OBJ=""
[ -f "$OUT/tadpole_icon.o" ] && ICON_OBJ="$OUT/tadpole_icon.o"

echo "==> tadpole-view.exe"
"$TRIPLE-gcc" -O2 -std=gnu17 -DTADPOLE_VERSION="\"$VERSION\"" \
    -o "$OUT/tadpole-view.exe" \
    "$V/tadpole_view.c" "$V/tadpole_ui.c" "$V/tadpole_hle.c" $ICON_OBJ \
    $SDL_CFLAGS $Z_CFLAGS $SDL_LIBS -lopengl32 $Z_LIBS -lshlwapi -static -mconsole

echo "==> tadpole.exe (launcher)"
"$TRIPLE-gcc" -O2 -o "$OUT/tadpole.exe" "$V/tadpole_launcher.c" $ICON_OBJ \
    -static -municode -mwindows

fi   # end of the cross-compiled branch

# ---- staging --------------------------------------------------------------
STAGE="$OUT/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/tadpole/viewer/build" "$STAGE/glasspole/build" "$STAGE/tools" \
         "$STAGE/runtime"
cp "$OUT/tadpole.exe"                    "$STAGE/tadpole.exe"
cp "$OUT/tadpole-view.exe"               "$STAGE/tadpole/viewer/build/"
cp "$OUT/glasspole/glasspole.exe"        "$STAGE/glasspole/build/"
[ -n "$SDL" ] && cp "$SDL/bin/SDL2.dll" "$STAGE/tadpole/viewer/build/" 2>/dev/null || true
# tadpole.sh is how every part of this finds the project root, so it ships
# even though nothing runs it on Windows.
cp "$PROJ/tadpole.sh" "$PROJ/glasspole.png" "$PROJ/tadpole.png" \
   "$PROJ/README.md" "$STAGE/"
cp -r "$PROJ/runtime/setup-sysroot.sh" "$STAGE/runtime/" 2>/dev/null || true

# THE ARM SHIM SHIPS. It is guest code, identical on every host, it is in git
# already, and without it a title loads no framebuffer, no input and no audio
# — the difference between an emulator and a window. What does NOT ship is
# runtime/libs, which is LeapFrog's own libraries: those arrive with the
# firmware the wizard fetches.
for d in shimlibs shimlibs-z shimlibs-gl; do
    [ -d "$PROJ/runtime/$d" ] && cp -r "$PROJ/runtime/$d" "$STAGE/runtime/"
done

# cart2tar.py and fatread.py go together — the converter is the front end and
# the FAT reader is all of the work. Both are pure stdlib on purpose, so the
# bundled 3.7 runs them with nothing added.
for t in install-game.py scan-games.py check-update.py fetch-firmware.py \
         install-firmware.py online-update.py make-profile.py \
         erase-firmware.py cart2tar.py fatread.py \
         pkgtool.py fix-perms.py lf3.py scan-games.sh packagelists; do
    cp -r "$PROJ/tools/$t" "$STAGE/tools/" 2>/dev/null || true
done

# ---- the Python the firmware tools need -----------------------------------
# IT SHIPS, and that is a deliberate reversal. Telling a Windows user to go
# install Python and then pip two packages is the same wall as "find a device
# and run LFConnect": the setup wizard could already DOWNLOAD the system files
# and then had nothing to install them with, which is the worst possible place
# to stop. So the interpreter comes in the box.
#
# 3.7.9 SPECIFICALLY, AND 3.8 IS NOT A SUBSTITUTE. This was 3.8.10 and it did
# not work on Windows 7 — the interpreter started and then could not import a
# single extension module:
#
#     ImportError: DLL load failed while importing _socket:
#                  The parameter is incorrect.
#
# Python 3.8 loads every .pyd with LoadLibraryExW and the flags
# LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR. Those
# flags do not exist on Windows 7 without KB2533623, and LoadLibraryExW
# rejects them with ERROR_INVALID_PARAMETER — which the import machinery
# reports, unhelpfully, as the message above. _socket and _bz2 were only the
# first two it happened to need. python38.dll references AddDllDirectory;
# python37.dll does not reference it at all and loads .pyd files the old way,
# which an unpatched Windows 7 supports.
#
# Requiring a Microsoft update would have worked and was the wrong answer:
# "install this KB first" is the same wall as "install Python first", on a
# machine whose Windows Update no longer reliably runs.
#
# WHAT THE VERSION DROP COSTS, and how each part is paid for:
#
#   ubi_reader 0.8.9 declares Requires-Python >=3.8. Its metadata does; its
#   code does not — all 29 files parse against the 3.7 grammar, and the wheel
#   is py3-none-any. Wheels are installed here by unzipping, which never
#   consults that field, so the same ubi_reader Linux uses runs on 3.7.
#   Dropping to 0.8.2 instead would have changed the API pkgtool calls.
#
#   lzallright has no cp37 wheel — only cp38-abi3. python-lzo does, and it is
#   the same algorithm, so tools/win-lzallright.py goes in beside it as
#   lzallright.py and translates. See that file for why the byte format is
#   known rather than guessed.
#
# It lands in build/deps/python because tadpole-view already probes exactly
# that path ahead of PATH, so no viewer code had to learn about this.
PY_VER="${PY_VER:-3.7.9}"
PYDIR="$OUT/winpython"
if [ ! -f "$PYDIR/python.exe" ]; then
    echo "==> fetching Python $PY_VER for Windows, with ubi_reader"
    rm -rf "$PYDIR"; mkdir -p "$PYDIR/Lib/site-packages"
    curl -fsSL -o "$OUT/python-embed.zip" \
        "https://www.python.org/ftp/python/$PY_VER/python-$PY_VER-embed-amd64.zip"
    unzip -qo "$OUT/python-embed.zip" -d "$PYDIR"
    rm -f "$OUT/python-embed.zip"

    # The embeddable build ships a ._pth that pins sys.path to the stdlib zip
    # and the executable's own directory — deliberately, so it cannot pick up
    # a system install. That also excludes site-packages, so the wheels below
    # would be invisible without this line. Written rather than appended: the
    # file is regenerated whole so a re-run cannot double the entry.
    printf 'python%s.zip\n.\nLib\\site-packages\n' \
        "$(echo "$PY_VER" | cut -d. -f1,2 | tr -d .)" \
        > "$PYDIR/python$(echo "$PY_VER" | cut -d. -f1,2 | tr -d .)._pth"

    # A WHEEL IS A ZIP, so this needs no pip on the host and no Windows to
    # run on — which matters, because the host here is Linux and pip cannot
    # install a win_amd64 wheel into a Linux tree by any normal route.
    # --platform/--python-version only downloads; unzip does the rest.
    WH="$OUT/wheels"; rm -rf "$WH"; mkdir -p "$WH"
    PIPPY=""
    for p in python3 python; do command -v $p >/dev/null && { PIPPY=$p; break; }; done
    if [ -n "$PIPPY" ]; then
        # Arch and friends ship no pip in the system Python; a throwaway venv
        # has one via ensurepip and costs a second.
        $PIPPY -m venv "$OUT/pipenv" >/dev/null 2>&1 || true
        [ -x "$OUT/pipenv/bin/python" ] && PIPPY="$OUT/pipenv/bin/python"
        # TWO DOWNLOADS, TWO PYTHON VERSIONS, on purpose. ubi_reader is asked
        # for as 3.8 because that is what its metadata demands and the wheel
        # it hands back is py3-none-any — the same file either way. --no-deps
        # so pip does not chase lzallright, which has no cp37 wheel and is
        # replaced below. python-lzo is asked for as 3.7 because it is a real
        # extension and the ABI has to match the interpreter.
        $PIPPY -m pip download -q --only-binary=:all: --no-deps \
            --platform win_amd64 --python-version 38 -d "$WH" \
            "ubi_reader==0.8.9" || {
                echo "could not download ubi_reader — the firmware installer" >&2
                echo "will not work on Windows without it." >&2; }
        $PIPPY -m pip download -q --only-binary=:all: --no-deps \
            --platform win_amd64 --python-version 37 -d "$WH" \
            "python-lzo" || {
                echo "could not download python-lzo — UBIFS volumes will not" >&2
                echo "decompress on Windows without it." >&2; }
    fi
    for w in "$WH"/*.whl; do
        [ -f "$w" ] || continue
        unzip -qo "$w" -d "$PYDIR/Lib/site-packages"
    done
    rm -rf "$WH"
    # The bridge from what ubi_reader imports to what is actually installed.
    cp "$HERE/win-lzallright.py" "$PYDIR/Lib/site-packages/lzallright.py"
fi
if [ -f "$PYDIR/python.exe" ]; then
    mkdir -p "$STAGE/build/deps"
    cp -r "$PYDIR" "$STAGE/build/deps/python"
    # __pycache__ from a host run would be x86-64 Linux bytecode under a
    # Windows tree: dead weight that Python ignores, so drop it.
    find "$STAGE/build/deps/python" -name __pycache__ -type d \
         -exec rm -rf {} + 2>/dev/null || true
    echo "    bundled Python $PY_VER ($(du -sh "$STAGE/build/deps/python" | cut -f1))"
    # EVERY PIECE, CHECKED SEPARATELY. Any one of these missing produces the
    # same symptom on the user's machine — the firmware installer stops with
    # an ImportError — and the build is the only place that can tell them
    # apart, because here we know what was supposed to be there.
    SP_="$STAGE/build/deps/python/Lib/site-packages"
    [ -d "$SP_/ubireader" ] || {
        echo "WARNING: ubi_reader missing — firmware install will fail." >&2; }
    ls "$SP_"/lzo*.pyd >/dev/null 2>&1 || {
        echo "WARNING: python-lzo missing — UBIFS will not decompress." >&2; }
    [ -f "$SP_/lzallright.py" ] || {
        echo "WARNING: the lzallright shim is missing — ubi_reader will not" >&2
        echo "         import." >&2; }
    # The whole reason for 3.7: a .pyd here must load on an unpatched Win7.
    case "$PY_VER" in
      3.7.*) ;;
      *) echo "WARNING: Python $PY_VER loads extension modules with flags that" >&2
         echo "         Windows 7 rejects without KB2533623." >&2 ;;
    esac
fi

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
