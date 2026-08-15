#!/bin/sh
# Glasspole for Android arm64 — how far it gets today, reproducibly.
#
# `dynarmic` is the target that matters and it BUILDS: 126 objects, including
# the whole of backend/arm64, linked into an AArch64 libdynarmic.a. That is the
# load-bearing fact of this whole port and it is why this script exists — so the
# next person can re-establish it in one command instead of rediscovering it.
#
#     ./android/glasspole/build-glasspole.sh dynarmic    # works
#     ./android/glasspole/build-glasspole.sh             # 3 known errors, below
#
# ### 16 KB pages
#
# The engine ships INSIDE the APK, so it is subject to the same alignment rule
# as libmain.so and libSDL2.so — see the note in android/env.sh. It was missed
# here at first and the phone said so by name:
#
#     lib/arm64-v8a/libglasspole.so : LOAD segment not aligned
#
# ### Boost, and why it is fed in through a symlink farm
#
# dynarmic needs Boost headers (boost/variant.hpp, boost/icl) and an NDK sysroot
# has none. Boost is header-only for what dynarmic uses, so the host's copy is
# the right answer for a cross build — but -isystem /usr/include would put the
# host's glibc headers on the include path of an Android compile, which goes
# wrong in ways that take an afternoon to understand. So this makes a directory
# containing exactly one entry, a symlink to /usr/include/boost, and points
# -isystem at that.
#
# ### The three things that stop the rest, all in Glasspole's own sources
#
# Written down rather than fixed, because glasspole/src/ is shared with main and
# another agent commits there; these are one-line changes each and they belong in
# a commit that can be reviewed against the desktop build.
#
# 1. src/signal.cpp:46-49 — four `constexpr uint32_t` collide with bionic macros
#
#        constexpr uint32_t SA_SIGINFO   = 0x00000004;
#        error: expected unqualified-id
#        note: expanded from macro 'SA_SIGINFO'  [asm-generic/signal-defs.h]
#
#    SA_SIGINFO, SA_RESTORER, SA_NODEFER and SA_RESETHAND are all macros in the
#    NDK's uapi headers. glibc does not expose them to this translation unit and
#    bionic does. The values are the GUEST's, so they must not become the host's:
#    #undef before, or rename to GP_SA_*.
#
# 2. src/cpu.cpp:81 — std::atomic_ref does not exist in the NDK's libc++
#
#        std::atomic_ref<T> ref(*reinterpret_cast<T *>(m->Ptr(addr)));
#        error: no member named 'atomic_ref' in namespace 'std'
#
#    C++20, and NDK r27's libc++ has not got it. This is the ldrex/strex
#    compare-exchange on guest memory, so it cannot just be dropped;
#    __atomic_compare_exchange_n(p, &exp, value, false, __ATOMIC_SEQ_CST,
#    __ATOMIC_SEQ_CST) is the same operation and is a clang builtin.
#
# 3. Not yet reached: host_posix.c against bionic, and the 4 GiB reservation in
#    cpu.cpp (GUEST_SPACE = 0x100000000). The reservation is fine on a 64-bit
#    host; it wants MAP_NORESERVE, and it is the single reason an armeabi-v7a
#    Glasspole is impossible rather than merely unsupported.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
. "$here/../env.sh"

[ -e "$root/glasspole/deps/dynarmic" ] || {
    echo "no dynarmic: run glasspole/fetch-deps.sh, or symlink glasspole/deps" >&2
    exit 1
}

boost=$root/build/android/boost-inc
mkdir -p "$boost"
[ -e "$boost/boost" ] || ln -s /usr/include/boost "$boost/boost"

# NAMED FOR THE ABI, like sdl-$abi and viewer-$abi beside it, because
# build-apk.sh packages lib/<abi>/ from a directory it derives from the ABI.
# When this was "glasspole-arm64" the packager looked for "glasspole-arm64-v8a",
# found nothing, and skipped the engine without saying so — the app then
# reported "no libglasspole.so in this APK", which reads as a build that never
# happened rather than one filed under the wrong name.
b=$root/build/android/glasspole-arm64-v8a
cmake -S "$root/glasspole" -B "$b" -GNinja \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a \
    -DANDROID_PLATFORM="android-$TADPOLE_ANDROID_API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXE_LINKER_FLAGS="$TADPOLE_ANDROID_LDFLAGS" \
    -DCMAKE_SHARED_LINKER_FLAGS="$TADPOLE_ANDROID_LDFLAGS" \
    -DBoost_INCLUDE_DIR="$boost" \
    -DCMAKE_CXX_FLAGS="-isystem $boost" \
    -DCMAKE_POLICY_DEFAULT_CMP0167=OLD

cmake --build "$b" --parallel ${1:+--target "$1"}

lib=$b/deps/dynarmic/src/dynarmic/libdynarmic.a
if [ -f "$lib" ]; then
    echo "--- libdynarmic.a ---"
    ls -la "$lib"
    NDKBIN=$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin
    echo "arm64 backend objects: $("$NDKBIN/llvm-ar" t "$lib" | grep -c arm64)"
fi
