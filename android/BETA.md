# Tadpole for Android — beta

A LeapPad2 emulator on a phone or tablet. It now sets itself up: it downloads
its own system files, installs them, and boots — no computer, no `adb`, and no
root.

**Tadpole contains no LeapFrog code, and this APK contains none either.** You
supply the system files and the games. The system files come from LeapFrog's
own public content server, which is where the device itself got them; games
come from cartridges or downloads you own.

---

## What you need

| | |
|---|---|
| A device | Android 8.0 or newer |
| On it | Wi-Fi, and about 1.5 GB free |
| Optional | A computer with `adb`, only if you want to sideload games |

**No root.** The guest runs inside the app's own sandbox, under the same
seccomp filter Android puts every app under. There is a root helper script in
the repository and it still works, but nothing needs it any more.

### About your device's CPU — read this bit

This is the one thing that decides whether it will work for you.

- **32-bit ARM (armeabi-v7a)** — the good case, and the one this beta is tested
  on. The LeapPad2's own binaries are 32-bit ARM, so on a 32-bit device they
  run **natively**: there is no emulator in the loop at all, which is why this
  is usable on cheap hardware.
- **64-bit ARM (arm64)** — the full APK carries a 39 MB arm64 build of the
  Glasspole engine for this case, since a 64-bit process cannot run the
  guest's 32-bit binaries directly. **It has not been tested on a real arm64
  device.** If you try it, that is exactly the report worth sending — say
  whether the wizard's welcome page reports an engine or says "No ARM engine
  installed".

There are two downloads for that reason. The **v7a** build is about 7 MB and is
the tested one; the **universal** build is 51 MB, almost all of it that engine,
and is the one to try on a 64-bit phone.

Check with `adb shell getprop ro.product.cpu.abilist`, or any CPU-info app.

Tested on: an AOSP 8.1 tablet, armeabi-v7a only, Mali-T720.

---

## 1. Install

```sh
adb install -r Tadpole-android-0.2-beta.apk
```

Launch it once. It will send you to a **Settings** page asking for *All files
access* — grant it. Tadpole browses the filesystem with its own file browser
and opens what it finds by path, so the Android document picker is no use to
it.

You should get a menu bar and a setup wizard. That is the app working; it has
no firmware yet.

---

## 2. Get the system files

### On the device, with no computer — do this one

Open **Help → Setup Wizard**, go to *System files*, and press **Online System
Update**. It downloads the system packages from LeapFrog's own content server
and installs them: firmware, apps, widgets, language pack, music.

Expect it to take a while — roughly 350 MB and about twenty minutes on slow
hardware — and expect it to tell you where it is the whole time. Anything it
has already downloaded is kept, so if it fails part way, press it again and it
picks up rather than starting over.

When it says *Installed. Tadpole can boot the system menu now*, close the
panel and use **File → Run System Menu**. The LeapPad's own first-boot flow
runs: language, date, and a profile.

The wizard's **Didj support** page works the same way, if you want Didj titles:
two Download buttons, no computer.

### From a desktop Tadpole you already have

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

## What is not ported yet

The setup tooling used to be shell and Python, which Android has neither of, so
every menu item that installed anything reported that rather than pretending.
It has since been rewritten in Java — including a UBIFS reader with LZO and
bzip2 decompression, which is what reading a LeapPad2 firmware image actually
requires. Install, erase, scan, content, Didj support and the online update all
work on the device now.

Three things still say "not available on Android yet", honestly rather than
half-working:

- **micromods** — per-title extras; a lot of network etiquette to get right.
- **cart2tar** — converting a raw cartridge dump, which is a desktop job
  anyway: the dump arrives over FTP from a real LeapPad.
- **make-profile** — and this one does not matter much. The guest creates its
  own profile in its own first-boot flow, which is what you will see.

Known rough edges in this build:

- The progress bar fills, then restarts for the next phase of an install.
  Honest per phase, slightly odd end to end.
- If the guest is killed uncleanly, a stale `.lock` can leave *Run System Menu*
  greyed out as "running". Restart the app.
- Didj titles need their compatibility files, which the wizard's **Didj
  support** page downloads for you.

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
