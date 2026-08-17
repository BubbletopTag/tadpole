# Transplanting your firmware to an Android device

Tadpole for Android runs the LeapPad2's own system software, and it cannot
install that software itself yet — the tools that do it on a desktop are shell
and Python scripts, and Android has neither. So the system files are prepared
on a computer and copied across with `adb`. That is what this page is for.

**You supply the firmware, from a LeapPad2 you own.** Tadpole contains no
LeapFrog code and neither does the APK. Nothing below works until you have your
own system files, because there is nothing in the app to fall back on.

> The full project README, the emulator's own documentation and the desktop
> build instructions are on the `main` branch. This branch is the Android
> experiment.

---

## What you need

| | |
|---|---|
| A phone | Android 8.0 or newer, **arm64** |
| On it | Developer options → USB debugging, and Tadpole installed |
| On a computer | `adb` and `tar`. No Android SDK. |
| Also | A desktop Tadpole whose `runtime/sysroot` is already built from your own device's firmware |

That last row is the real prerequisite: this copies a prepared sysroot, it does
not create one. Use the desktop build's Setup Wizard first.

**arm64 matters.** The APK installs on a 32-bit-only phone and the interface
runs, but nothing emulates — the ARM engine is arm64 and there is no 32-bit
build of it. Check with:

```sh
adb shell getprop ro.product.cpu.abilist64
```

If that prints nothing, this will not work on that device.

---

## Before you copy anything

Install the app and launch it once, so its storage exists and the copy has
somewhere to land:

```sh
adb install -r Tadpole-android-0.1-beta.apk
```

On first launch it sends you to a Settings page asking for **All files access**.
Grant it — Tadpole browses the filesystem with its own file browser and opens
what it finds by path, so the Android document picker is no use to it.

---

## The easy way

From this branch, pointed at your desktop Tadpole checkout:

```sh
TADPOLE_SRC=/path/to/your/leappad-emu ./android/push-firmware.sh
```

It packs, pushes, unpacks, repairs the symlinks and then proves it by running a
guest binary. The last line should be:

```
GUEST-OK
```

That is a stock LeapFrog ARM32 binary executing on your phone. If you see it,
you are done — launch Tadpole and pick **File → Run System Menu**.

It moves about 1.2 GB and takes under a minute on USB 3.

---

## The manual way

The same steps, if you would rather see them. Run from your desktop Tadpole
checkout:

```sh
OLD=$(pwd)                       # your desktop Tadpole checkout
PKG=org.tadpole.view
F=/data/user/0/$PKG/files

# 1. Pack. NOT tar -h — the symlinks inside the tree are meaningful.
tar cf /tmp/tp.tar     -C "$OLD/runtime" sysroot shimlibs shimlibs-gl shimlibs-z libs
tar cf /tmp/rootfs.tar -C "$OLD"         rootfs

# 2. Push. One big file each, because adb push cannot create symlinks at all.
adb push /tmp/tp.tar     /data/local/tmp/
adb push /tmp/rootfs.tar /data/local/tmp/

# 3. Unpack INTO the app. The shell reads and run-as writes: the app's user
#    cannot read /data/local/tmp, and the shell cannot write into the app.
adb shell "cat /data/local/tmp/tp.tar | run-as $PKG sh -c 'mkdir -p files/runtime && tar xf - -C files/runtime'"
adb shell "cat /data/local/tmp/rootfs.tar | run-as $PKG tar xf - -C files"

# 4. Repair the symlinks. The sysroot is a link farm with your desktop's paths
#    baked into it, and none of them exist on a phone.
adb shell "run-as $PKG sh -c '
cd $F || exit 1
n=0
for l in \$(find runtime -type l); do
  t=\$(readlink \"\$l\")
  case \"\$t\" in
    $OLD/*) rm -f \"\$l\"; ln -s \"$F/\${t#$OLD/}\" \"\$l\"; n=\$((n+1));;
  esac
done
echo \"re-pointed \$n\"'"

# 5. Tidy up — that is 1.2 GB sitting in /data/local/tmp.
adb shell rm -f /data/local/tmp/tp.tar /data/local/tmp/rootfs.tar
```

Then check it worked:

```sh
adb shell "run-as org.tadpole.view sh -c 'cd /data/user/0/org.tadpole.view/files && \
    ./glasspole/build/glasspole --sysroot \
    /data/user/0/org.tadpole.view/files/runtime/sysroot /bin/busybox echo GUEST-OK'"
```

---

## Four things that go wrong, and what they look like

**`adb push` of the sysroot directly fails.**

```
adb: error: failed to copy '.../sysroot/boot': remote symlink failed: Permission denied
```

`adb push` cannot create symlinks, and the tree is full of them — it dies about
four entries in. That is why everything travels as a tar.

**A 130 KB tarball instead of 1.1 GB.** Your `runtime/sysroot` is itself a
symlink — a git worktree does exactly this — and `tar` archived the link rather
than following it. Point `$OLD` at the checkout the link resolves to.

**The app says the system files are missing while plainly having them.** Step 4
was skipped, or only half worked. The sysroot's top level has eight absolute
symlinks and the rest of the tree has two hundred more, including `LF/Base`,
which is the one the app tests for. `find runtime -type l`, not just the top
level.

**A guest that dies with `can't load library 'libVideoMPI.so'`.** Same cause,
different victim: `runtime/libs` is a link farm of its own, 168 entries deep. It
has to be both copied and repaired.

---

## Why the app cannot do this itself

Everything in **File** and **Options** that installs, scans or downloads is a
shell or Python script; those menu items say so rather than pretending.
Firmware installation additionally needs to read a UBIFS volume, which needs a
UBIFS reader with LZO decompression that does not exist in this codebase on any
platform — `tools/install-firmware.py` has always said as much.

So for now: **set up on a desktop, play on a phone.**

---

## Once it is on there

`android/BETA.md` covers installing, the on-screen controls, what works and
what does not, and what is worth reporting. `android/NOTES-arm32.md` explains
why the engine is arm64-only, and `android/NOTES-camera.md` is a separate
investigation into wiring the phone's camera to the LeapPad's.
