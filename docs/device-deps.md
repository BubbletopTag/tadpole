# M2 — Device Dependency Manifest (LeapPad2 / VALENCIA)

Captured 2026-07-27 by running the stock `AppManager` under `qemu-arm -strace`
against the extracted stock rootfs (firmware 4.6.0.784). This is the checklist
the shim has to satisfy.

Reproduce with `runtime/run.sh`; add `-strace` to the qemu invocation for the
raw syscall log (~528k lines).

## Status

| Milestone | State |
|---|---|
| M1 — ARM binaries execute under qemu-user | **DONE** |
| M2 — dependency manifest | **DONE** (this file) |
| Brio stack loads (33 NEEDED libs, 0 errors) | **DONE** |
| `AppManager` reaches `main()`, modules init | **DONE** |
| Screen geometry via faked sysfs | **DONE** (480x272) |
| M3 — framebuffer + input shim | next |

## What already works

`AppManager` loads the complete Brio stack and enters its main function:

```
+++ AppManager: Entered main function
+++ AppManager: Bgn Init Logging
+++ AppManager: Bgn InitMutex
[0x280]    AppManager ReLaunch Threshold = 5
[0x6] Start ButtonPowerUSB Task ... !
[0x6] Start Cartridge task ...!
[0x5] InitModule: Screen = 480 x 272
```

All 33 `NEEDED` libraries resolve:

```
libVideoMPI libCameraMPI libMicrophoneMPI libAudioMPI libButtonMPI
libCartridgeMPI libDebugMPI libDisplayMPI libEventMPI libFontMPI
libKernelMPI libModuleMPI libUSBDeviceMPI libPowerMPI libUtility
libLightningJSON libLightningBase libLightning2D libustring
libpng12 libz libjpeg libEGL libGLESv1_CM libvr5
libdl libpthread libintl libiconv libstdc++ libm libgcc_s libc
```

It then segfaults, because there is no framebuffer.

## 1. Framebuffer — THREE devices

`libDisplay.so` (the Brio Display *Module*) references:

```
/dev/fb0
/dev/fb1
/dev/fb2
```

Confirms the multi-layer hardware is exposed as three separate framebuffer
devices, matching PLAN.txt §3. Current failure:

```
[0x5] CreateHandle: No framebuffer allocation available
[0x5] InitModule: Mapped 00000000 to 0xffffffff, size 00000000
[0x5] InitModule: Screen = 0 x 0, pitch = 0
```

Needs: `FBIOGET_VSCREENINFO`, `FBIOGET_FSCREENINFO`, `mmap`, and probably
`FBIOPUT_VSCREENINFO` / `FBIOPAN_DISPLAY` / `FBIO_WAITFORVSYNC`, plus the four
`LF1000FB_*` ioctls and the `nonstd` bit-packing.

Format strings recovered from `libDisplay.so`, useful for matching behaviour:

```
%s: Screen = %u x %u
%s: Screen = %d x %d, pitch = %d
%s: Mapped %08lx to %p, size %08x
%s: %p: %dx%d (%d) @ %p
%s: No framebuffer allocation available
```

## 2. Input — five devices, matched BY NAME

`AppManager` scans `/dev/input/event0` .. `/dev/input/event24` (opening each
8x) and identifies devices by name. It reports:

```
CEventModule::ButtonPowerUSBTask: cannot open: gpio-keys
CEventModule::ButtonPowerUSBTask: cannot open: Power Button
CEventModule::ButtonPowerUSBTask: cannot open: USB
CEventModule::ButtonPowerUSBTask: cannot open: Accelerometer
CEventModule::ButtonPowerUSBTask: cannot open: touchscreen interface
```

So the shim must present five evdev nodes whose `EVIOCGNAME` returns exactly:

| Name | Purpose |
|---|---|
| `gpio-keys` | buttons + D-pad |
| `Power Button` | power/battery event channel (see PLAN.txt §4) |
| `USB` | USB connect/disconnect |
| `Accelerometer` | tilt |
| `touchscreen interface` | stylus/touch |

**`Power Button` was predicted in PLAN.txt §4 from the LeapPad1 driver source
and is now confirmed empirically on LeapPad2.** This was the exact thing that
blocked the original 2010s attempt.

tslib also probes independently:

```
ButtonPowerUSBTask: tslib: Can't find touchscreen event device in /dev/input
ButtonPowerUSBTask: tslib: Falling back on touchscreen interface
```

## 3. sysfs — required, and now faked successfully

Read by `AppManager`:

```
/sys/devices/system/board/platform          -> "VALENCIA"
/sys/devices/system/board/platform_family   -> [VERIFY on device]
/sys/devices/system/board/system_rev
/sys/devices/system/board/lcd_size          -> "480x272"   format %ux%u
/sys/devices/platform/lf1000-gpio/board_id  (note: lf1000-, not lf2000-)
/sys/devices/platform/lf2000-power/status
```

Fallback path also present in `libDisplay.so`:

```
/sys/devices/platform/lf1000-dpc/xres
/sys/devices/platform/lf1000-dpc/yres
```

The full board attribute set, per `/LF/Base/MfgTest/MfgTest_ReleaseNotes.txt`
(which contains a real shell session dumping them on a "RIO" board):

```
lcd_mfg  lcd_mfg_get  lcd_size  lcd_type  platform  platform_family  system_rev
```

Setting `lcd_size` to `480x272` in the faked sysroot changed the log from
`Screen = 0 x 0` to `Screen = 480 x 272`. Confirmed working.

Also referenced elsewhere in the system:

```
/sys/devices/platform/lf2000-aclmtr/calibration     (rcS)
/sys/class/graphics/fb0/rotate                      (rcS)
/sys/devices/platform/lf2000-power/adc_constant     (rcS)
/sys/devices/platform/lf2000-power/adc_slope_256    (rcS)
```

## 4. procfs

```
/proc/uptime
```

## 5. ioctls seen so far

Only `TCGETS` (terminal probing). Nothing else, because execution stops before
the framebuffer is opened. Expect the fb ioctls to appear once M3 lands.

---

## Gotchas discovered while getting here

**1. Executable bits.** `ubireader_extract_files` without `-k` drops
permissions, so nothing is executable and you get `Exec format error` — even
for the dynamic loader. Fix: `chmod +x` every ELF and shell script (518 files
in this rootfs). Do NOT use `-k` instead; it creates read-only directories and
then fails to write into them.

**2. Host library fall-through — the subtle one.** Guest libraries are split
across `/lib`, `/usr/lib`, `/LF/Base/Brio/lib` and `/LF/Base/lib`. When a lib
is missing from the first directory searched, qemu-user falls back to the
**host** path, hands the guest an x86-64 `.so`, and the guest loader dies with
`is not an ELF executable for ARM`. No single search order works, because
`libstdc++`/`libz`/`libEGL` are in `/usr/lib` while `libpthread`/`libgcc_s`
are in `/lib`. Fix: one flat directory of symlinks to every ARM `.so`, first in
`LD_LIBRARY_PATH`. Drops the error count from ~50 to 0.

**3. `qemu-arm -L` translates paths only for the process it launches.** It is
not a chroot. Verified: `busybox ls /LF/Base/bin` and
`busybox cat /sys/devices/system/board/lcd_size` both resolve inside the
sysroot, and `uname -m` reports `armv7l`.

But **any fork/exec resolves against the HOST**. `busybox sh -c 'cat /sys/...'`
silently runs the *host's* `/bin/cat` on the *host's* filesystem. This will
break shell scripts — including `/usr/bin/app` and `/etc/init.d/rcS`.

Fix when we need multi-process: register `binfmt_misc` for ARM (needs root),
so guest execs re-enter qemu transparently. Single-process work like
`AppManager` is unaffected.

**4. `/etc/ld.so.cache` is corrupt.** Harmless. The rootfs ships only
`ld.so.cache-didj`, so qemu falls through to the host's x86-64 cache and uClibc
rejects it, then falls back to path search. Filter the warning out or drop an
empty cache into the sysroot.

## Next: M3/M4

1. `LD_PRELOAD` shim intercepting `ioctl`/`mmap` (cross-compile
   `-march=armv7-a -mfpu=vfp -mfloat-abi=softfp`).
2. Implement `/dev/fb0..2` — fb ioctls + a shared mmap-backed buffer.
3. Five uinput-style evdev nodes answering `EVIOCGNAME` with the names above.
4. Native SDL2 viewer mmapping the same buffer.

Emit "on external power" once at startup on the `Power Button` device. Never
emit `KEY_POWER` unless a shutdown is wanted.

---

# Session 2 — getting AppManager to run

## The injection problem: LD_PRELOAD is unavailable

LeapFrog's uClibc 0.9.32 was built **without** `__LDSO_PRELOAD_ENV_SUPPORT__`.
The guest loader never opens an `LD_PRELOAD` path at all — verified with a
known-good ARM library, so it is not a fault in our object. (`LD_PRELOAD` does
appear in `ld-uClibc`, but only in the list of variables it scrubs for setuid
binaries.) `LD_LIBRARY_PATH` *is* honoured.

**Tadpole's solution — impersonate a library the target already links:**

1. copy the real library aside, patching its SONAME in place to a
   **same-length** name so no offsets shift
   (`libdl.so.0` -> `libdl.so.9`, `libz.so.1` -> `libz.so.9`)
2. build the shim with SONAME set to the **original** name and `DT_NEEDED` on
   the renamed real one
3. put it first in `LD_LIBRARY_PATH`

Our `open`/`ioctl` win symbol lookup; everything else resolves straight through
to the real library, so `dlopen`/`dlsym` keep working and
`dlsym(RTLD_NEXT, ...)` behaves normally.

Two variants are needed because targets link different things:

| variant | covers | why |
|---|---|---|
| `libdl.so.0` | AppManager, VideoDaemon | libdl is #22 in DT_NEEDED, libc #33 |
| `libz.so.1`  | imager-fb, drawtext-fb, flipbook-fb, fbtest | these link no libdl at all |

## Three crashes, diagnosed by gdb backtrace

`qemu-arm -g <port>` plus the host gdb (`set architecture arm`) gives real
backtraces through the stripped binaries via the library symbol tables.

**1. `CMfgData::Read` — segfault inside libc**

```
#4  BaseUtils::HasDoomPackageCritical()
#3  CSystemData::GetLocaleCode()
#2  CMfgData::GetLocale(char*)
#1  CMfgData::Read(unsigned char*, int, int)
#0  <libuClibc>
```

`libMfgData.so` parses `/proc/mtd` for a partition named `MfgData0`, then
opens `/dev/mtd<N>`:

```
GetNorPartitionFilename: Could not find %s in /proc/mtd
CMfgData::Init: GetNorPartitionFilename failed
```

We provided neither, so `Init` failed and `Read` ran on an uninitialised
object. **Fix:** synthesise `/proc/mtd` from the documented partition table
plus sparse `/dev/mtd0..2`. It then degrades gracefully:
`GetLocaleCode unable to read from mfg_data, using en-us`.

**2. `BaseUtils::CreateFile` — infinite recursion**

```
#8..#11  BaseUtils::CreateFile(Glib::ustring const&)   <- same address
```

strace shows the loop, 174,662 times until the stack blows:

```
mkdir("/LF")                                  -> EEXIST
open("/LF/Bulk/Data/Uploads/0/profile.log")   -> ENOENT
stat64("/LF/Bulk/Data/Uploads/0")             -> ENOENT
```

It recurses to create missing parents but only ever retries `mkdir("/LF")`,
never building the intermediate components. On hardware the directories
already exist so the bug never fires. **Fix:** `mkdir -p
/LF/Bulk/Data/Uploads/0`.

**3. The "doom" message is a red herring**

`HasDoomPackageCritical: global doom file detected` is logged, but AppManager
never stats `/LF/doom` or `CriticalDoom.json` — zero filesystem calls for
either. It is a *symptom* of the locale lookup failing, not a cause. And on
real hardware doom produces a warning **screen**, not a crash.

For reference, `LF/Base/LpadAssets_en/Data/CriticalDoom.json` lists ten
folders that must exist on the Bulk partition:

```
/LF/Bulk/LanguagePack_en          /LF/Bulk/LanguagePack_en/Audio
/LF/Bulk/LanguagePack             /LF/Bulk/LanguagePack/Audio
/LF/Bulk/ProgramFiles/KeyboardWidget    /LF/Bulk/ProgramFiles/CameraWidget
/LF/Bulk/ProgramFiles/PhotoEditor       /LF/Bulk/ProgramFiles/SneakPeekWidget
/LF/Bulk/Downloads/PAD2-0x00210008-200000
/LF/Bulk/Downloads/PADS-0x1F1E0002-300000
```

We have the two LanguagePacks from the LFConnect cache. The four ProgramFiles
widgets and the two Downloads live on the Bulk partition and are still
missing — see "What we still need" below.

## Where it stops now

`SetupSystem` completes. Remaining:

```
[0x280] AppManager signal handler installed
[0x0] DaemonControl socket connect failed ret=-1
```

AppManager wants `/tmp/video_events_socket` from **VideoDaemon** (`rcS` runs
`VideoDaemon 750 &`). VideoDaemon starts and detaches but never calls
`socket()`/`bind()` — next thing to chase.

Note also that `qemu-arm -L` is **not a chroot**: it translates paths for the
process it launches, but any fork/exec resolves against the host. Running
AppManager and VideoDaemon as two separate `qemu-arm` invocations works
because they rendezvous through files in the shared sysroot `/tmp`.

## Reproducing

```
runtime/setup-sysroot.sh     # build the faked sysroot (idempotent)
tadpole/  make               # shim (ARM) + viewer (host)
runtime/run.sh               # AppManager
runtime/run.sh /bin/busybox sh
```

## What we still need from real hardware

The firmware image only contains RFS. The **Bulk partition** (`/dev/mtd6`,
9721 files per `erootfs.md5`) is not in it, and that is where the ProgramFiles
widgets, the OpenGL ES driver package `MULT-0x0022000A-PP2000`
(libEGL/libGLES/libvr5), and installed content live. Dumping Bulk over FTP
from a working device is the single most useful thing left to collect.
