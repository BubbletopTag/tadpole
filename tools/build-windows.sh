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
ALLOW_DEV=0

while [ $# -gt 0 ]; do
    case "$1" in
        --installer) WANT_INSTALLER=1 ;;
        # `--version` WITH NOTHING AFTER IT IS A MISTAKE, NOT A REQUEST FOR
        # "dev". It used to silently mean the latter, which is the worst
        # possible reading: a release script whose variable came out empty
        # would build a complete, correctly-named Glasspole-Setup.exe that
        # reported itself as an unreleased build, and nothing anywhere would
        # say so.
        --version)   shift
                     [ -n "${1:-}" ] || {
                         echo "--version needs an argument, e.g. 09082026-0003" >&2
                         exit 2; }
                     VERSION="$1" ;;
        # For deliberately building the installer out of a working copy — see
        # the refusal further down.
        --allow-dev) ALLOW_DEV=1 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

# AN UNVERSIONED INSTALLER IS INDISTINGUISHABLE FROM A RELEASE ONE. The staged
# binaries are the same, the filename is the same, and Glasspole-Setup.exe is
# the exact name tools/check-update.py downloads and the viewer then EXECUTES.
# Someone who builds one by hand and uploads it has shipped a build that tells
# every user it is unreleased, and the update checker can never mark it current.
# Refuse by default; --allow-dev for when that really is what you want.
if [ "$WANT_INSTALLER" = 1 ] && [ "$VERSION" = dev ] && [ "$ALLOW_DEV" = 0 ]; then
    echo "refusing to build Glasspole-Setup.exe with no version." >&2
    echo "  Pass --version DDMMYYYY-NNNN (tools/release.sh does), or" >&2
    echo "  --allow-dev if you really want an installer that reports 'dev'." >&2
    exit 2
fi

# WHAT THE VIEWER MUST CARRY WHEN IT IS BUILT. Used by both branches below.
# The check is `strings | grep -x`, the same one tools/build-appimage.sh and
# the release script use, because a version that is passed and a version that
# is IN THE BINARY are different claims — and the second is the only one that
# reaches a user.
#
# NO `grep -q` HERE, AND THAT IS NOT A STYLE CHOICE. This script runs under
# `set -o pipefail`. `grep -q` exits the instant it matches, `strings` is still
# pouring several megabytes into the pipe, so it dies of SIGPIPE with status
# 141 — and pipefail hands the PIPELINE that 141. The check then fails hardest
# in exactly the case where it should pass, which would have made every release
# refuse itself. Letting grep read to the end costs a few milliseconds on a
# 3 MB binary and removes the trap entirely.
check_stamp() {
    [ "$VERSION" = dev ] && return 0
    strings "$1" 2>/dev/null | grep -Fx -- "$VERSION" >/dev/null || {
        echo "error: $(basename "$1") does not carry $VERSION after building." >&2
        echo "  Refusing to ship a binary that would misreport its version." >&2
        exit 1; }
    echo "    $(basename "$1") reports $VERSION"
}

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
    # -DTADPOLE_VERSION WAS ACCEPTED AND IGNORED HERE UNTIL THE VIEWER'S
    # CMakeLists LEARNED TO READ IT. CMake does not fail on an unused -D; it
    # printed "Manually-specified variables were not used by this project"
    # among the configure output and built a viewer that fell back to "dev".
    # Every native MSYS2 build was unversioned. Now it is consumed — and
    # checked below, because being told is not the same as carrying it.
    cmake -S "$PROJ/tadpole/viewer" -B "$PROJ/tadpole/viewer/build" -G Ninja \
          -DTADPOLE_VERSION="$VERSION" >/dev/null
    ninja -C "$PROJ/tadpole/viewer/build"
    check_stamp "$PROJ/tadpole/viewer/build/tadpole-view.exe"
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

# ---- ogg, vorbis, theora --------------------------------------------------
# The startup animation the boot sequence plays is Ogg Theora with Vorbis
# audio. Fetched and cross-built here for the third time and the third reason
# in a row: there is no mingw package for any of them worth relying on either.
#
# THEY GO INTO THE EXE, NOT BESIDE IT. Static archives only (--disable-shared),
# so this adds nothing to the installer's file list — the Windows viewer is one
# copyable binary and stays one. About 700 KB of object code between them.
#
# LINK ORDER MATTERS AND IS NOT ALPHABETICAL: theoradec calls into ogg, vorbis
# calls into ogg, so ogg comes last. A static link resolves left to right and
# would otherwise leave oggpack_read undefined with libogg.a sitting right
# there on the command line.
#
# OPTIONAL, LIKE EVERYWHERE ELSE. If a build cannot get the sources — offline,
# most likely — the viewer is compiled without TADPOLE_THEORA and shows the
# boot logo alone, which is a smaller loss than a release that will not build.
OGG_VER="${OGG_VER:-1.3.5}"
VORBIS_VER="${VORBIS_VER:-1.3.7}"
THEORA_VER="${THEORA_VER:-1.1.1}"
AV="$OUT/av"
# CPPFLAGS AND LDFLAGS, NOT --with-ogg. libvorbis and libtheora both take a
# --with-ogg=PREFIX, and cross-compiling both ignore it far enough that the
# compiler never sees $AV/root/include — every source failed with
#
#     sharedbook.c:21:10: fatal error: ogg/ogg.h: No such file or directory
#
# Naming the staging tree in the flags the compiler actually reads is the
# reliable spelling, and --with-ogg is kept beside it for the configure checks
# that do consult it.
#
# The log is kept rather than discarded. This is three source builds deep in a
# release script; "could not build them" with nothing to read is a note that
# wastes whoever sees it next.
av_build() {                    # $1=name $2=version $3=url $4=extra configure
    local d="$AV/$1-$2" log="$AV/$1.log"
    [ -f "$d/.built" ] && return 0
    mkdir -p "$AV"
    curl -fsSL "$3" | tar xz -C "$AV" || return 1
    ( cd "$d" \
      && CPPFLAGS="-I$AV/root/include" LDFLAGS="-L$AV/root/lib" \
         ./configure --host="$TRIPLE" --prefix="$AV/root" \
           --disable-shared --enable-static --disable-oggtest $4 \
      && make -j"$(nproc)" && make install ) >"$log" 2>&1 || {
        echo "  $1 failed; see $log"
        return 1
    }
    touch "$d/.built"
}
AV_CFLAGS=""; AV_LIBS=""
if [ ! -f "$AV/root/lib/libtheoradec.a" ]; then
    echo "==> fetching and building ogg/vorbis/theora for $TRIPLE"
    av_build libogg "$OGG_VER" \
        "https://downloads.xiph.org/releases/ogg/libogg-$OGG_VER.tar.gz" "" &&
    av_build libvorbis "$VORBIS_VER" \
        "https://downloads.xiph.org/releases/vorbis/libvorbis-$VORBIS_VER.tar.gz" \
        "--with-ogg=$AV/root" &&
    av_build libtheora "$THEORA_VER" \
        "https://downloads.xiph.org/releases/theora/libtheora-$THEORA_VER.tar.gz" \
        "--with-ogg=$AV/root --with-vorbis=$AV/root --disable-encode --disable-examples" ||
        echo "  (the startup animation will be skipped)"
fi
if [ -f "$AV/root/lib/libtheoradec.a" ]; then
    AV_CFLAGS="-DTADPOLE_THEORA -I$AV/root/include"
    AV_LIBS="-L$AV/root/lib -ltheoradec -lvorbis -logg"
fi

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
      -DTADPOLE_VERSION="$VERSION" \
      -DCMAKE_BUILD_TYPE=Release >/dev/null
ninja -C "$OUT/glasspole" glasspole
# glasspole.exe used to carry no version at all — see glasspole/CMakeLists.txt.
# It is the executable a user is most likely to run by hand, and it was the
# least able to say what it was.
check_stamp "$OUT/glasspole/glasspole.exe"

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
# Compiled from source every time — there are no object files to go stale, so
# this branch never had the "make skipped the relink" hazard the Makefile grew
# .tadpole-version to close. check_stamp confirms that rather than assuming it.
"$TRIPLE-gcc" -O2 -std=gnu17 -DTADPOLE_VERSION="\"$VERSION\"" \
    -o "$OUT/tadpole-view.exe" \
    "$V/tadpole_view.c" "$V/tadpole_ui.c" "$V/tadpole_hle.c" \
    "$V/tadpole_boot.c" $ICON_OBJ \
    $SDL_CFLAGS $Z_CFLAGS $AV_CFLAGS \
    $SDL_LIBS -lopengl32 $Z_LIBS $AV_LIBS -lshlwapi -static -mconsole
check_stamp "$OUT/tadpole-view.exe"

echo "==> tadpole.exe (launcher)"
"$TRIPLE-gcc" -O2 -DTADPOLE_VERSION="\"$VERSION\"" \
    -o "$OUT/tadpole.exe" "$V/tadpole_launcher.c" $ICON_OBJ \
    -static -municode -mwindows
check_stamp "$OUT/tadpole.exe"

# ---- hle-probe.exe ---------------------------------------------------------
# THE ONE DIAGNOSTIC THAT CANNOT BE RUN FROM HERE. Everything else about a
# Windows problem can be reasoned out of the binaries on this machine; what
# the user's GPU driver actually offers cannot. hle_probe.c says it plainly:
# on Windows the operating system's fallback renderer is GDI OpenGL 1.1 with
# no FBOs at all, and GLES 1.x has no shaders, so HLE needs a real
# compatibility profile from a real driver. If SDL lands on the GDI fallback
# the replay renders wrong rather than failing, which is the hardest kind of
# wrong to diagnose from a photograph.
#
# It is a few seconds of build time and it ships, so the answer is always one
# command away on the machine that has the problem. -mconsole because the
# probe's output IS its product.
echo "==> hle-probe.exe"
"$TRIPLE-gcc" -O2 -std=gnu17 -o "$OUT/hle-probe.exe" "$V/hle_probe.c" \
    $SDL_CFLAGS $SDL_LIBS -lopengl32 -static -mconsole || {
        echo "  (hle-probe did not build — not fatal)" >&2
        rm -f "$OUT/hle-probe.exe"; }

fi   # end of the cross-compiled branch

# ---- staging --------------------------------------------------------------
STAGE="$OUT/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/tadpole/viewer/build" "$STAGE/glasspole/build" "$STAGE/tools" \
         "$STAGE/runtime"
cp "$OUT/tadpole.exe"                    "$STAGE/tadpole.exe"
# Ships beside the binaries so a rendering complaint can be answered with a
# measurement instead of a round trip. See the note where it is built.
[ -f "$OUT/hle-probe.exe" ] && cp "$OUT/hle-probe.exe" "$STAGE/"
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
# -L DEREFERENCES, and it is the whole point of this loop on Windows.
#
# Three of these files are symlinks in git (mode 120000):
#   libGLESv1_CM.so.1   -> libGLESv1_CM.so     the SONAME every title links
#   libEGL.so.1         -> libEGL.so
#   libopengles_lite.so -> libGLESv1_CM.so     the name NATIVE 3D titles link
#
# Windows git defaults to core.symlinks=false, so a checkout writes them as
# TEXT FILES containing the target's name — 15 bytes, not an ELF. A plain
# `cp -r` then faithfully copies either a symlink (Linux build host) or a
# 15-byte stub (Windows build host that never ran `make gl`), and the shipped
# tree has no working libopengles_lite.so either way. The loader resolves
# DT_NEEDED by filename, finds the stub, cannot load it, and falls through to
# runtime/libs and the STOCK VR5 driver — which is the exact "native 3D titles
# were never using our OpenGL at all" bug, re-created on Windows by a
# different mechanism. It does not fail loudly; it renders wrong.
#
# -L copies what the link POINTS AT, so the staged tree holds three real
# copies of the library under the three names the loader looks for, on every
# build host, whether or not `make gl` ran and whatever core.symlinks says.
for d in shimlibs shimlibs-z shimlibs-gl; do
    [ -d "$PROJ/runtime/$d" ] && cp -rL "$PROJ/runtime/$d" "$STAGE/runtime/"
done

# -L HAS NOTHING TO FOLLOW WHEN THE CHECKOUT ITSELF LOST THE SYMLINK.
# Building from a Windows clone (core.symlinks=false, and no `make gl` to
# re-create the aliases via MSYS's copying ln) leaves a text stub on disk, not
# a link, so the copy above faithfully staged the stub. Repair the staged tree
# directly: the stub's entire content is the name of the file it should have
# been, and no real shared library is under 256 bytes.
for d in shimlibs shimlibs-z shimlibs-gl; do
    for f in "$STAGE/runtime/$d"/*; do
        [ -f "$f" ] || continue
        [ "$(wc -c < "$f")" -lt 256 ] || continue
        t="$(tr -d '\r\n' < "$f")"
        case "$t" in ""|*/*) continue ;; esac
        [ -f "$(dirname "$f")/$t" ] || continue
        cp -f "$(dirname "$f")/$t" "$f"
        echo "    materialised $d/$(basename "$f") -> $t"
    done
done

# cart2tar.py and fatread.py go together — the converter is the front end and
# the FAT reader is all of the work. Both are pure stdlib on purpose, so the
# bundled 3.7 runs them with nothing added.
#
# THIS LIST IS THE WHOLE CONTRACT WITH WINDOWS, and it is easy to forget: the
# viewer's .sh -> .py port table in tadpole_view.c decides what a menu item
# RUNS, and this decides what actually SHIPS. A tool wired up there but missing
# here fails at the user with Python's own "can't open file" — which is how
# Didj setup shipped broken in 11082026-0003, since install-didj.py was mapped
# and never staged. The `|| true` below is why nobody noticed at build time, so
# it is now a hard failure instead.
for t in install-game.py scan-games.py check-update.py fetch-firmware.py \
         install-firmware.py online-update.py make-profile.py \
         erase-firmware.py cart2tar.py fatread.py netssl.py \
         install-didj.py \
         pkgtool.py fix-perms.py lf3.py scan-games.sh packagelists; do
    cp -r "$PROJ/tools/$t" "$STAGE/tools/" \
        || { echo "could not stage tools/$t — it is in the ship list and" \
                  "missing, so the feature that runs it would fail at the" \
                  "user rather than here" >&2; exit 1; }
done

# EVERY PORTED TOOL THE VIEWER CAN INVOKE IS PRESENT. The check above catches a
# name that is in this list and absent from tools/; this catches the opposite
# and more likely mistake — a tool added to the port table and never added to
# this list at all.
missing=""
for t in $(sed -n 's/.*{ "tools\/[a-z0-9-]*\.sh", *"tools\/\([a-z0-9-]*\.py\)".*/\1/p' \
                  "$PROJ/tadpole/viewer/tadpole_view.c" | sort -u); do
    [ -f "$STAGE/tools/$t" ] || missing="$missing $t"
done
if [ -n "$missing" ]; then
    echo "the viewer's port table maps to tools that are not staged:$missing" >&2
    echo "  add them to the ship list above" >&2
    exit 1
fi

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

    # VCRUNTIME140.dll NEXT TO THE EXTENSION, not only beside python.exe.
    #
    # Python loads a .pyd with LoadLibraryExW(..., LOAD_WITH_ALTERED_SEARCH_PATH),
    # and that flag REPLACES the executable's directory with the .pyd's own
    # directory in the search order. Every stdlib extension sits in the
    # interpreter's root, next to vcruntime140.dll, so they all resolve it and
    # nothing looks wrong — but lzo.cp37-win_amd64.pyd is installed into
    # Lib\site-packages, where there is no vcruntime, so it alone failed with
    #
    #   ubi_reader is not available to this Python (DLL load failed)
    #
    # after bz2 had already read all 78 packages from the same interpreter.
    # Copying the DLL beside it is the whole fix.
    cp "$PYDIR/vcruntime140.dll" "$PYDIR/Lib/site-packages/" 2>/dev/null || true

    # THE CA BUNDLE, because Windows 7 cannot verify LeapFrog's certificate.
    # Their chain is rooted at DigiCert Global Root G2 (2013) and Windows
    # ships roots via Windows Update, so a fresh 7 does not have it: the
    # download failed verification and reported "cannot reach the server"
    # while the same host browsed fine over http:// in IE. Taken from the
    # build host's Mozilla bundle; see tools/netssl.py for how it is used.
    for ca in /etc/ssl/certs/ca-certificates.crt \
              /etc/pki/tls/certs/ca-bundle.crt \
              /etc/ssl/cert.pem; do
        [ -r "$ca" ] && { cp "$ca" "$PYDIR/cacert.pem"; break; }
    done
    # FATAL, NOT A WARNING. This used to warn and carry on, which means a build
    # host without a CA bundle ships a package whose every HTTPS download —
    # firmware, Didj files, and the update check itself — fails on the Windows 7
    # this bundle exists for, with the failure landing on the user instead of
    # here. A warning in the middle of a long release build is not seen; the
    # release script refuses on far less than this.
    [ -f "$PYDIR/cacert.pem" ] || {
        echo "no CA bundle found on this host, so HTTPS would fail on any" >&2
        echo "  Windows that lacks the DigiCert G2 root — which is every" >&2
        echo "  un-updated Windows 7. Looked in:" >&2
        echo "    /etc/ssl/certs/ca-certificates.crt" >&2
        echo "    /etc/pki/tls/certs/ca-bundle.crt" >&2
        echo "    /etc/ssl/cert.pem" >&2
        echo "  Install your distribution's ca-certificates package." >&2
        exit 1; }
fi
if [ -f "$PYDIR/python.exe" ]; then
    mkdir -p "$STAGE/build/deps"
    cp -r "$PYDIR" "$STAGE/build/deps/python"
    # __pycache__ from a host run would be x86-64 Linux bytecode under a
    # Windows tree: dead weight that Python ignores, so drop it.
    find "$STAGE/build/deps/python" -name __pycache__ -type d \
         -exec rm -rf {} + 2>/dev/null || true
    echo "    bundled Python $PY_VER ($(du -sh "$STAGE/build/deps/python" | cut -f1))"
    # AND IT SURVIVED THE COPY. netssl looks for the bundle beside the
    # interpreter, so it has to be in the STAGED tree, not merely in $PYDIR —
    # a .gitignore, a filter or a stale $PYDIR would each lose it silently and
    # the loss only shows up as an unverifiable certificate on Windows 7.
    [ -f "$STAGE/build/deps/python/cacert.pem" ] || {
        echo "the CA bundle did not reach $STAGE/build/deps/python — HTTPS" >&2
        echo "  would fail on Windows 7. See tools/netssl.py." >&2; exit 1; }
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

# THE VERSION OF THE INSTALLED TREE, and it is read now rather than written and
# forgotten. tools/check-update.py falls back to this file when nothing passed
# it --current, which is every case except the viewer asking on its own behalf:
# only tadpole-view.exe has the version compiled in, so without this the tool
# run by hand out of an installed tree answered `status dev` and offered
# thirteen releases as new.
echo "$VERSION" > "$STAGE/.tadpole-version"

# CHECK THE TREE THAT IS ABOUT TO BE PACKAGED, not the one that was built, and
# check EVERY executable in it rather than the one that happens to draw the
# About box. Everything above verified binaries in $OUT; what NSIS picks up is
# $STAGE, and between the two there is a pile of cp — one stale copy is all it
# takes for the installer to disagree with the build that produced it. Two of
# these three had no version to check at all until now, which is precisely how
# a Windows user ended up running something that called itself "dev".
check_stamp "$STAGE/tadpole.exe"
check_stamp "$STAGE/tadpole/viewer/build/tadpole-view.exe"
check_stamp "$STAGE/glasspole/build/glasspole.exe"
[ "$(cat "$STAGE/.tadpole-version")" = "$VERSION" ] || {
    echo "error: the staged .tadpole-version does not say $VERSION" >&2; exit 1; }

echo "staged: $STAGE"
[ "$WANT_INSTALLER" = 1 ] || { echo "done (no installer asked for)"; exit 0; }

# ---- installer ------------------------------------------------------------
command -v makensis >/dev/null || {
    echo "no makensis — install nsis" >&2; exit 1; }
echo "==> Glasspole-Setup.exe"
makensis -NOCD -DVERSION="$VERSION" -DSTAGE="$STAGE" -DOUTFILE="$OUT/Glasspole-Setup.exe" \
         "$HERE/glasspole.nsi" >/dev/null

# AND THE INSTALLER ITSELF CARRIES IT. The payload is solid-LZMA compressed, so
# this can only see the NSIS header — BrandingText and the DisplayVersion that
# goes into Add/Remove Programs — which is exactly the part -DVERSION controls
# and therefore exactly what this can usefully confirm. The binaries inside were
# checked before staging, where they were still readable.
check_stamp "$OUT/Glasspole-Setup.exe"
echo "built: $OUT/Glasspole-Setup.exe"
