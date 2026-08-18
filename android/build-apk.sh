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
#     android/build-apk.sh device          what ABIs the attached device has
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
            -DCMAKE_SHARED_LINKER_FLAGS="$TADPOLE_ANDROID_LDFLAGS" \
            -DCMAKE_EXE_LINKER_FLAGS="$TADPOLE_ANDROID_LDFLAGS" \
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

    # THE LAUNCHER ICON, resampled from the SAME tadpole.png the desktop front
    # end uses, so the two builds cannot drift apart. Generated at build time
    # rather than committed five times over: the source is one 173x173 file, and
    # five near-duplicates of it in the tree would be five things to forget about
    # the day the artwork changes.
    #
    # The sizes are Android's launcher densities. 173 is a DOWNSCALE for
    # everything up to xxhdpi, which is where the sharpness comes from; xxxhdpi
    # is a 1.11x upscale, small enough to be invisible and much better than
    # handing the platform one small bitmap and letting it stretch that to 192.
    if [ -f "$root/tadpole.png" ]; then
        python3 - "$root/tadpole.png" "$out/res" <<'ICONPY'
import sys, os
from PIL import Image
src, res = sys.argv[1], sys.argv[2]
im = Image.open(src).convert("RGBA")
for name, px in (("mdpi", 48), ("hdpi", 72), ("xhdpi", 96),
                 ("xxhdpi", 144), ("xxxhdpi", 192)):
    d = os.path.join(res, "mipmap-" + name)
    os.makedirs(d, exist_ok=True)
    im.resize((px, px), Image.LANCZOS).save(os.path.join(d, "ic_launcher.png"))
print("  launcher icon: %s -> 48/72/96/144/192" % os.path.basename(src))
ICONPY
    else
        echo "  no tadpole.png at $root - the APK keeps the stock Android icon" >&2
    fi

    "$BUILD_TOOLS/aapt2" compile --dir "$out/res" -o "$out/res.zip"
    # -A ships android/app/assets verbatim. The viewer reads its logo as a FILE
    # off the project directory rather than as an Android resource, so the PNGs
    # travel as assets and TadpoleActivity unpacks them into that directory on
    # first run — see extractAssets(). Resources would mean teaching the viewer
    # about AssetManager, which is a change to a shared file for a picture.
    "$BUILD_TOOLS/aapt2" link -o "$out/unsigned.apk" -I "$PLATFORM" \
        --manifest "$here/app/AndroidManifest.xml" --java "$out/gen" \
        -A "$here/app/assets" \
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
        entries = [
            ('libSDL2.so', f'{root}/build/android/sdl-prefix/{abi}/lib/libSDL2.so'),
            ('libmain.so', f'{root}/build/android/viewer-{abi}/libmain.so'),
            ('libtadpoleexec.so', f'{root}/build/android/viewer-{abi}/tadpoleexec'),
        ]
        # THE ENGINE TRAVELS AS A LIBRARY BECAUSE THAT IS THE ONLY PLACE IT MAY
        # BE EXECUTED FROM. glasspole is an ordinary PIE executable, but an
        # Android app may not exec a file it wrote — the probe measures that
        # every launch and reports "exec from app files DENIED". It MAY exec
        # something the package installer put in the native library directory,
        # which is the route libtadpoleexec.so was written to prove. So the
        # engine is packaged under a .so name it does not deserve, and
        # TadpoleActivity links the path the viewer looks for to it.
        #
        # arm64 only, and not for want of trying: dynarmic has no 32-bit host
        # backend, so there is no armeabi-v7a engine to package. See
        # android/NOTES-arm32.md.
        gp = f'{root}/build/android/glasspole-{abi}/glasspole'
        if os.path.exists(gp):
            entries.append(('libglasspole.so', gp))
        for name, path in entries:
            z.write(path, f'lib/{abi}/{name}', compress_type=zipfile.ZIP_STORED)
print('packaged ABIs:', ' '.join(abis))
PY

    ks=$here/app/debug.keystore
    [ -f "$ks" ] || "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$ks" \
        -storepass android -keypass android -alias androiddebugkey \
        -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Tadpole Debug,O=Tadpole,C=GB" 2>/dev/null

    # -P 16 page-aligns the STORED .so entries to 16 KB, which is what makes
    # mapping them in place legal on an Android 15 device. The segments inside
    # each .so are aligned to match by TADPOLE_ANDROID_LDFLAGS; see the note in
    # env.sh for why both halves are needed and how the omission announced
    # itself. -p is the old 4 KB form and is what this used to pass.
    # THE 4 IS POSITIONAL AND IS NOT THE PAGE SIZE. zipalign's argument order is
    # [-f] [-p] [-P <kb>] <align> in out — <align> is the ordinary entry
    # alignment in BYTES and has always been 4. -p (4 KB pages) and -P (an
    # explicit page size) are mutually exclusive, so -P 16 replaces -p, and the
    # trailing 4 stays exactly where it was. Dropping it makes zipalign read
    # "unsigned.apk" as the alignment and print its usage.
    "$BUILD_TOOLS/zipalign" -f -P 16 4 "$out/unsigned.apk" "$out/aligned.apk"
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

# The one-minute question this whole port turns on, asked without installing
# anything. See android/NOTES-arm32.md: the property is the ROM's claim and the
# file is whether the 32-bit loader is actually there, and they can disagree.
do_device() {
    "$ADB" devices -l
    for p in ro.product.model ro.build.version.sdk ro.product.cpu.abilist \
             ro.product.cpu.abilist32 ro.product.cpu.abilist64 ro.zygote; do
        printf '%-28s ' "$p"; "$ADB" shell getprop "$p"
    done
    printf '%-28s ' /system/bin/linker
    "$ADB" shell 'ls /system/bin/linker >/dev/null 2>&1 && echo "PRESENT (32-bit userspace)" || echo "ABSENT (64-bit only)"'
}

case "${1:-package}" in
    build)   do_build ;;
    package) do_build; do_package ;;
    install) do_build; do_package; do_install ;;
    run)     do_build; do_package; do_install; do_run ;;
    logs)    do_logs ;;
    device)  do_device ;;
    all)     do_build; do_package; do_install; do_run; sleep 2; do_logs ;;
    *) echo "usage: $0 {build|package|install|run|logs|device|all}" >&2; exit 2 ;;
esac
