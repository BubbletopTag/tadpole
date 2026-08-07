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

## Where it stops now

`AppServer` gets a long way. It loads the whole Qt stack, binds the QWS server
socket, builds its font database into `/tmp/qtembedded-0/fonts/fontdb`, finds
`libqgfxtransformed.so`, and the shim creates `fb0.bin` at **19,660,800 bytes
= 1024 x 600 x 4 x 8 buffers**, which is the correct Ultra geometry.

Then it dies:

```
--- SIGSEGV {si_signo=SIGSEGV, si_code=2, si_addr=0x40000ff8} ---
```

`si_code=2` is `SEGV_ACCERR` — a permissions fault, not an unmapped address —
at `0x40000ff8`, immediately below a page boundary. That reads like a guard
page at the bottom of a **thread** stack rather than the main one: `tadpole.sh`
passes `qemu -s 67108864` for the main stack, and that only sizes the main
stack. Qt starts threads. This is the next thing to chase.

`/dev/fb0` is never opened, so it is dying at or just before QWS screen
connect.

---

## Still to do

* the `SIGSEGV` above — likely thread stack size
* **VideoDaemon has no injection vector** — it links neither libdl, libz nor
  libEGL on this firmware, so nothing intercepts it yet
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
