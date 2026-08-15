# Tadpole for Android — where the toolchain lives on this machine.
#
# Source it: . android/env.sh
#
# NOTHING HERE IS INSTALLED BY THE DISTRIBUTION. Arch's `android-sdk` packages
# are in the AUR, need root, and lag the NDK by a year or more; everything below
# is user-local under ~/Android and was fetched with `sdkmanager`. The one
# exception is `/usr/bin/adb`, from Arch's `android-tools` — it is the older
# client (37.0.0) and it fights with the SDK's own server if both are used, so
# this file puts the SDK's platform-tools FIRST on PATH and every script here
# calls "$ADB" rather than `adb`.
#
# If you have never run any of this: android/setup-sdk.sh installs the lot from
# scratch, and needs no root.

export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/27.2.12479018}"
export ANDROID_NDK_ROOT="$ANDROID_NDK_HOME"
export JAVA_HOME="${JAVA_HOME:-$HOME/Android/jdk}"
export GRADLE_HOME="${GRADLE_HOME:-$HOME/Android/gradle}"

BUILD_TOOLS="$ANDROID_HOME/build-tools/35.0.1"
export BUILD_TOOLS
export ADB="$ANDROID_HOME/platform-tools/adb"

export PATH="$ANDROID_HOME/platform-tools:$BUILD_TOOLS:$JAVA_HOME/bin:$GRADLE_HOME/bin:$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/emulator:$PATH"

# WHAT WE TARGET, AND WHY armeabi-v7a IS FIRST IN THE LIST.
#
# minSdk 26 (Android 8.0) is a deliberate reach downwards, to cheap old tablets
# — the class of hardware someone actually wants a LeapPad emulator on. NDK r27
# still supports 21, so 26 costs nothing.
#
# armeabi-v7a is the PRIMARY ABI, not a courtesy build. On a v7a APK the app
# process is itself ARM32, the same architecture as the guest's stock LeapFrog
# uClibc binaries — which is the difference between "port a JIT" and "solve a
# loader problem". See android/NOTES-arm32.md for what that is worth and what
# it costs.
export TADPOLE_ANDROID_API=26
export TADPOLE_ANDROID_ABIS="armeabi-v7a arm64-v8a"
export TADPOLE_ANDROID_ABI="${TADPOLE_ANDROID_ABI:-armeabi-v7a}"
export TADPOLE_ANDROID_SDK_TARGET=35

# ---- 16 KB PAGES, AND THIS IS NOT OPTIONAL --------------------------------
#
# Android 15 introduced a 16 KB page size and Google requires every app
# targeting API 35 or later to work on it. A .so whose LOAD segments are only
# 4 KB aligned cannot be mapped in place on such a device, and the first build
# of this installed and ran on a real phone and was met with a system dialog:
#
#     This app isn't 16 KB compatible. ELF alignment check failed.
#     lib/arm64-v8a/libSDL2.so : LOAD segment not aligned
#     lib/arm64-v8a/libtadpoleexec.so : LOAD segment not aligned
#     lib/arm64-v8a/libmain.so : LOAD segment not aligned
#
# It ran anyway, because that phone has 4 KB pages and the check is a warning
# there. On a 16 KB device it would not have started at all — and the emulator
# never said a word, which is the whole argument for testing on hardware.
#
# NDK r28 aligns to 16 KB by default. r27 does not, so every link that produces
# something shipped inside the APK passes this, and zipalign stores the entries
# on a 16 KB boundary to match. Both halves are needed: the segments have to be
# aligned within the file AND the file has to be placed on a boundary.
export TADPOLE_ANDROID_LDFLAGS="-Wl,-z,max-page-size=16384"
