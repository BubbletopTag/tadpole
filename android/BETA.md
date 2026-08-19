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
- **64-bit ARM (arm64)** — works, through the emulator. The full APK carries a
  39 MB arm64 build of the Glasspole engine, and the app links and runs it for
  you. Verified on a OnePlus (Android 16, Snapdragon): installed, downloaded
  its own system files, booted to the LeapPad2 home screen, no computer and no
  root. Slower than native and the camera does not work yet — see the known
  list below.

  A 64-bit phone needs the engine even when its CPU still supports 32-bit, and
  the reason is not the CPU: Android refuses to execute a file the app itself
  wrote (the W^X rule from API 29), and the guest's binaries come out of your
  own firmware. The engine sidesteps that by never exec'ing them — it reads
  them and JITs.

There are two downloads for that reason. The **v7a** build is about 6 MB and is
the tested one; the **universal** build is 51 MB, almost all of it that engine,
and is the one to try on a 64-bit phone.

If the v7a APK refuses to install with **`INSTALL_FAILED_NO_MATCHING_ABIS`**,
your device has no 32-bit support at all. That is not a packaging mistake — it
is the answer. Recent flagships (and every ARMv9 core Samsung and Qualcomm ship
now) removed 32-bit execution from the hardware, so the guest's own binaries
cannot run there by any means. Use the universal APK if you want to help test
the emulator path, and expect it to be rough.

Check with `adb shell getprop ro.product.cpu.abilist`, or any CPU-info app.

Tested on: an AOSP 8.1 tablet, armeabi-v7a only, Mali-T720.

---

## 1. Install

```sh
adb install -r Tadpole-android-0.5-beta.apk
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

## Reporting a problem

### The easy way: Help -> Save Diagnostic Log

No computer, no `adb`, no cable.

Open **Help -> Save Diagnostic Log**. It writes one file into your **Downloads**
folder, named like `tadpole-log-20260819-191845.txt`, and tells you the name on
screen. Attach that to your report — any file manager, chat app or mail client
can find it in Downloads.

It contains everything anyone will ask you for anyway:

- your device, its Android version and its ABI list;
- **whether the app is running the 32-bit native path or the 64-bit engine**,
  which is the first thing that decides how a bug is read;
- whether the engine linked, whether firmware is installed, how many packages
  and whether Didj support is there;
- Tadpole's own log, and the guest's crash report if it left one.

It is Tadpole's output only. Android has filtered the log by app since 4.1, so
it cannot pick up anything belonging to you or to another app.

### The `adb` way, if you already have it set up

Everything below does the same job from a computer, and is what to use if the
app will not start far enough to reach its own menu.

### If it does not start at all, this one command answers it

```sh
adb logcat -c            # clear
# now launch Tadpole on the device, and wait for the menu bar
adb logcat -d | grep "probe:"
```

Tadpole probes the device on every launch and prints what it found. The line
that decides whether this device can work at all is:

```
probe: /system/bin/linker    PRESENT - a 32-bit userspace exists, ARM32 guest binaries CAN be loaded
probe: /system/bin/linker    ABSENT  - 64-bit-only ROM, nothing 32-bit can execute here at all
```

**ABSENT means the device cannot run the guest NATIVELY**, and nothing in
Tadpole can change that: the LeapPad2's binaries are 32-bit ARM, and recent
flagship cores dropped 32-bit execution outright.

That is not the end of the road, though — it is the case the **emulator** is
for, and it is what the universal APK carries.

### On a 64-bit device: check the engine linked

The engine ships inside the APK as `lib/arm64-v8a/libglasspole.so`, and the app
symlinks it into place on every launch (a symlink and not a copy, because a
file the app wrote may not be executed, while one the package installer placed
may). That either worked or it did not, and it says so:

```sh
adb logcat -d | grep -i "engine:"
```

```
engine: /data/.../glasspole/build/glasspole -> /data/app/.../lib/arm64/libglasspole.so   good
engine: no libglasspole.so in this APK      you installed the v7a build on a 64-bit device
engine: could not link                      the interesting failure - send this one
```

If the link is there and titles still do not run, say so explicitly and include
the whole log. The engine path is known to work — so "the engine linked and
then X happened" is a real bug and worth reporting in full.

Paste the whole `probe:` block. It also reports whether the app can make
executable memory, exec a file, `mkfifo`, and map shared memory — which is the
rest of the answer when the first line says PRESENT.

### A full log, for anything else

```sh
adb logcat -c                       # clear first, or you get hours of noise
# reproduce the problem on the device
adb logcat -d > tadpole-log.txt
```

Send the whole file. `-d` dumps and exits, so this is a clean capture of just
your reproduction rather than a stream you have to interrupt.

If you only want Tadpole's own lines — smaller, but it drops crashes, which
are logged by Android under other tags:

```sh
adb logcat -s tadpole
```

That is the whole of Tadpole's diagnostic output, the same lines the desktop
build writes to a terminal, guest included.

### If it crashed rather than misbehaved

Do not filter. The crash is logged by Android, not by Tadpole:

```sh
adb logcat -d | grep -B 5 -A 40 -E "signal|tombstone|FATAL"
```

The guest's own crashes are separate and are written on the device, so grab
that too — it names the guest library and offset:

```sh
adb shell "run-as org.tadpole.view cat files/crash.log" | tail -40
```

### What to say alongside it

1. **What device**, plus:
   ```sh
   adb shell getprop ro.product.cpu.abilist
   adb shell getprop ro.build.version.release
   ```
2. **Which APK** — the v7a one or the universal one.
3. **What you were doing**, and what happened. A screenshot beats a
   description; `adb exec-out screencap -p > shot.png` takes one.

### Known, and not worth reporting yet

- micromods, cart2tar and make-profile say "not available on Android yet",
  because they are.
- The progress bar fills and then restarts for the next phase of an install.
- After an unclean exit a stale `.lock` can leave *Run System Menu* greyed out
  as "running" — restart the app.
- **The camera does not work on 64-bit devices.** It works on 32-bit ones. The
  viewfinder comes up black or green while the phone's own camera indicator
  says the camera is live — the frames are captured and never reach the guest.
  Known, being worked on; no need to report it again.
- Under the engine, the guest runs noticeably slower than on a 32-bit device,
  where there is no emulation at all.
