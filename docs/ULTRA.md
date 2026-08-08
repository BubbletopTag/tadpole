# LeapPad Ultra — porting notes

Working notes for the `ultra` branch. Same spirit as `docs/HANDOVER.md`: what
was tried, what it did, and the measurement behind it.

Firmware under test: **5.2.7.1002**, built 2013-12-16, `Device="LeapPadUltra"`.

---

## The premise, corrected

The Ultra was expected to be "a LeapPad2 with a bigger screen", on the theory
that it shares the NXP3200. It does not, and it does not matter much.

**It is not the same SoC.** The LeapPad2 is Nexell NXP3200 (LeapFrog's LF2000
platform, board VALENCIA). The Ultra is the LF3000 platform, board **RIO** —
publicly reported as a Nexell NXP4330. Its own `rcS` proves the board name: it
branches on `platform_family = RIO` and `platform == RIO`, alongside the
MADRID / LUCY / VALENCIA cases the LeapPad2 firmware also carries.

**It does not matter much, because Tadpole does not emulate the SoC.** Tadpole
runs guest *userspace* under `qemu-arm` and fakes the kernel interfaces around
it. What matters is the ABI and the interfaces, and those are the same:

| | LeapPad2 | LeapPad Ultra |
|---|---|---|
| ELF class / machine | ELF32 ARM | **same** |
| `Tag_CPU_arch` | v7 | **same** |
| `Tag_FP_arch` | VFPv2 | **same** |
| float ABI | softfp | **same** |
| uClibc | 0.9.32 | 0.9.33.1 |
| GL stack | `libvr5.so`, `libEGL`, `libGLESv1_CM` | **same names** |
| kernel platform devices | `lf2000-power`, `lf2000-aclmtr`, `lf2000-touchscreen`, `lf1000-gpio`, `lf1000-dpc` | **same** |
| package format | `.lfp` ZIP / `.lf2` bzip2 tar, `Firmware-Base` with `kernel.bin` + `*-erootfs.ubi` | **same** |
| framebuffer | `/dev/fb0..2` | **same** |
| screen | 480x272 | **1024x600** |

So `tadpole/Makefile`'s `ARMFLAGS` are correct unchanged, and the shim, the
viewer, the framebuffer protocol and the evdev fakes all apply as they are.

**What is genuinely different is the shell.** The LeapPad2 boots `AppManager`
— Brio plus Flash Lite. The Ultra boots **`AppServer`, a Qt 4.8.4 Embedded
(QWS) application**, and keeps `AppManager.legacy` beside it only to run
cartridge titles. Its home screen, sign-in, parent mode and setup are QML
modules under `/LF/Base/Qt/Modules/`:

```
AppServer  BrioServer  BrioWrapper  FirmwareUpdate
InitialSetup  MainPicker  ParentPicker  SignIn
```

`/usr/bin/app` launches it as `nice -n $niceLevel AppServer $appPath $2 -qws`.
`-qws` makes the process the Qt Window System **server**, which is what puts Qt
directly in charge of `/dev/fb0` — so the existing framebuffer shim is the
right substrate for it, no new display path required.

---

## How the screen size was established

1024x600, from the firmware, twice over:

* `/var/screens/rio-sparkle-pngs/Particle_*.png` are full-screen boot frames
  and measure 1024x600.
* the Qt shell's QML declares `width: 1024` / `height: 600`.

`rcS` symlinks `/var/screens/rio_$LCD_SIZE.png`, which would say it outright,
but no `rio_*.png` is in the root filesystem — it lives on Bulk.

---

## Where the firmware comes from

All three packages are on the public CDN, under `PAD3FW` (which is
OpenLFConnect's `LF_URL` for this device — see
`OpenLFConnect/extras/device_profiles/leappad_ultra.cfg`):

```
packages/PAD3FW/PHR1-0x00280005-000000.lfp   114,786,635   Firmware Base
packages/PAD3FW/PHR1-0x00270003-000000.lfp     5,935,943   Surgeon
packages/PAD3FW/PHR1-0x00280005-000002.lfp        34,385   Firmware Bulk Empty
```

`tools/fetch-firmware.py --device leappadultra` reads all of that out of the
device profile. Of the 84 packages in `EnglishLeapPadUltra.xml`, **82 are
downloadable** — 482 MB, including Pet Pad Party, Pet Chat, Art Studio Ultra,
Photo Fun Ultra and the widgets. The two that are not are the "In-Home Only"
LeapFrog Learning Songs pair.

---

## Device profiles

Everything that used to be a LeapPad2 constant now lives in
`runtime/devices/<id>.conf`, read by `runtime/device.sh`:

```
runtime/devices/leappad2.conf        VALENCIA, 480x272,  AppManager
runtime/devices/leappadultra.conf    RIO,      1024x600, AppServer -qws
```

**The device is detected, not configured.** `Firmware/meta.inf` in every
installed rootfs carries `Device="LeapPad2Explorer"` or `Device="LeapPadUltra"`,
and `tad_detect_device` matches that against each profile's
`DEV_META_DEVICE`. The wizard's picker and `ui.cfg`'s `device` line are
overrides, and the setting is absent by default on purpose: a stale one would
boot a LeapPad2 rootfs at the Ultra's geometry and produce a working emulator
showing a corrupt screen.

---

## Four things that had to be fixed to get Qt running

### 1. `libEGL.so` is the only injection vector the Qt shell offers

The shim cannot use `LD_PRELOAD` (uClibc is built without it), so it
impersonates a library the target already links. The two existing variants are
`libdl.so.0` and `libz.so.1`. **The Ultra's Qt shell links neither.** The one
library every part of it does link is `libEGL.so`:

```
                     libdl   libz   libEGL
AppServer            no      no     YES
MainPicker           no      no     YES
SignIn               no      no     YES
ParentPicker         no      no     YES
InitialSetup         no      no     YES
BrioServer           no      no     YES
FirmwareUpdate       no      no     YES
AppManager.legacy    YES     YES    YES
VideoDaemon          no      no     no      <- still unsolved
```

Hence a fourth variant, `runtime/shimlibs-egl/libEGL.so` (`make shimegl`).

**The transitive route does not work, and the reason is worth keeping.** The
stock `libEGL.so.1.4` lists `libdl.so.0` in its own `NEEDED`, and
`setup-sysroot.sh` points that name at our shim — so it looks as though the
shim arrives by itself. It does not: ELF symbol resolution is breadth-first, so
every *direct* dependency of the executable is searched before any second-level
one, and `libc.so.0` is a direct dependency that defines `open()`. A shim
pulled in one level down is found after it and never wins. Impersonation has to
happen at the first level or not at all.

**It must chain, not replace.** The stock `libEGL` is not only EGL — it also
*defines* five globals that `libvr5.so` imports, and replacing it outright kills
every guest that loads the GL stack at load time:

```
symbol 'g_IOControlCallSend': can't resolve symbol
g_IOControlCallWait  g_IOControlCallWaitDummy
gUseTextureOriginalSize  gForceLinear
```

So the stock library is copied to `libEGLreal.so` and kept in the process
underneath ours — the same trick as `libGLESv1_CM` on the LeapPad2. Its own
`libdl.so.0` string is patched to `libdl.so.9` in the copy (same length, so it
patches in place), which stops a *second* copy of the framebuffer shim being
loaded underneath the first with its own static state.

### 2. An absolute symlink inside the rootfs escapes to the host

```
VideoDaemon: '/usr/lib/libustring.so' is not an ELF executable for ARM
```

The Ultra ships `usr/lib/libustring.so` as a symlink to the **absolute** path
`/usr/lib/libglibmm-2.4.so.1`. Correct on the device. But `setup-sysroot.sh`
built `runtime/libs/libustring.so` as a symlink *to that symlink*, and the host
resolves the chain against the host's own `/usr/lib` — handing the guest an
x86-64 object, with an error that sounds like a broken toolchain.

Fixed with `rootfs_target()`: any absolute symlink target is re-rooted under
`$ROOTFS`. Relative links need no help. The LeapPad2 has no such link, which is
why this never came up before.

### 3. Qt's QWS socket directory has to exist

```
QWSServerSocket: could not bind to file /tmp/qtembedded-0/QtEmbedded-0
FATAL........: Failed to bind to /tmp/qtembedded-0/QtEmbedded-0
```

`/tmp` is tmpfs on the device and Qt creates this itself; here `/tmp` is a real
directory that `tadpole.sh` wipes each boot, so the parent is created in both
`setup-sysroot.sh` and `tadpole.sh`, mode 0700 (Qt checks).

### 4. The Qt environment comes from `/etc/profile`, which nothing sources

Tadpole execs the shell binary directly, so Qt came up with **no `QWS_DISPLAY`
at all**, never opened `/dev/fb0`, and segfaulted before reaching its screen
driver. The device sets it in `/etc/profile` — with the comment "do it here
(not rcS) so that user's command line gets it too":

```
QWS_DISPLAY=Transformed:Rot0
QWS_DATA_HOME=/LF/Bulk/Data/Local/All/qws/share
QWS_CACHE_HOME=/LF/Bulk/Data/Local/All/qws/cache/
TMPDIR=/LF/Bulk/Data/Local/All/qws
QWS_MOUSE_PROTO=TsLib:$(list-input-devices | fgrep "touchscreen interface")
```

`Transformed:Rot0` selects `libqgfxtransformed.so`, the only gfxdriver plugin
in the image, wrapping LinuxFb with zero rotation. These now live in the
profile as `DEV_ENV` and are passed as `qemu -E`.

`QWS_MOUSE_PROTO` is deliberately omitted: it resolves to `/dev/input/event2`
here, but `tadpole.sh` disables tslib by default because its module chain
crashes on the first touch. Ultra touch input is a separate job.

---

## Bulk is populated, Pet Pad Party included

The firmware image contains only RFS. `/LF/Bulk` — the widgets, the apps, the
language packs — comes from the content packages, and all of it downloads:

```
./tools/fetch-firmware.py --device leappadultra --get all -o <dir>
./tools/lf3.py <dir> -o <dir>            # the encrypted ones
./tools/install-game.sh <dir>/*.tar
./tools/install-content.sh runtime/sysroot/LF/Bulk <dir>
```

**Ten of the Ultra's own apps ship as `.lf3`** — the encrypted digital-purchase
format — and `tools/lf3.py` decrypts them with the existing key, unchanged:

```
Pet Pad Party      Pet Chat        Art Studio Ultra    Photo Fun Ultra
Voice Memo         Calculator      Notepad             Clock
Calendar           Songs from Us!
```

That was not a given: the key is a client-side constant from LFConnect and
could have been per-device. It is not.

Result: 30 packages in `LF/Bulk/ProgramFiles`, 35 downloads, both language
packs. The Ultra's Qt-era widget set is all there — `RioConnmanApp`,
`ProfileManager`, `LeapSearch`, `BookPicker`, `XMVRio`, `ContentManager`.

**Run `install-content.sh` twice.** DeviceAssets install *into* their parent
package's directory, and the parents here are the `.lf3` ones, so on a first
pass all nine report "no parent" and are skipped. Once `lf3.py` and
`install-game.sh` have put the parents in place, a second pass attaches every
one. Without the icon the home picker filters the title out entirely.

`install-content.sh` needed no changes for the Ultra — it dispatches on
`Type=` from `meta.inf`, and those types are the same.

---

## Two live paths, not one

The legacy Brio path is alive too. `AppManager.legacy` reaches `main()` — the
same milestone the LeapPad2 port called M2:

```
+++ AppManager: Entered main function: 966156.96 64522969.82
Unable to open module '/LF/Base/Brio/Module/libModule.so', Unable to resolve symbol
BOOTFAIL: Failed to load found module at sopath: /LF/Base/Brio/Module/libModule.so
```

So there are two independent routes to a working Ultra, and the Brio one has
the shorter unknown list — the shim already reaches it through `libdl`, and an
unresolved symbol in a Brio module is a much more tractable problem than a
guard-page fault inside Qt.

---

## It boots

`shots/ultra-home.png` — the LeapPad Ultra home screen at 1024x600, green
theme, dock, profile name, battery. Zero crashes, zero "Couldn't load asset".

```sh
./tadpole.sh --boot
```

Four more things had to be fixed after the Qt stack came up. Each one is a
class of bug worth naming, because none announced itself.

### 5. TWO SHIMS IN ONE PROCESS — the one that cost a gdb session

`libdl.so.0`, `libz.so.1` and `libEGL.so` are the SAME SHIM under three names,
and all three were on `LD_LIBRARY_PATH` at once. Harmless while a guest links
exactly one of them; the LeapPad2 always did. The Ultra's Qt shell links
libEGL **and**, through libpng, libz — so it loaded two, whose `open()` chained
into each other and recursed until the 64 MB guest stack was gone:

```
Program received signal SIGSEGV
#0  0x45ea0320 in ?? () from runtime/shimlibs-z/libz.so.1
#1  0x45ea034c in ?? () from runtime/shimlibs-z/libz.so.1
Backtrace stopped: previous frame identical to this frame (corrupt stack?)
sp  0x40001008          <- the bottom of the stack
```

It is invisible to every cheap tool: nothing repeats in an `strace`, because
the recursion never reaches a syscall, and no crash report is printed, because
the handler needs stack that has gone. Only gdb through `qemu-arm -g` says
where it is.

Fixed by giving a Qt device **exactly one** variant on its path —
`shimlibs-egl` is now self-contained, carrying its own `libdl.so.9` and GLES —
and by leaving `/lib/libdl.so.0` as the device's own, since that route is only
needed by AppManager. The shim also detects the condition at init now and says
so in one line instead of dying silently.

*A first attempt at a guard only caught a shim finding ITSELF via RTLD_NEXT.
That is a different bug and it did not fire. The check had to ask who owns
`open` globally.*

### 6. `execve` is the one path `-L` does not translate

`-L` is not a chroot. It rewrites paths for the process qemu launched, so
`stat()` on a guest path succeeds — and then the exec is handed to the host
kernel unchanged:

```
stat64(".../runtime/sysroot/LF/.../MainPicker")     = 0
execve("/LF/Base/Qt/Modules/MainPicker/MainPicker") = -1 ENOENT
```

AppServer launches every screen as a child, so the home screen died instantly
and AppServer relaunched it forever — reported as `MainPicker crashed,
exit=0, error=0`, which reads like the application failing rather than the
exec never happening. `docs/device-deps.md` predicted this and proposed
`binfmt_misc`, which needs root. The shim does it instead: rewrite to
`qemu -L <sysroot> <sysroot><path>`, which is what binfmt_misc would have
arranged anyway.

**Overriding `execve` alone does nothing.** uClibc's `execv` and `execvp` reach
`execve` by an internal binding that never goes through the PLT, and Qt's
QProcess uses the wrappers — so the hook was never entered while an `execve`
syscall still went out. All three entry points have to be interposed.

**And do not ask `access()` whether the host can run it.** `access` IS
translated, so it answers "yes, this already works" about precisely the guest
paths `execve` cannot run, and passes every one of them straight through.

### 7. Downloads self-wrap too

`install-content.sh` used `extract_as` (wrapper-aware) for Applications but
plain `extract_to` for Downloads. The LeapPad2's downloads happen to be flat.
The Ultra's whole UI theme arrives as MicroDownloads that self-wrap, so it
landed one level too deep —

```
Downloads/PHR1-0x00270008-200002/PHR1-0x00270008-200002/art/global/...
```

— AssetManager found nothing, every home-screen icon logged "Couldn't load
asset", and `libQtSystemStatus` segfaulted on the first null image.

### 8. The profile format is richer than the LeapPad2's

The Ultra keeps a **third** file, `profile_rio.dsc`, beside `profile.dsc` and
`profile_private.dsc`, and `profile.dsc` itself gains `NickName`, `Badges`,
`Tokens`, `BirthMonth`, `BirthYear`, `Gender`. The field that matters is

```
Theme=/LF/Bulk/Downloads/PHR1-0x00270008-200001/
```

— the installed theme package. `RIO_THEME_GREEN` is `-200001`, `RIO_THEME_PINK`
is `-200002`. With `Theme=/` the shell has no dock, no battery and no icons.

`tools/make-profile.sh` now takes the shell package ID from the device profile
instead of hardcoding the LeapPad2's, but **it still writes the LeapPad2 shape**
— the Ultra's extra files were written by hand for this boot. Teaching it the
RIO format is the obvious next job.

### And note what MainPicker alone cannot do

Run standalone with `-qws` it starts, sizes itself to 1024x600 and builds its
QML — but it only has its own `_LOCAL` art. The theme comes from AppServer, so
a standalone MainPicker has no dock and no battery whatever the profile says.
The home screen needs the pair.

---

## Still to do

* **input** — nothing is wired up. `QWS_MOUSE_PROTO` resolves to
  `TsLib:/dev/input/event2` on the device, but `tadpole.sh` disables tslib by
  default because its module chain crashes on the first touch. Until this
  lands the home screen renders but does not respond.
* **`make-profile.sh` does not write the RIO profile shape** (see 8 above)
* **VideoDaemon has no injection vector** — it links neither libdl, libz nor
  libEGL on this firmware, so nothing intercepts it yet
* audio: `libasound.so.2` lives in `shimlibs`, which is off the path for a Qt
  device now, so the Ultra has no audio shim
* the GL path is untested — the home screen is QWS raster, not GL, so nothing
  has exercised `libEGL`'s actual EGL yet
* touch input (`QWS_MOUSE_PROTO` / tslib)
* the Ultra has WiFi, and nothing shims it: `libWirelessMPI.so` is the one Brio
  library it has that the LeapPad2 does not, and the Qt shell links
  `libQtRioConnman.so.1`, so the network stack is **ConnMan over D-Bus**
* `DEV_SYSTEM_REV`, `DEV_LCD_TYPE`, `DEV_LCD_MFG`, `DEV_UIPKG`, `DEV_CODEC` and
  the MTD table are marked UNVERIFIED in the profile — the LeapPad2's came off
  a live device, these were inferred
* Leapster GS: same NXP3200 platform as the LeapPad2, 320x240, CDN dir
  `GAMFW`, firmware `GAM2-0x00210004-000000.lfp` (34 MB, confirmed present).
  Much less work than the Ultra — no Qt — and the community wants it.

---

## An incidental finding, on `main` rather than here

`imager-fb` — which `tadpole.sh` uses to draw the boot splash and create the
framebuffer — **is broken on the LeapPad2 too**, and has been:

```
imager-fb: can't resolve symbol '__aeabi_uidiv'
```

The `libz` shim variant references `__aeabi_uidiv`, and on both devices nothing
in the process defines it: no `.so` in either rootfs exports it, `libgcc_s.so.1`
included. `tadpole.sh` calls it with `>/dev/null 2>&1 || true`, so the failure
is invisible and the framebuffer simply gets created later by whatever boots
next. Verified identically on `main` with the LeapPad2 rootfs, so it is not
something this branch introduced. Worth a look on `main`: the splash never
draws.
