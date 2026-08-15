#!/bin/sh
# Build SDL2 for Android, once per ABI, with the NDK's own CMake toolchain.
#
# WHICH SDL, AND WHY NOT THE SYSTEM ONE. Android has no system SDL. SDL2's
# Android support is first-class but it is a source-level thing: libSDL2.so is
# built as part of your app and shipped inside your APK, and the Java side
# (SDLActivity) has to match the .so exactly, so "which SDL" is a version you
# pin rather than a package you install. 2.32.10 is the last of the 2.x line.
#
# NOT SDL3. The viewer is ~13k lines of SDL2 calls and Tadpole's desktop build
# is SDL2; changing major version and platform at the same time would make every
# failure ambiguous.
#
# BOTH ABIs, v7a FIRST. See android/NOTES-arm32.md — on an armeabi-v7a build the
# app process is ARM32, which is the same architecture as the guest binaries,
# and that is the whole of why this is worth trying.
set -e
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../env.sh"

root=$(cd "$here/../.." && pwd)
src=${SDL_SRC:-$root/build/android/SDL2-2.32.10}
prefix=$root/build/android/sdl-prefix

[ -d "$src" ] || { echo "no SDL2 source at $src — run android/fetch-sdl.sh" >&2; exit 1; }

for abi in $TADPOLE_ANDROID_ABIS; do
    echo "=== SDL2 for $abi (API $TADPOLE_ANDROID_API) ==="
    b=$root/build/android/sdl-$abi
    cmake -S "$src" -B "$b" -GNinja \
        -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$abi" \
        -DANDROID_PLATFORM="android-$TADPOLE_ANDROID_API" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$prefix/$abi" \
        -DCMAKE_SHARED_LINKER_FLAGS="$TADPOLE_ANDROID_LDFLAGS" \
        -DCMAKE_EXE_LINKER_FLAGS="$TADPOLE_ANDROID_LDFLAGS" \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
    cmake --build "$b" --parallel
    cmake --install "$b" >/dev/null
    echo "--- $abi ---"
    ls -la "$prefix/$abi/lib/libSDL2.so"
    "$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" \
        -h "$prefix/$abi/lib/libSDL2.so" | grep -E 'Class|Machine'
done

# The Java side. SDL ships it in the source tree and the version MUST match the
# .so — SDLActivity does its own JNI binding by name, so a mismatched pair fails
# at runtime with UnsatisfiedLinkError rather than at build time.
echo "=== SDL Java sources ==="
find "$src/android-project/app/src/main/java" -name '*.java' | wc -l
