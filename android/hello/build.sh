#!/bin/sh
# The smallest APK this machine can make, built WITHOUT Gradle.
#
# Why bother, when the SDL project template ships a Gradle build? Because when
# something later fails, this tells you which half is broken. Gradle downloads
# the Android Gradle Plugin, a Kotlin compiler and a dependency graph from the
# network on first use, and any of that can fail for reasons that have nothing
# to do with the SDK being correctly installed. This script touches aapt2, d8,
# apksigner, zipalign, javac and adb and nothing else — if it produces an
# installed, running app, the SDK itself is sound and a Gradle failure after it
# is a Gradle failure.
#
# It is also the fallback build. Tadpole's viewer is C; the Java side of an
# SDL app is three files that never change. Gradle is convenience here, not a
# requirement, and this script is the proof of that.
set -e
here=$(cd "$(dirname "$0")" && pwd)
. "$here/../env.sh"

out=$here/out
rm -rf "$out"; mkdir -p "$out/gen" "$out/classes"

PLATFORM=$ANDROID_HOME/platforms/android-$TADPOLE_ANDROID_SDK_TARGET/android.jar

# 1. Resources. aapt2 compiles each file to a .flat, then links the lot with
#    the manifest into an APK that has resources and no code yet.
"$BUILD_TOOLS/aapt2" compile --dir "$here/res" -o "$out/res.zip"
"$BUILD_TOOLS/aapt2" link -o "$out/unsigned.apk" \
    -I "$PLATFORM" \
    --manifest "$here/AndroidManifest.xml" \
    --java "$out/gen" \
    --min-sdk-version "$TADPOLE_ANDROID_API" \
    --target-sdk-version "$TADPOLE_ANDROID_SDK_TARGET" \
    "$out/res.zip"

# 2. Java -> class -> dex. Note -source/-target 8: d8 will accept newer, but
#    every JDK past 11 warns that 8 is deprecated and one past 21 will refuse
#    it, so this is a thing to revisit rather than a thing that is settled.
"$JAVA_HOME/bin/javac" -source 8 -target 8 -nowarn \
    -classpath "$PLATFORM" -d "$out/classes" \
    $(find "$here/java" "$out/gen" -name '*.java')
"$BUILD_TOOLS/d8" --min-api "$TADPOLE_ANDROID_API" \
    --output "$out" --lib "$PLATFORM" \
    $(find "$out/classes" -name '*.class')

# 3. Put the dex in the APK, at the root of the archive.
#
#    Python and not `zip`, because this Arch box does not have Info-ZIP
#    installed and pulling it in needs root; python3 is already a hard
#    dependency of Tadpole's tooling (tools/*.py), so it costs nothing here.
#    STORED and not DEFLATED for the dex: it makes no measurable difference to
#    the APK size and it keeps this readable next to what aapt2 already did.
python3 - "$out" <<'PY'
import sys, zipfile, os
out = sys.argv[1]
with zipfile.ZipFile(os.path.join(out, 'unsigned.apk'), 'a', zipfile.ZIP_DEFLATED) as z:
    z.write(os.path.join(out, 'classes.dex'), 'classes.dex')
PY

# 4. Sign. A debug key made on the spot — it exists to satisfy the installer,
#    which refuses an unsigned APK, and it is not a release key.
ks=$here/debug.keystore
[ -f "$ks" ] || "$JAVA_HOME/bin/keytool" -genkeypair -keystore "$ks" \
    -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 \
    -dname "CN=Tadpole Debug,O=Tadpole,C=GB" 2>/dev/null

"$BUILD_TOOLS/zipalign" -f -p 4 "$out/unsigned.apk" "$out/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$ks" --ks-pass pass:android \
    --key-pass pass:android --out "$out/hello.apk" "$out/aligned.apk"

echo "built $out/hello.apk"
"$BUILD_TOOLS/apksigner" verify -v "$out/hello.apk" | head -5
