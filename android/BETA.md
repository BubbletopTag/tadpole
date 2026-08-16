# Tadpole for Android — private beta

A LeapPad2 emulator on a phone. This is early: the emulator works, the setup
tooling does not, so getting your system files onto the device is a job for a
computer and `adb`.

**Tadpole contains no LeapFrog code, and this APK contains none either.** You
supply the system files and the games, from a device you own. That is not
paperwork — nothing below will work until you do, because there is nothing in
the app to fall back on.

---

## What you need

| | |
|---|---|
| A phone | Android 8.0 or newer, **arm64** |
| On it | Developer options → USB debugging |
| On a computer | `adb` (your distribution's `android-tools`, or Google's platform-tools) |
| Also on that computer | A working desktop Tadpole, set up from **your own** LeapPad2's firmware |

That last row is the real prerequisite. The phone build cannot install firmware
itself yet — see [Why the setup screens do not work](#why-the-setup-screens-do-not-work)
— so it needs a `runtime/sysroot` that a desktop Tadpole has already built.

**arm64 specifically.** The APK installs on a 32-bit-only device and the
interface runs, but nothing will emulate: the ARM engine is arm64 and there is
no 32-bit build of it. If `adb shell getprop ro.product.cpu.abilist64` prints
nothing, this is not going to work on that phone.

---

## 1. Install

```sh
adb install -r Tadpole-android-0.1-beta.apk
```

Launch it once. It will send you to a **Settings** page asking for *All files
access* — grant it. Tadpole browses the filesystem with its own file browser
and opens what it finds by path, so the Android document picker is no use to
it.

You should get a menu bar and a setup wizard. That is the app working; it has
no firmware yet.

---

## 2. Put your firmware on the phone

### The easy way

If you have this repository checked out beside your desktop Tadpole:

```sh
TADPOLE_SRC=/path/to/your/leappad-emu ./android/push-firmware.sh
```

It needs `adb` and `tar` and nothing else — no Android SDK. It packs, pushes,
unpacks and then proves it by running a guest binary; the last line should be
`GUEST-OK`.

### By hand

Same thing, if you would rather see it. Run these from your desktop Tadpole
checkout, with `$OLD` set to its absolute path:

```sh
OLD=$(pwd)          # your desktop Tadpole checkout
PKG=org.tadpole.view
F=/data/user/0/$PKG/files

# 1. Pack. NOT tar -h: the symlinks inside the tree are meaningful.
tar cf /tmp/tp.tar     -C "$OLD/runtime" sysroot shimlibs shimlibs-gl shimlibs-z libs
tar cf /tmp/rootfs.tar -C "$OLD"         rootfs

# 2. Push. One big file, because adb push cannot create symlinks at all.
adb push /tmp/tp.tar     /data/local/tmp/
adb push /tmp/rootfs.tar /data/local/tmp/

# 3. Unpack INTO THE APP. The shell reads and run-as writes: the app's user
#    cannot read /data/local/tmp, and the shell cannot write into the app.
adb shell "cat /data/local/tmp/tp.tar | run-as $PKG sh -c 'mkdir -p files/runtime && tar xf - -C files/runtime'"
adb shell "cat /data/local/tmp/rootfs.tar | run-as $PKG tar xf - -C files"

# 4. Re-point the symlinks. The sysroot is a link farm built with your
#    desktop's paths baked in, and none of them exist on a phone.
adb shell "run-as $PKG sh -c '
cd $F || exit 1
for l in \$(find runtime -type l); do
  t=\$(readlink \"\$l\")
  case \"\$t\" in
    $OLD/*) rm -f \"\$l\"; ln -s \"$F/\${t#$OLD/}\" \"\$l\";;
  esac
done
echo done'"

# 5. Tidy up — that is 1.2 GB sitting in /data/local/tmp.
adb shell rm -f /data/local/tmp/tp.tar /data/local/tmp/rootfs.tar
```

Two failures worth recognising, because both look like something else:

- **`adb push` of the sysroot directly** fails with `remote symlink failed:
  Permission denied` a few entries in. That is why it goes as a tar.
- **Skipping step 4** gives you an app that says the system files are missing
  while plainly having them, or a guest that dies with
  `can't load library 'libVideoMPI.so'`. Both are dangling symlinks.

If the tar step produces a file of about 130 KB rather than about 1.1 GB, your
`runtime/sysroot` is itself a symlink (a git worktree does this) and `tar`
archived the link. Point `$OLD` at the checkout it resolves to.

---

## 3. Run it

Launch Tadpole, then **File → Run System Menu**. You should get the LeapPad2
home screen, in portrait, with a D-pad and a Home button drawn over it.

Launch titles from that home screen — the app's own *Launch App* is not wired
up on Android yet.

### The controls

The **D-pad** and **Home** button are ours, drawn over the picture. Size,
opacity and which corner they take are in *Options → Controller Settings*, and
they can be turned off there.

There is deliberately no A or B. Titles that use them run under LeapFrog's own
Leapster emulator, which draws its own A and B onto the touchscreen — you will
see them appear on the right in those games.

**ROT** in the menu bar turns the phone as well as the picture: landscape at 0
and 180, portrait at 90 and 270, so four presses walk through every way of
holding it.

---

## Why the setup screens do not work

Everything in *File* and *Options* that installs, scans or downloads anything is
a shell or Python script, and Android has neither — those menu items report that
rather than pretending. Firmware install additionally needs to read a UBIFS
volume, which needs a UBIFS reader with LZO decompression that does not exist in
this codebase on any platform; `tools/install-firmware.py` has always said as
much.

So for now: **set up on a desktop, play on a phone.** That is the reason this
document exists.

---

## What to report

Useful things to say, roughly in order of value:

1. **What device**, and `adb shell getprop ro.product.cpu.abilist`.
2. **Which title**, and what it did — a screenshot beats a description.
3. **The log.** Everything the emulator prints goes to logcat:

   ```sh
   adb logcat -s tadpole
   ```

   That is the whole of Tadpole's diagnostic output — the same lines the desktop
   build writes to a terminal, guest included.

4. If it crashed rather than misbehaved, the tombstone:

   ```sh
   adb logcat | grep -A 40 "signal 11"
   ```

Known and not worth reporting yet: the software rasteriser is used for the home
screen (it encodes no GL); *Launch App* is refused; there is no on-screen
keyboard, so the wizard's profile-name field needs a hardware one.
