#!/bin/sh
# Fetch SDL2's source and build it for every ABI we target.
#
# PINNED, and it has to be. libSDL2.so ships inside the APK and SDLActivity
# binds to it by JNI method name, so the .java and the .so must come from the
# same SDL — a mismatched pair fails at RUNTIME with UnsatisfiedLinkError, not
# at build time. android/build-apk.sh compiles the Java out of this same tree
# for exactly that reason.
#
# 2.32.10 is the last of the 2.x line. Not SDL3: the viewer is 13k lines of SDL2
# and Tadpole's desktop build is SDL2, so changing major version and platform in
# the same step would make every failure ambiguous.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
. "$here/env.sh"

VER=2.32.10
URL=https://github.com/libsdl-org/SDL/releases/download/release-$VER/SDL2-$VER.tar.gz
dst=$root/build/android

mkdir -p "$dst"
if [ ! -d "$dst/SDL2-$VER" ]; then
    echo "=== SDL2 $VER ==="
    curl -L -o "$dst/SDL2-$VER.tar.gz" "$URL"
    tar xzf "$dst/SDL2-$VER.tar.gz" -C "$dst"
    rm -f "$dst/SDL2-$VER.tar.gz"
fi

SDL_SRC=$dst/SDL2-$VER "$here/sdl/build-sdl.sh"
