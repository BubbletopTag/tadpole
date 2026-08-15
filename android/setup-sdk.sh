#!/bin/sh
# Install the whole Android toolchain, user-local, with no root.
#
# NO sudo AND NO pacman, anywhere. Arch's Android packages are in the AUR, need
# root, and lag the NDK badly; Google's own cmdline-tools do not need any of
# that. Everything lands under ~/Android and can be deleted with `rm -rf`.
#
# What this machine had before it was run: /usr/bin/adb from Arch's
# `android-tools`, and nothing else. No JDK at all — `java` was not a command.
#
# Roughly 6 GB and ten minutes on a fast line. Idempotent: re-running skips
# whatever is already there.
set -e
here=$(cd "$(dirname "$0")" && pwd)

JDK_URL=https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/eclipse
CLT_URL=https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip
GRADLE_URL=https://services.gradle.org/distributions/gradle-8.11.1-bin.zip

dl=$HOME/Android/dl
mkdir -p "$dl"

# 1. A JDK. sdkmanager is a Java program, so this comes first and nothing else
#    works without it. 17 rather than 21 because the Android build tools are
#    tested against it and d8 still wants -source/-target 8 to work.
if [ ! -x "$HOME/Android/jdk/bin/java" ]; then
    echo "=== JDK 17 (Temurin) ==="
    curl -L -o "$dl/jdk.tar.gz" "$JDK_URL"
    mkdir -p "$HOME/Android/jdk"
    tar xzf "$dl/jdk.tar.gz" -C "$HOME/Android/jdk" --strip-components=1
fi
"$HOME/Android/jdk/bin/java" -version

# 2. cmdline-tools, which is what sdkmanager ships in. It MUST end up at
#    $SDK/cmdline-tools/latest — sdkmanager refuses to run from anywhere else,
#    with a message about the SDK root that does not explain the real rule.
if [ ! -x "$HOME/Android/Sdk/cmdline-tools/latest/bin/sdkmanager" ]; then
    echo "=== cmdline-tools ==="
    curl -L -o "$dl/cmdline-tools.zip" "$CLT_URL"
    mkdir -p "$HOME/Android/Sdk/cmdline-tools"
    (cd "$HOME/Android/Sdk/cmdline-tools" && \
     python3 -c 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(".")' "$dl/cmdline-tools.zip" && \
     mv cmdline-tools latest && chmod +x latest/bin/*)
fi

# 3. Gradle. Not used by android/build-apk.sh — see the note at its top — but
#    the SDL project template is a Gradle build and having it here means that
#    path can be tried without another install step.
if [ ! -x "$HOME/Android/gradle/bin/gradle" ]; then
    echo "=== Gradle ==="
    curl -L -o "$dl/gradle.zip" "$GRADLE_URL"
    (cd "$HOME/Android" && \
     python3 -c 'import zipfile,sys; zipfile.ZipFile(sys.argv[1]).extractall(".")' "$dl/gradle.zip" && \
     mv gradle-* gradle && chmod +x gradle/bin/gradle)
fi

. "$here/env.sh"

# 4. Licences, then the packages. `yes` because sdkmanager --licenses is an
#    interactive prompt per licence and there are six of them.
echo "=== licences ==="
yes 2>/dev/null | sdkmanager --sdk_root="$ANDROID_HOME" --licenses >/dev/null || true

echo "=== SDK packages ==="
sdkmanager --sdk_root="$ANDROID_HOME" \
    "platform-tools" \
    "platforms;android-$TADPOLE_ANDROID_SDK_TARGET" \
    "build-tools;35.0.1" \
    "ndk;27.2.12479018" \
    "cmake;3.31.6" \
    "emulator" \
    "system-images;android-35;google_apis;x86_64"

# 5. An emulator to develop the front end against. It is x86_64 and it CANNOT
#    stand in for a real device on the question that matters — the SDK emulator
#    refuses to run ARM guests on an x86 host at all ("QEMU2 emulator does not
#    support arm64 CPU architecture"), and the last armeabi-v7a system image is
#    API 25. It runs the arm64 APK through ndk-translation, which is enough for
#    the viewer and useless for the guest.
if [ ! -d "$HOME/.android/avd/tadpole35.avd" ]; then
    echo "=== AVD ==="
    echo no | avdmanager create avd -n tadpole35 \
        -k "system-images;android-35;google_apis;x86_64" -d pixel_6
fi

cat <<EOF

Done. Everything is under ~/Android; nothing was installed system-wide.

  . android/env.sh                     put it all on PATH
  android/fetch-sdl.sh                 SDL2 source + build, both ABIs
  android/build-apk.sh all             build, install, run, follow the logs

  emulator -avd tadpole35 -no-window & # if no real device is attached
EOF
