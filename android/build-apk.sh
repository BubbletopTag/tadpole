#!/bin/sh
# Tadpole for Android — configure, build, package, install, run, and get the
# logs back. One script, because the last step is the one that matters and it
# is the one an IDE-shaped workflow leaves out.
#
# Usage:
#     android/build-apk.sh                 build both ABIs and package
#     android/build-apk.sh install         ... and install to the device
#     android/build-apk.sh run             ... and launch it
#     android/build-apk.sh logs            follow the app's output
#     android/build-apk.sh all             build, install, run, follow logs
#
# NO GRADLE. Not on principle — the SDL template's Gradle build works — but
# because the whole native side of this is CMake driven by the NDK toolchain
# file and the Java side is nine files SDL wrote plus one we did. Gradle would
# add an AGP download, a Kotlin compiler and a daemon to a build that is
# currently five commands, and every one of those is a thing that can fail for
# reasons unrelated to Tadpole. If the Java side ever grows, revisit.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
. "$here/env.sh"

PKG=org.tadpole.view
ACT=$PKG/.TadpoleActivity
SDL_SRC=${SDL_SRC:-$root/build/android/SDL2-2.32.10}
out=$root/build/android/apk
PLATFORM=$ANDROID_HOME/platforms/android-$TADPOLE_ANDROID_SDK_TARGET/android.jar

do_build() {
    for abi in $TADPOLE_ANDROID_ABIS; do
        b=$root/build/android/viewer-$abi
        cmake -S "$here/viewer" -B "$b" -GNinja \
            -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake" \
            -DANDROID_ABI="$abi" \
            -DANDROID_PLATFORM="android-$TADPOLE_ANDROID_API" \
            -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$b"
    done
}

do_package() {
    rm -rf "$out"; mkdir -p "$out/gen" "$out/classes" "$out/res/values"

    cat > "$out/res/values/strings.xml" <<'XML'
<?xml version="1.0" encoding="utf-8"?>
<resources><string name="app_name">Tadpole</string></resources>
XML

    "$BUILD_TOOLS/aapt2" compile --dir "$out/res" -o "$out/res.zip"
    "$BUILD_TOOLS/aapt2" link -o "$out/unsigned.apk" -I "$PLATFORM" \
        --manifest "$here/app/AndroidManifest.xml" --java "$out/gen" \
        --min-sdk-version "$TADPOLE_ANDROID_API" \
        --target-sdk-version "$TADPOLE_ANDROID_SDK_TARGET" "$out/res.zip"

    # SDL's own Java, straight out of the source tree. The .java and the .so
    # MUST come from the same SDL: SDLActivity binds to native methods by name
    # through JNI, so a mismatched pair fails at RUNTIME with
    # UnsatisfiedLinkError and not at build time.
    "$JAVA_HOME/bin/javac" -source 8 -target 8 -nowarn \
        -classpath "$PLATFORM" -d "$out/classes" \
        $(find "$SDL_SRC/android-project/app/src/main/java" -name '*.java') \
        $(find "$here/app/java" "$out/gen" -name '*.java')
    "$BUILD_TOOLS/d8" --min-api "$TADPOLE_ANDROID_API" --output "$out" \
        --lib "$PLATFORM" $(find "$out/classes" -name '*.class')

    python3 - "$out" "$root" "$TADPOLE_ANDROID_ABIS" <<'PY'
import sys, os, zipfile
out, root, abis = sys.argv[1], sys.argv[2], sys.argv[3].split()
with zipfile.ZipFile(os.path.join(out, 'unsigned.apk'), 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(out, 'classes.dex'), 'classes.dex')
    for abi in abis:
        # STORED, not DEFLATED, and this is not cosmetic: since Android 6 the
        # loader can map a .so straight out of an uncompressed, aligned APK
        # instead of copying it to /data first. android:extractNativeLibs then
        # defaults to false and the app takes half the space on device.
        for name, path in (
            ('libSDL2.so', f'{root}/build/android/sdl-prefix/{abi}/lib/libSDL2.so'),
            ('libmain.so', f'{root}/build/android/viewer-{abi}/libmain.so')):
            z.write(path, f'lib/{abi}/{name}', compress_type=zipfile.ZIP_STORED)
print('packaged ABIs:', ' '.join(abis))
PY

    ks=$here/app/debug.keystore
    [ -f "$ks" ] || "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$ks" \
        -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Tadpole Debug,O=Tadpole,C=GB" 2>/dev/null

    # -p 4 page-aligns the STORED .so entries, which is what makes mapping
    # them in place legal.
    "$BUILD_TOOLS/zipalign" -f -p 4 "$out/unsigned.apk" "$out/aligned.apk"
    "$BUILD_TOOLS/apksigner" sign --ks "$ks" --ks-pass pass:android \
        --key-pass pass:android --out "$out/tadpole.apk" "$out/aligned.apk"
    echo "built $out/tadpole.apk"
}

do_install() { "$ADB" install -r "$out/tadpole.apk"; }

do_run() {
    "$ADB" logcat -c
    "$ADB" shell am start -n "$ACT"
}

# THE STEP THAT MAKES THIS WORKABLE. tadpole_jni.c redirects the viewer's
# stdout and stderr into logcat under the tag "tadpole", so this is `run.log`
# and every existing grep in tools/ works on it unchanged. DEBUG:* catches
# native crashes — tombstones print there, with the faulting address and a
# backtrace, which is the Android equivalent of tadpole_crash.c's report.
do_logs() {
    "$ADB" logcat -v time -s tadpole:V SDL:V DEBUG:V AndroidRuntime:E libc:V
}

case "${1:-package}" in
    build)   do_build ;;
    package) do_build; do_package ;;
    install) do_build; do_package; do_install ;;
    run)     do_build; do_package; do_install; do_run ;;
    logs)    do_logs ;;
    all)     do_build; do_package; do_install; do_run; sleep 2; do_logs ;;
    *) echo "usage: $0 {build|package|install|run|logs|all}" >&2; exit 2 ;;
esac
