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
