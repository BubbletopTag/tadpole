# Tadpole — complete handover

_LeapPad2 Explorer emulator. Written 2026-07-27, one working session._

This file is self-contained: everything needed to pick the project up cold.
Companions: `PLAN.txt` (strategy, hardware), `docs/device-deps.md` (how the
manifest was derived), `docs/STATUS.md` (short status).

---

## 1. What Tadpole is

Runs LeapPad2 ARM binaries on a PC under **qemu-user**, with a shim faking the
hardware they expect, and a native SDL2 viewer showing the framebuffer.

There is **no kernel, no boot, no machine emulation**. A LeapPad2 binary is
just a process. Everything below its syscalls is faked.

```
┌─ qemu-arm (one process) ───────────────────┐
│   AppManager / saplayer / Brio   (ARM)     │
│                │                           │
│     [ shim: open/openat/ioctl/fopen ]      │  impersonates libdl.so.0
│          │                  │              │  or libz.so.1
└──────────┼──────────────────┼──────────────┘
      mmap │            ioctl │ state
           ▼                  ▼
   /tmp/tadpole/fb{0,1,2}.bin   state.bin      ← plain host files
           ▲                  ▲
┌──────────┼──────────────────┼──────────────┐
│   tadpole-view (native x86-64, SDL2)       │
│   composites layers → window; keys/mouse ──┼──▶ FIFOs → guest evdev
└────────────────────────────────────────────┘
```

Pixels cross the boundary with **no copying and no protocol**: the guest's
`mmap` of `/dev/fb0` is a real mmap of a host file; the viewer maps the same
file. Only the control path (ioctl) is emulated.

---

## 2. Current state

**Working**
- Stock firmware 4.6.0.784 obtained via LFConnect; rootfs extracted and
  verified complete against its own `erootfs.md5` manifest (1611 files).
- ARM binaries run. `uname -m` → `armv7l`.
- Display: 3 framebuffers, correct geometry, **pixel-exact** output verified
  against the source asset.
- Input: 6 evdev nodes with the real hardware names.
- `AppManager` completes `SetupSystem` and stays alive.
- **`saplayer` runs real Flash Lite content and executes LeapFrog's own
  ActionScript** — the furthest we've got.

- **AppManager runs the whole first-run flow**, through profile creation with
  the on-screen keyboard, Sign In, and onto the home screen.
- **The home screen is complete**: 7 tiles — Sneak Peeks, MyStuff, Cartridge,
  My Books, PetPad, Music, Camera — with icons and NEW! badges. Needed
  `ProfileAccess` in meta.inf; see "Missing home-screen apps — SOLVED".
- Five Flash titles run: Calculator, Notepad, Clock, Calendar, Pet Pad.
- Touch input no longer crashes.
- The UI can be driven headlessly and repeatably — `tools/probe-home.sh`,
  `tools/fbshot.py`, `tools/tap.py`.
- **Touch is correct at any window size and rotation.** SDL already delivers
  logical mouse coordinates when a logical size is set; the viewer was
  converting a second time. See "Touch offset — SOLVED".
- **Audio plays** — home-screen music and narration are audible. See "Audio".
- **Native Brio apps launch.** Music and Camera both start from the home
  screen; their UI chrome renders and they double-buffer. See below.

**Not working**
- Native apps launch but their content area is still blank — see
  "Native Brio apps".
- Launching a native (`.so`) app from the home screen is untested.
- `DaemonControl socket connect failed` still logged, but NOT fatal.

---

## 3. Run it

```
runtime/setup-sysroot.sh        # build the faked sysroot (idempotent)
tools/install-content.sh        # install 55 content packages into /LF/Bulk
cd tadpole && make              # ARM shim (x2 variants) + host viewer

./tadpole.sh --logo             # viewer + boot logo — best first test
./tadpole.sh --shell            # ARM shell in the guest
./tadpole.sh                    # VideoDaemon + AppManager
./tadpole.sh --run /LF/Base/Flash/bin/saplayer -- \
    /LF/Bulk/ProgramFiles/PADS-0x0028000D-000000/Main.swf
```

Viewer: arrows = D-pad, Z/X = A/B, Home = menu, Esc = back, mouse = stylus,
Ctrl+Q quit, `-s 3` for a bigger window.

Standalone Flash apps to try (`/LF/Bulk/ProgramFiles/<id>/Main.swf`):
`PADS-0x0028000C-000000` Notepad, `-0x0028000D-` Calculator,
`-0x0028000E-` Clock, `-0x0028000F-` Calendar.

Device shell (needs `/flags/developer` on the device):
`python3 tools/lfsh.py <ip> "command"`

---

## 4. The five techniques that made it work

These were each non-obvious and each blocked everything until solved.

### 4.1 LD_PRELOAD does not exist here

LeapFrog's uClibc 0.9.32 was built **without `__LDSO_PRELOAD_ENV_SUPPORT__`**.
The guest loader never opens an `LD_PRELOAD` path — verified with a known-good
ARM library, so it is not a fault in our object. (`LD_PRELOAD` appears in
`ld-uClibc` only in the list of vars it scrubs for setuid binaries.)
`LD_LIBRARY_PATH` *is* honoured.

**Solution — impersonate a library the target already links:**
1. copy the real library aside, patching its SONAME **in place** to a
   same-length name so no offsets shift (`libdl.so.0` → `libdl.so.9`)
2. build the shim with SONAME set to the **original** name, `DT_NEEDED` on the
   renamed real one
3. put it first in `LD_LIBRARY_PATH`

Our `open`/`ioctl` win symbol lookup; everything else resolves through to the
real library, so `dlopen`/`dlsym` still work and `dlsym(RTLD_NEXT, …)` behaves.

Two variants needed:
| variant | covers | why |
|---|---|---|
| `libdl.so.0` | AppManager, VideoDaemon, saplayer | libdl is #22 in DT_NEEDED, libc #33 |
| `libz.so.1` | imager-fb, drawtext-fb, flipbook-fb, fbtest | link no libdl at all |

### 4.2 qemu-user cannot create files in the sysroot

`-L` only redirects paths that **already exist**. A new file falls through to
the host path and fails:

```
stat64("/LF/Bulk/Data/Uploads/0")                  = 0        ← dir exists
open(".../profile.log", O_WRONLY|O_CREAT|O_TRUNC)  = -1 ENOENT
```

The guest can read the sysroot but never write to it. The shim translates
creating opens itself (`TADPOLE_SYSROOT`).

### 4.3 Hooking open() is not enough — fopen bypasses it

uClibc's stdio reaches its own open through a hidden alias that never touches
the PLT, so interposing `open` cannot see `fopen`. The shim hooks
`fopen`/`fopen64` too. Without this, nothing using C stdio can create files.

### 4.4 qemu-user's 8MB default stack is too small  ← the big one

Both AppManager and saplayer segfaulted identically: faulting on
`str r1, [sp]` with `sp` exactly 8MB below the stack base — i.e. **stack
exhaustion**, not a logic bug. Brio and Flash Lite recurse deeply through
their scene graphs and printf-family frames are large.

`qemu-arm -s 67108864` (64MB) fixed **both**. This single flag took the
project from "crashes everywhere" to "runs".

Diagnose this class of bug with:
`qemu-arm -g <port>` + host `gdb -ex 'set architecture arm'` — real
backtraces through stripped binaries via library symtabs.

### 4.5 Brio pans a different framebuffer than it draws into

Symptom: everything renders, the logs look perfect, but the window shows a
stale page (e.g. the boot logo from the priming step).

What actually happens, from `--debug` ioctl logging:

```
CreateHandle: 480x272 (1920) @ 0x820ff000     <- fb0 base + 0xFF000
[tadpole] fb1 PAN yoff=544                     <- pan issued on fb1
```

`0xFF000` = 544 lines = exactly two screens. Brio allocates the drawing
buffer inside **fb0's** smem but issues `FBIOPAN_DISPLAY` on **fb1** — it
treats the three framebuffers as one address space, so the pan is a *shared
scanout offset*, not a per-layer one.

The shim therefore applies a pan to every claimed layer. Also: layers are
enabled **on use** (pan / mode-set / unblank), not assumed to be fb0 — Flash
titles claim fb1.

Verify with `--debug` and by dumping `state.bin`; the active layer should show
`enabled=1 blank=0 yoffset=544`.

### 4.6 rename() has the same trap as open(O_CREAT)

Brio's `CAtomicFile` writes `<name>.atomic` then renames it into place. qemu's
`-L` only redirects paths that already exist, so a rename whose DESTINATION
does not exist falls through to the host and fails — every atomic write in the
system is stranded as a `.atomic` file. `/tmp/ui_ready.atomic` sitting there
was the symptom. The shim translates `rename()` and `unlink()` too.

### 4.7 VideoDaemon was never broken — it double-forks

Hours were lost here. VideoDaemon prints "Started Process", then:

```
clone()          fork 1
  setsid()       new session
  clone()        fork 2
    chdir("/")   <- the real daemon, survives
exit_group(0)    <- PARENT exits, and this is what you measure
```

`exit=0` is the parent doing exactly what a daemon should. Checking "did it
stay up?" by the launcher's exit status always says no. Check for a surviving
process and for the files it creates instead.

### 4.8 ALSA must succeed or the UI never renders

`CAudioMixer` opens "plugdmix", falls back to "plughw:0,0". Neither works: the
guest's alsa-lib 1.0.24 cannot drive the host card through qemu ("Invalid
value for card"), even though the guest correctly detects `Found codec: PCH`.
CAudioMixer then **retries forever**, and the spin starves the UI thread — you
get a black screen with thousands of ALSA errors per second.

Fix: `runtime/sysroot/etc/asound.conf` defines a **null sink**, so the open
succeeds and the loop stops. ALSA errors drop from thousands to zero and the
UI renders. This SILENCES audio rather than implementing it. Real output
should route through the shim to SDL, exactly like the framebuffer.

### 4.9 tslib crashes on the first touch — use Brio's own path

tslib's chain (`input->pthres->variance->dejitter->linear`) has a null
`ops->read` once a real touch event arrives; the first tap dies on `blx r3`
with r3 = 0. Brio has its own touchscreen path and announces it —
"Falling back on touchscreen interface" — and that path works. `tadpole.sh`
points `TSLIB_CONFFILE` at a nonexistent file so tslib init fails cleanly.
`TADPOLE_TSLIB=1` re-enables it.

Two dead ends chased here, recorded so nobody repeats them: it is NOT our shim
tail-calling a null `real_ioctl` (all dlsym lookups resolve), and the faulting
PLT entry is `fprintf`, not `read` (off-by-one when indexing `.rel.plt`).

### 4.10 All layers share ONE framebuffer arena

The single most misleading bug so far. Brio allocates every layer inside
**fb0's** memory but pans a **different fb device** for each:

```
CreateHandle: 0x18840 @ 0x820ff000    fb0 base + 0xFF000  = line 544
CreateHandle: 0x53b38 @ 0x8217e800    fb0 base + 0x17E800 = line 816
fb0 PAN yoff=544        <- layer 0
fb1 PAN yoff=816        <- layer 1, but its pixels are in fb0's memory
```

So layer N's pixels live in the shared arena at `layer[N].yoffset` — NOT in
`fbN.bin`. Backing each device with its own file loses every layer but the
first, and the screen goes black while the logs look perfect.

The shim now backs all three `/dev/fbN` with one arena file (`fb0.bin`) and
keeps a **per-layer** yoffset; the viewer maps that one arena for every layer.

An earlier version broadcast each pan to all layers ("shared scanout offset").
That was wrong — it collapsed every layer onto one offset and hid the content.
Per-layer offsets into a shared arena is the correct model.

`FBIOBLANK` arg 0 means SHOW, non-zero means HIDE. Observed live:
`fb0 BLANK arg=0`, `fb1 BLANK arg=0`, `fb2 BLANK arg=1`.

### 4.11 Host library fall-through

Guest libs are split across `/lib`, `/usr/lib`, `/LF/Base/Brio/lib`,
`/LF/Base/lib`, `/LF/Base/Flash/lib`. When a lib is missing from the searched
dir, qemu falls back to the **host** path and hands the guest an x86-64 `.so`
(`is not an ELF executable for ARM`). No single search order works.

Fix: one flat dir of symlinks to every ARM `.so` (`runtime/libs`), first in
`LD_LIBRARY_PATH`. Plus a complete `/usr/lib` in the sysroot, because some
binaries resolve `libpthread.so.0` by absolute path even though it's in
`/lib`.

---

## 5. What the firmware demands

All derived from the firmware or a live device.

**sysfs** (`/sys/devices/system/board/`) — values from real hardware:
```
platform=VALENCIA   platform_family=LPAD   system_rev=0x310
lcd_size=480x272    lcd_type=ILI6480G2     lcd_mfg=K&D-1
```
`rcS` branches on `platform`; `libDisplay.so` parses `lcd_size` with `%ux%u`.
Also `/sys/devices/platform/lf2000-power/status`, `lf2000-aclmtr/calibration`,
`lf1000-dpc/{xres,yres}`, `/sys/class/graphics/fb0/rotate`.

**Framebuffers**: `/dev/fb0,1,2`. `smem_len` must cover **multiple** screens —
Brio allocates buffers inside one fb and flips with `FBIOPAN_DISPLAY`
(`AllocBuffer: new buf offset 000FF000` = two screens in). We advertise
`yres_virtual = yres * 8`. Viewer honours `yoffset`.

**Input**: six evdev nodes, matched by name via `EVIOCGNAME`. Exact names and
order from `/proc/bus/input/devices` on hardware:
```
event0 "LF2000 USB"            lf2000/usb              EV=0x21
event1 "gpio-keys"             gpio-keys/input0        EV=0x23
event2 "touchscreen interface" lf2000/touchscreen      EV=0x0b
event3 "touchscreen raw"       lf2000/touchscreen-raw  EV=0x09
event4 "LF2000 Accelerometer"  lf2000/aclmtr           EV=0x0b
event5 "Power Button"          lf2000/power_button     EV=0x03
```
The nodes must **exist as directory entries** or enumeration stops after
event1. `usr/bin/make_dev_nodes.sh` in the firmware confirms the required set.

**`/proc/mtd` + `/dev/mtd0..2`**: `CMfgData::Init` parses `/proc/mtd` for a
partition named `MfgData0`, then opens `/dev/mtd<N>`. Without it, `Init` fails
and `CMfgData::Read` segfaults in libc. With it, the locale lookup degrades to
`en-us`. mtd0 is named **`NOR_Boot`**.

**Runtime files** (from a booted device):
```
/tmp/bulk_ready = "1"      /tmp/splash = "0"     /tmp/initial (empty)
/tmp/cart_brio_state = "7, CARTRIDGE_STATE_REINSERT"
/flags/pointercal = "39032 -162 -4245764 28 20690 -1583256 65536"   ← tslib cal
/flags/volume = "7"        /flags/developer (enables telnetd+vsftpd)
```

**`/var/sounds` symlinks**: the shipped ones point at **`LucyAssets`** — a
different board. `rcS` repoints them per-platform; for VALENCIA they must
resolve into `LpadAssets`. Otherwise they dangle.

**`/LF/Bulk/Data/Uploads/0/`** must exist: `BaseUtils::CreateFile` recurses to
create missing parents but only ever retries `mkdir("/LF")`, looping ~175k
times until the stack blows. On hardware the dirs already exist.

---

## 6. Firmware and content

`Firmware-Base` (33MB .lfp/ZIP) contains `4,2268688,kernel.bin` and
`5,53477376,C4G-E1M-W4K-erootfs.ubi`. **Filename = flashing recipe**: leading
number is the target MTD index; `C4G-E1M-W4K` is the NAND geometry (Chip 4GB,
Erase 1MB, Write page 4KB) which is exactly the UBI parameters.

Extract with `ubireader_extract_files` — **not** `-k` (it makes read-only dirs
it then can't write into). Then `chmod +x` every ELF and script; ubireader
drops exec bits and you get `Exec format error` even for the loader.

`.lfp` = ZIP, `.lf2` = bzip2 tar. `LFC_Downloads_bigger.zip` has all **55**
packages. `tools/install-content.sh` reads each `meta.inf` and installs by
`Type`:
```
Application            -> /LF/Bulk/ProgramFiles/<PackageID>/
Download|MicroDownload -> /LF/Bulk/Downloads/<PackageID>/
LanguagePack           -> /LF/Bulk/            (tarball self-wraps)
Music|MusicInfo        -> /LF/Bulk/Music/<PackageID>/
```
Caveat: the device has some packages installed under their **Name**
(`KeyboardWidget`) and others under **PackageID** (`PAD2-0x001E0003-000006` =
PaintWidget). No meta.inf field distinguishes them — looks historical. We use
PackageID uniformly; the device proves that form is accepted.

**DRM: none found.** SWFs are plain `Macromedia Flash data, version 8`, Brio
libs are plain ELF, assets are plaintext JSON. Nothing references the
NXP3200 AES engine or ECID. Cartridges are still unverified (M7).

---

## 7. Hardware facts

```
SoC     Nexell NXP3200, Cortex-A9 550MHz (ARMv7-A)
RAM     128MB DDR2       NAND 4GB       NOR 512KB (bootloader)
Display GiantPlus 480x272 5" touchscreen, ILI6480G2
Audio   LFP200          Kernel 2.6.37   U-Boot 2010.06
libc    uClibc 0.9.32, ARMv7-A + VFPv2, SOFT-FLOAT calling convention
```
`readelf` on AppManager: `Tag_ABI_VFP_args` **absent** ⇒ floats in core
registers. This is **not** Debian armhf. Build the shim with
`-march=armv7-a -mfpu=vfp -mfloat-abi=softfp`.

Partitions: mtd0 NOR_Boot, mtd1/2 MfgData0/1, mtd3 Reserved, mtd4 Kernel,
mtd5 RFS (UBIFS), mtd6 Bulk (UBIFS, 3.3GB).

Recovery combos: USB/Surgeon = D-pad RIGHT + HOME at power-on;
Mfg Test = VOL-UP + VOL-DOWN; U-Boot console = serial, shift+M then `o`,`n`.

---

## 8. Files

```
tadpole.sh                  one-command launcher
runtime/
  setup-sysroot.sh          builds the faked sysroot (idempotent)
  run.sh                    lower-level runner
  sysroot/                  the faked guest filesystem
  libs/                     flat dir of every ARM .so
  shimlibs/  shimlibs-z/    the two shim variants + renamed real libs
tadpole/
  Makefile                  make check / shim / shimz / viewer
  shim/tadpole_shim.c       guest-side device shim
  viewer/tadpole_view.c     native SDL2 viewer
tools/
  install-content.sh        install .lfp/.lf2 packages into Bulk
  lfsh.py                   scripted shell to a real device over telnet
rootfs/stock-4.6.0.784/…    extracted stock rootfs (NEVER redistribute)
sources/nxp320/             firmware + GPL sources
reference/device-capture/   captures from real hardware
docs/                       this file, device-deps.md, STATUS.md
shots/                      screenshots
```

**`.gitignore` `rootfs/`, `sources/`, `runtime/sysroot/` before any commit.**
Publish the emulator and the format docs; never the firmware or content.

---

## 9. Open threads

1. **VideoDaemon exits without creating its socket.** On hardware it runs fine
   (pid 472) and creates `/tmp/video_events_socket`, so it's environmental.
   Next: `strace` it on the device and diff.
2. ~~Flash apps stall~~ **FIXED.** Needed `/LF/Bulk/Data/Local/<profile>/`
   for `saveGame.xml`. The Calculator now runs to a fully drawn UI.
   Remaining: input round-trip untested; audio not attempted.
3. Audio (ALSA) not attempted.
4. Cartridge support (M7) — `/etc/mdev/cartridge.sh` + `cnotify` is the
   documented insert path.
5. Full-system emulation (M9) remains plausible: Cortex-A9 is well supported
   by QEMU, U-Boot source is in the drop, and `mach-nxp3200/prototype/module/`
   is a complete Nexell SoC HAL.

## 10. A note on the "doom" message

`HasDoomPackageCritical: global doom file detected` is **cosmetic**. It is
logged before the locale lookup and never touches the filesystem — AppManager
does not stat `/LF/doom` or `CriticalDoom.json` at all. On real hardware doom
produces a warning *screen*, not a crash. Do not chase it.

## Touch coordinate space — OPEN

Our input pipeline is verified exact: injecting `ABS_X=338 ABS_Y=200` into the
FIFO makes the guest read back `ABS code=0 val=338`, `code=1 val=200`. Nothing
is distorted between the viewer and the app.

So when taps land in the wrong place, Brio's non-tslib fallback is applying
its own transform — we are sending correct numbers in the wrong SPACE. Screen
pixels (0..479, 0..271) are apparently not what it expects.

**To resolve:** on real hardware run

```
evtest /dev/input/event2
```

and read the reported ABS_X / ABS_Y min and max. That is the driver's true
coordinate range. Then set (or bake in) `TADPOLE_TS_MAX_X` / `TADPOLE_TS_MAX_Y`.

`tools/lfsh.py <ip> "evtest /dev/input/event2"` should work if evtest is
non-interactive enough; otherwise capture it from a telnet session.

Debugging aids: `TADPOLE_TOUCH_DEBUG=1` makes the viewer print
`touch down win(x,y) -> fb(x,y) rot=N`, and `--debug` makes the shim print
`ev2 GUEST-GOT ABS code=0 val=...` for what the guest actually receives.
Together they trace one click end to end.

## Touch offset — CAUSE FOUND (HiDPI)

Not Brio's transform after all. SDL reports mouse positions in **window**
coordinates while the renderer works in **drawable** pixels; on a scaled
display these differ and `SDL_RenderWindowToLogical` does not bridge them.
Clicks were therefore wrong by exactly the display scale factor — and lined up
only at the one window size where window size == drawable size, which is what
made it look like a mysterious "pattern".

`window_to_fb()` now converts window -> drawable via `SDL_GetWindowSize` vs
`SDL_GetRendererOutputSize` before calling `SDL_RenderWindowToLogical`.
`TADPOLE_TOUCH_DEBUG=1` prints both sizes so a mismatch is visible.

Note: the earlier conclusion "our pipeline is exact, so Brio must be
transforming" was right about the pipeline (injected events do arrive intact)
but wrong about the inference — the distortion was upstream of the FIFO, in
the viewer's own mouse handling.

## Home screen reached — but sparse

The first-run flow completes: profile creation, then the themed home screen.

`/LF/Base/LpadAssets_en/Data/ProgramFileAppOrder.json` is the home-screen app
list, keyed by PackageID:

```
PAD2-0x001F0005-000000  10  LeapPad Pet Pad
PADS-0x001F0006-000000  20  Camera & Video Recorder
PADS-0x001F0007-000000  30  GalleryApp
PAD2-0x001E0013-000000  40  My Books
MULT-0x001B00B9-000000  50  Explorer Music Player
PAD2-0x001E0010-000000  60  SneakPeekWidget
```

Only some tiles appear. Ruled out so far:
* NOT directory naming — SneakPeekWidget has no PackageID directory (it
  self-wraps to `SneakPeekWidget/`) yet still appears, while Camera/Gallery/
  Music have correct PackageID directories and do not.
* NOT the `Hidden` / `DeviceHidden` flags — My Books appears **with**
  `Hidden=1`, the absent ones have no such flag.

Unverified hypothesis: the missing apps are native `.so` apps whose hardware
or content prerequisites are absent (no camera device, no photos, no music on
the Bulk partition), and the UI hides them. Needs checking against the real
device's home screen, which is the only authority.

## Missing home-screen apps — SOLVED: DeviceAsset packages

The apps were installed correctly all along. What was missing were their
**DeviceAsset** packages — a separate package type carrying each app's
home-screen icon (`BaseIcon.png` / `BaseImage.png`) and `GameInfo.json`.
Without one, an app is installed but has no tile, so the home screen never
shows it.

They pair by ProductID: `PADS-0x001F0006-DA0000` belongs to
`PADS-0x001F0006-000000` (Camera & Video Recorder).

`lfpkg` installs a DeviceAsset **into its parent package's directory**, found
like this (from `usr/bin/lfpkg`):

```sh
PRODUCT_ID=`echo $PKGDIR | cut -d '-' -f 2`
DA_PRODUCT_ID=$PRODUCT_ID-`echo $PKGDIR | sed s:-DA:-00: | cut -d '-' -f 3`
INSTALLED_DIR=`dirname $(find "$BASE" -name "meta.inf" | xargs fgrep -l "$DA_PRODUCT_ID")`
```

`tools/install-content.sh` implements this, and runs in **two passes** —
every Application must exist before any DeviceAsset is processed. lfpkg
installs one package at a time and never hits that ordering problem; a batch
installer does.

DeviceAssets whose parent app is absent are skipped by design: those belong to
cartridge or purchased titles not present in this firmware.

Earlier theories, all wrong and now retired: directory naming, `Hidden` flags,
postinstall scripts, and "the UI hides apps whose hardware is missing".

## /tmp must be wiped every launch

`/proc/mounts` on the device shows `/dev/ram0 /tmp tmpfs` — a tmpfs, empty at
every boot. Our sysroot `/tmp` is a real directory that persists, so flags
survive between runs. A leftover `/tmp/shutdown` makes AppManager exit
immediately (`::Run return mRestart:0`) before drawing anything.
`tadpole.sh` now clears `/tmp` on launch and recreates only the boot-time
files, and sweeps stranded `*.atomic` files.

## mkstemp64, not mkstemp

Brio's `fopenAtomic()` uses `mkstemp`, which creates a new file and so hits the
same qemu-user trap as `open(O_CREAT)`. But `libUtility.so` imports
**`mkstemp64`** — exporting only the plain name means the interception is
never reached. The shim exports both.

## MISTAKE: apps were being installed twice

While chasing `FindWidget` I added a second install of every Application under
its **Name** as well as its PackageID. The theory (that FindWidget matches on
Name) was later disproven — the real cause was self-wrapping archives — but
the duplicate install was never removed.

Result: `ProgramFiles/` held 32 entries instead of 18, with pairs like
`Calculator/` and `PADS-0x0028000D-000000/` carrying identical `meta.inf`
files and therefore duplicate PackageIDs. Anything enumerating ProgramFiles
would see each app twice.

`install-content.sh` now does exactly what lfpkg does and nothing more:
self-wrapping archive -> wrapper directory, flat archive -> PackageID
directory. 18 entries, no duplicate PackageIDs. The resulting mixed naming
(widgets under names like `KeyboardWidget/`, flat apps under PackageIDs)
matches the real device — which is the confirmation that the rule is right.

Lesson: when a theory is disproven, remove the change it motivated.

## Launching native apps directly

Native Brio apps are `.so` files exporting a C-linkage **`CreateApp`**, which
`libLightningBase.so` dlsyms after loading. They are not executables and
cannot be run standalone — they need Brio initialised and an app-host context,
which is what AppManager provides.

There is no file-based launch hook: `/tmp/restart` holds a player ID, not an
app path, and neither AppManager nor `/usr/bin/app` takes launch arguments.

So "launch a native app" currently means "make AppManager launch it", and
AppManager demonstrably can — it already loads `KeyboardWidget/App.so` during
profile creation. Getting other native apps running is therefore the same
problem as getting them onto the home screen.

A standalone launcher would mean reimplementing AppManager's app-host: init
Brio, dlopen, call `CreateApp`, drive the returned object's lifecycle. The
vtable would have to be reversed from `libLightningBase.so`. Feasible, but a
project in itself.

## Home-screen apps — compared against a WORKING device

`reference/device-capture/leappad2-working/` holds a capture from the freshly
reflashed, fully working LeapPad2.

**`/LF/Bulk/ProgramFiles/` on the device is IDENTICAL to ours** — the same 18
entries with the same mixed naming (widgets under wrapper names, flat apps
under PackageIDs). Installation is correct; that thread is closed.

The interesting file is the per-profile
`Data/Local/0/PAD2-0x1F1E0002-100000/ProgramFileAppOrder.json`. On the working
device it contains only:

```json
{"PAD2-0x001E0010-000000": 60, "PAD2-0x001E0013-000000": 40}
```

SneakPeekWidget and My Books — **the same two tiles our emulated home screen
shows**. So it records user ORDERING, not the full app list, and seeding it
from the 8-entry base defaults was wrong (removed).

`UIData.json` is written by AppManager itself and holds UI flags
(`BadgeNumber`, `HasProfileBeenViewed`, ...), not an app list.

This strongly suggests the emulated home screen is already behaving like the
hardware, and the remaining apps live somewhere the UI has to navigate to —
plausibly category pages, since meta.inf carries `Category="Creativity"`,
`"More"`, `"Other"`, `"Video"`. Broken touch would prevent reaching them.

**Open question for the hardware:** does the real device's home screen show
more than My Books + Sneak Peeks on its FIRST screen, and if so how are the
others reached? That determines whether anything is actually missing.

**ANSWERED: the hardware DOES show apps on page one.** The reasoning above was
wrong on two counts, both worth keeping as warnings:

* `ProgramFileAppOrder.json` is read from TWO places. The per-profile copy is
  only an override; the picker falls back to the base file in `/LF/Base` and
  that is what normally drives the screen. Reading only the per-profile copy
  made a 2-entry override look like the whole app list.
* "Our two tiles match the device's two-entry file" was a coincidence, and it
  was treated as confirmation. Two observations agreeing is not evidence when
  neither has been shown to be the cause.

The actual cause is in the next section.


## Missing home-screen apps — SOLVED: `ProfileAccess`

The home picker is Flash, and it says what it is doing. From a headless boot:

```
HomePickerState::GetAllIcons
[0x200] SystemPlugin::getProgramFileApps - Valencia - sort file exists \
        /LF/Base/LpadAssets_en/Data/ProgramFileAppOrder.json
HomePickerState::LoadIconData----------icon0 .. icon2
```

The screen is NOT an enumeration of `ProgramFiles`. It walks that sort file,
which lists 8 PackageIDs, and for each one checks which PROFILES may see the
app. **`ProfileAccess`** is that list; `-1,0,1,2,3` means "everyone, all four
profile slots". The LFConnect downloads omit it, so Camera, Gallery, Pet Pad
and Music were installed but invisible.

Confirmed against the live device
(`reference/device-capture/leappad2-working/05-meta-inf.txt`) — the factory
Bulk carries three fields the redistributable packages lack:

```
Size=0x02373000
DeviceAccess=0x00000000
ProfileAccess=-1,0,1,2,3
```

Measured with `tools/probe-home.sh`, varying only Camera's meta.inf:

| Camera's meta.inf | tiles |
|---|---|
| pristine, as LFConnect ships it | 3 |
| `+ Device="LeapPad2Explorer"` | 3 |
| `+ DeviceAccess=1` | 4 |
| `+ DeviceAccess=0x00000000` (the device's own value) | 3 |
| `+ ProfileAccess=-1,0,1,2,3` alone | **7** |

`tools/install-content.sh` now writes both `DeviceAccess=0x00000000` and
`ProfileAccess=-1,0,1,2,3` to match the factory exactly, scoped to packages the
sort file names. Result: 7 tiles — Sneak Peeks, MyStuff, Cartridge, My Books,
PetPad, Music, Camera.

**`DeviceAccess=1` was a FALSE POSITIVE and was briefly shipped as the fix.**
It really did take the screen from 3 tiles to 4, and it really was necessary
and sufficient in that one-variable experiment. But the hardware has
`DeviceAccess=0x00000000` and still shows every app, so it was never the
filter. What exposed it was checking the device's actual VALUE instead of
stopping at "the key is present" — the emulator showed 3 tiles with the exact
string the working device carries, which is impossible if that field were the
cause. **When a fix works, compare its value against hardware, not just its
shape.**

## Driving Tadpole headlessly

Clicking through the SDL window to test a one-line content change is slow and
not repeatable. Three tools make the UI scriptable:

* `tools/fbshot.py` — composites the shared arena to a PNG exactly as the
  viewer does (fb2→fb1→fb0, per-layer yoffset). No SDL needed.
* `tools/tap.py` — writes touch events straight into the shim's `ev2` FIFO.
  Because it bypasses the viewer entirely, it is also the reference for
  deciding whether a touch bug lives in the viewer's window→framebuffer
  mapping or in what Brio does with the values.
* `tools/probe-home.sh` — boot, sign in, dismiss the Connect nag, screenshot
  the home screen, and print the picker's own decisions.

Three traps, all of which produced convincing-looking wrong answers first:

* **Hold length.** Flash Lite polls the touchscreen on its frame tick, and
  under qemu that tick is far slower than on hardware. A 0.12s tap is observed
  as a release with no matching press — `ProcessMouseUp` with no
  `ProcessMouseDown`, and nothing responds. `tap.py` holds 0.8s.
* **Stale guests steal input.** `tadpole.sh` execs qemu as a grandchild, so
  killing the launcher leaves `AppManager` alive holding `ev2` open. Several
  leftover instances all read the same FIFO and split the event stream between
  them, which looks exactly like flaky touch. `probe-home.sh` reaps every guest
  bound to its `TADPOLE_DIR` before and after each run.
* **`PushState` is not "on screen".** The state is logged long before the movie
  has pixels. Wait for `onLoadInit( _level0.mcContent.<Name>_mc )`. Likewise
  `GetAllIcons` fires before the Connect nag appears, so it is not evidence the
  nag has closed — wait for `UIPetLPAD::EnableButtons`.

Never `pkill -f "$TADPOLE_DIR"`: the pattern matches the calling script's own
command line and kills the shell running it. Kill by PID from `pgrep`.


## Touchscreen ranges from real hardware

`evtest /dev/input/event2` on the device — the driver ADVERTISES a range it
does not use:

| axis | advertised | actually emitted |
|------|-----------|------------------|
| ABS_X | min 1, max 1023, fuzz 2 | 2 … 482 |
| ABS_Y | min 1, max 1023, fuzz 2 | 0 … 271 |
| ABS_PRESSURE | min 1, max 1023, fuzz 5 | 10 … 70 |

So coordinates arrive in **panel pixel space** (480×272), not in the advertised
0–1023 space. There is no `/etc/pointercal`, so tslib's `linear` module is
identity and passes pixels through unchanged.

**FIXED.** The shim used to report `max = g_w - 1` / `g_h - 1` — the panel
size, which no real device advertises. It now reports the hardware values
(min 1, max 1023, fuzz 2; pressure fuzz 5) and still emits pixels, exactly as
the device does. The viewer's pressure went from 255 to 60, inside the range
hardware actually produces.

Regression-tested with `tools/probe-home.sh`: sign-in still works and the home
screen still renders 7 tiles, so a tap at fb(355,57) still lands on the profile
pod. That also settles an open question — **Brio does NOT rescale by the
advertised range**; it treats ABS_X/ABS_Y as panel pixels directly. Had it
rescaled, changing 479 -> 1023 would have broken every tap.


## Touch offset — the viewer's mapping is NOT the cause

`tadpole-view --selftest [-r N]` round-trips framebuffer points out to window
coordinates and back through the REAL `window_to_fb`, at eight window sizes
including deliberately awkward aspect ratios. Result: **≤ 1 px error at every
size, at all four rotations.** Combined with `tap.py` (which bypasses the
viewer entirely and lands exactly), both ends of the pipeline are now measured
and correct.

So the earlier explanations were all wrong, and none should be revived without
new evidence:

* **"HiDPI double-correction."** Plausible, but `SDL_GetRendererOutputSize`
  equals `SDL_GetWindowSize` on this display, so that branch never runs. The
  pre-scale in `window_to_fb` is a no-op here, not a bug.
* **"Dividing by the render scale twice."** Would show up as an error growing
  with scale. The round-trip is clean at scale 0.7 through 3.0.
* **"It works at one specific window size."** No size behaves differently.

**A warning about the selftest itself.** Its first version tested only the
window CENTRE — which is a fixed point of every rotation, so it "passed" all
four rotations while proving nothing about any of them. The second version had
a bug in its own forward model: `SDL_RenderGetViewport` returns the viewport in
LOGICAL units (the stored output-pixel rect divided by the render scale), so
the letterbox offset must be added BEFORE scaling. Adding it after understates
the offset by `vp.x * (scale - 1)` and produced confident, reproducible,
entirely fictitious failures of up to 119 px that looked exactly like the
reported symptom. Both mistakes were caught by predicting the error magnitude
from the suspected cause and checking it against the number printed — a habit
worth keeping.

**SDL's own mouse-event path is now measured too, and it is also correct.**
No automated test here can reach it (XTEST clicks are not delivered to the SDL
surface under XWayland), so it was done by hand with `--touch-debug`, which
draws a red crosshair at the computed framebuffer point INTO the guest's pixel
buffer — the marker therefore rotates and scales by the identical path as the
content.

A real click at `-r 270`, window 378x613:

| | window coords |
|---|---|
| click, from the viewer's own log | (74, 157) |
| viewer computed | fb(357, 45) |
| where the renderer should draw fb(357,45) | (73.1, 156.4) |
| crosshair measured in the screenshot | (72, 152) |

The residual 4 px is the assumed title-bar height (38 px; at 42 it is exact).
The crosshair's arms measured 21 px for a 17 px framebuffer cross — 17 x 1.277,
the render scale — confirming it went through the rotation path.

So the viewer is fully exonerated: mapping, rotation, and the real mouse path.

Note when reading such a screenshot: the pointer will usually NOT be on the
crosshair, because motion with no button held neither logs nor moves the mark.
Compare the crosshair against the LOGGED click coordinates, not against where
the cursor happens to be sitting.

**BUT that measurement could not resolve a few percent.** Converting the
crosshair from screenshot pixels to window pixels needs the window decoration
sizes, and 38 vs 42 px of title bar moves the answer by 4 px — the same order
as the error being hunted. The user then reported the decisive fact: **clicking
near the window's top-left is accurate, and the error grows with distance from
it.** That is a SCALE error, not an offset, and no amount of crosshair-position
arithmetic would have separated the two.

`--touch-debug` therefore also draws a reference grid at known framebuffer
coordinates, 60 px apart, brighter at fb(30,30). Click a green dot: the red
cross shows where the viewer thinks you clicked, the error reads off in grid
units, and whether it grows with distance is obvious at a glance. No decoration
measuring, no dependence on where the cursor drifted. The debug line also
reports the nearest grid dot and the offset from it.

## Touch offset — SOLVED: SDL had already converted the coordinates

**SDL_MOUSEMOTION / SDL_MOUSEBUTTON* coordinates are ALREADY LOGICAL.** When a
renderer has a logical size, SDL's own renderer event watch rewrites those
events in place — render scale AND letterbox viewport offset both applied —
before the application sees them. `window_to_fb` then called
`SDL_RenderWindowToLogical` on coordinates that were already logical, dividing
by the render scale a **second** time.

The error is therefore zero at the viewport origin and grows linearly with
distance from it: invisible at `-s 1`, exactly 2x at `-s 2`. Which is exactly
what was reported for weeks — "it works if I shrink the window to about 1:1,
and gets worse the bigger I make it".

Isolated by adding one line to a minimal SDL program:

| logical size set? | true window-relative | `event.x/y` | `SDL_GetMouseState` |
|---|---|---|---|
| no  | (300,400) | (300,400) | (300,400) |
| yes | (300,400) | **(150,200)** | (300,400) |

Note `SDL_GetMouseState()` is NOT rewritten — only the event structs. Never mix
the two.

Fix: `event_to_fb()` takes the event coordinates as logical and only undoes the
rotation. Verified end to end by driving the real viewer with xdotool: clicking
the Ghb profile pod at `-s 2` now yields fb(355,57) and the guest signs in, and
the same visual target maps to fb(355,57) +/-1 in 700x900, 900x700, 400x900 and
1000x620 windows — pillarboxed, letterboxed, wide and narrow.

### Why this took so long — read before trusting a test here

* **`--selftest` could never have caught it.** It feeds `window_to_fb`
  synthetic WINDOW coordinates, so it tested the function against the
  convention the code assumed rather than the one SDL actually uses. It passed
  at every size and rotation while the bug was live. A test that supplies its
  own inputs cannot discover that the real inputs are in a different space.
* **The `--touch-debug` crosshair also could not catch it**, because it is
  drawn at the framebuffer point the viewer computed — so it always agrees with
  the viewer, right or wrong. It confirms rendering, not input.
* **Three theories died on measurement**: HiDPI double-correction (output size
  equals window size here, so that branch never ran), stale render scale after
  resize (0.9853 and 2.0 were both correct for their windows), and "the content
  renders at 1:1" (a screenshot showed the outline filling the window with dots
  120 px apart).
* **A coincidence in the numbers cost hours.** 14 clicks fitted a "renders at
  scale 1" hypothesis with 19 px total error versus 419 px for the correct one,
  because 60 px window spacing happens to be what a 60 px framebuffer grid
  looks like at 1:1. The clicks simply were not on the dots. Screenshotting the
  actual window settled in one image what the arithmetic could not.
* **xdotool cannot address every monitor.** `xdotool getdisplaygeometry`
  reports 1920x1080 while the desktop spans 3000x1920 across three outputs. A
  window centred at x=2188 is outside xdotool's reach, and clicks aimed there
  land somewhere else entirely — which produced a convincing but entirely false
  "exactly half" reproduction. Force `SDL_VIDEODRIVER=x11` and
  `xdotool windowmove` the window onto the primary output first.

The user's own observation — accurate near the top-left, worse with distance —
was the single most valuable datum in the whole hunt, and it was correct from
the moment it was made.


## Audio

`qemu-user does not forward the ALSA control ioctls` — `SNDRV_CTL_IOCTL_CARD_INFO`
works natively (card 0, id "PCH") but returns ENOTTY through qemu. So there is
nothing to pass through TO, and the real libasound cannot reach a device even
if allowed to try. `tadpole/shim/tadpole_asound.c` REPLACES libasound.so.2
outright, implementing exactly the 26 `snd_pcm_*` symbols
`LF/Base/Brio/Module/libAudio.so` imports (Brio uses `snd_pcm_mmap_writei`, not
`snd_pcm_writei`).

PCM goes to `$TADPOLE_DIR/audio`, a FIFO; the negotiated format is left in
`$TADPOLE_DIR/audio.fmt` as one line of text so the viewer can open its SDL
device to match without touching the shared state struct. The viewer drains the
FIFO into a ring in the main loop and SDL's callback copies out of it — the
callback must never block, and a FIFO read can return short.

**Pacing matters.** Writes are non-blocking but retry with a bounded wait, so
the guest is throttled by how fast the viewer consumes (which SDL paces). With
an infinitely fast reader (`cat`) Brio free-runs at ~78x real time: 748 MB in
75 s. Bounded retry means a dead viewer drops audio instead of wedging Brio's
audio thread.

**PROVEN END TO END.** `make tone` builds a freestanding ARM binary that writes
a 440 Hz tone through the shim:

```
./tadpole.sh --run "$PWD/runtime/tone"
  -> audio.fmt = "32000 2 16 1024"
  -> 131072 bytes captured, 100% non-zero, peak 21844/32767
```

`shots/tadpole-audio-proof.wav` is that capture. So the transport is not the
problem.

### /proc/asound/card0/id — the guest was reading the HOST's sound card

`CAudioModule` reads `/proc/asound/card0/id` and logs `Found codec: %s`. We had
no such file, and qemu's `-L` falls through to the HOST path for anything
missing, so the guest was reading the development machine's Intel HDA and
finding **"PCH"**. Real hardware reports `socaudiolfp100` (confirmed in
`LF/Base/MfgTest/MfgTest_ReleaseNotes.txt`). `setup-sysroot.sh` now creates it.
This did NOT fix the silence, but any missing /proc or /sys path is a silent
host-leak of this kind and worth auditing.

### Brio DOES produce sound — and how I convinced myself otherwise

Home-screen music and the "to move an app, drag where you want it to go"
narration are audible. Getting there needed nothing beyond the shim above.

I reported the opposite first, twice, from the same two mistakes:

* **Analysed only the head of the capture.** The scan stopped after 150 MB of
  what grew to a 1.6 GB stream, and the audio starts at byte 805,306,368 —
  49.6% in. Everything before that is genuinely silent because it is the boot
  phase. Scanning 9% of a file and reporting "0.00% non-zero, peak 0" reads as
  a precise measurement; it was a precise measurement of the wrong 9%.
* **Never signed in.** The harness waited for `SignIn_mc` and stopped there.
  The music and narration are on the HOME screen, past a tap the test never
  made. `tools/probe-home.sh` exists precisely to get past that and was not
  used.

The `loadSound` clue that seemed decisive — ActionScript naming an .mp3 that
never appears in an `open` trace — was consistent with sound working all along.
It only looked damning next to a silence measurement that was itself wrong. Two
weak signals agreeing is not corroboration.

**When measuring a stream, scan all of it, and check the harness reached the
state where the thing you are measuring actually happens.**

NOTE: `aplay` from the rootfs is NOT a usable test — it needs 94 `snd_*`
symbols against Brio's 26, 70 of which we do not implement.


## Native Brio apps — SOLVED: there were TWO libdl instances

Tapping Music or Camera on the home screen used to segfault every time: the
launch animation played, the screen blanked, then SIGSEGV. The log always
stopped at the same place, and it is NOT where it looks:

```
LoadNewApp path = /LF/Bulk/ProgramFiles/MULT-0x001B00B9-000000/App.so
LoadNewApp: after ...              <- the new app LOADED fine
mCurrentTransition = kReplaceTopApp !
ExitPopUnloadApp: before UnloadModule /LF/Base/LPAD/
qemu: uncaught target signal 11
```

The crash is in UNLOADING THE OLD APP, not loading the new one.

qemu-user writes an ARM target core (`$SYSROOT/qemu_<prog>_*.core`). Read it
with `gdb -ex 'set architecture arm' -ex "core-file <core>" <ARM binary>`:

```
#0  0x440099d0 in _dl_find_hash () from lib/ld-uClibc-0.9.32.1-git.so
```

`_dl_find_hash` is the loader's symbol lookup. `info sharedlibrary` showed why:

```
0x443b0000  runtime/shimlibs/libdl.so.0      <- our shim
0x44e40000  /lib/libdl-0.9.32.1-git.so       <- the REAL libdl, loaded AGAIN
```

**Two providers of dlopen/dlsym/dlclose in one link map.** The shim is found
through `LD_LIBRARY_PATH`, but `/lib/libdl.so.0` and `/usr/lib/libdl.so.0` in
the sysroot still pointed at the real uClibc libdl, so any absolute-path
resolution loaded a second copy. Loading tolerated it; `dlclose` did not — the
loader walked the duplicated scope and dereferenced garbage.

Fix (`setup-sysroot.sh`): point both paths at the shim, so there is exactly one
libdl. dlopen/dlsym/dlclose still resolve, through the shim's `DT_NEEDED` on
the renamed real `libdl.so.9`. Music and Camera now both launch, render their
UI chrome, and double-buffer (`fb1 PAN yoff=816/1088` alternating).

### Also fixed on the way: the shims depended on themselves

Both shims had a self-referential `DT_NEEDED`:

```
libdl.so.0:  SONAME=libdl.so.0  NEEDED=libdl.so.9 libc.so.0 libdl.so.0
libz.so.1:   SONAME=libz.so.1   NEEDED=libz.so.9  libdl.so.9 libc.so.0 libz.so.1
```

`-L$(GUESTLIBS) -l:...` let lld satisfy `dlsym` from the ORIGINAL
`runtime/libs/libdl.so.0` and add a `DT_NEEDED` on our own SONAME — a cycle in
the link map. Now linked against explicit file paths so lld cannot wander.

This was NOT the crash (fixing it alone changed nothing) but it is wrong, and a
cycle plus a duplicate provider is not a state worth reasoning about.

### Reading a qemu target core

Do not trust the reported faulting instruction — the first core blamed
`sub r0, r0, #1`, which cannot fault. The FUNCTION was correct and the register
dump and `info sharedlibrary` were what actually solved it. Also note gdb
resolves recorded guest paths against the HOST filesystem, so
`/usr/lib/libz.so.1` appears in `info sharedlibrary` as an x86-64 object even
though the guest correctly loaded the ARM one — a gdb artifact, not a
host-leak. Check `readlink -f` on the sysroot path before chasing it.

### OPEN: the app content area is blank

Music renders its rainbow chrome and Camera starts, but the inner region stays
black. The apps are alive and panning, so this is a rendering or asset path
issue, not a crash. `tools/probe-launch.sh <app>` reproduces either in one
command.

## Cartridges

`/LF/Cart` is the mount point (`/etc/mdev/cartridge.sh`: `mount -r /dev/mtdblock7 /LF/Cart`).
Cartridge tars from LFManager unpack flat into it: `App.so`, `Data/`,
`meta.inf`, `GameInfo.json`, `Game_Icon.png`, `PopUpIcon.png`.

**Cart state is PUSHED, not polled.** Brio's CartridgeTask (in `libEvent.so`)
listens on `/tmp/cart_events_socket`; `/sbin/cnotify <n>` sends a state and also
writes `/tmp/cart_brio_state` in the format `"%d, %s"`. With no mdev we write
that file directly from `tadpole.sh`.

The enum is POSITIONAL, in the order the strings appear in `cnotify`:

```
0 NONE   1 INSERTED  2 DRIVER_READY  3 READY   4 REMOVED
5 FS_CLEAN  6 CLEAN   7 REINSERT  8 RESTART_APPMANAGER  9 REBOOT  10 UNKNOWN
```

**`tadpole.sh` used to hardcode `7, CARTRIDGE_STATE_REINSERT` on every boot** —
telling the UI "this cartridge is bad, take it out and put it back" forever.
It now reports `3, CARTRIDGE_STATE_READY` when `/LF/Cart` is non-empty and
`0, CARTRIDGE_STATE_NONE` when it is not.

With SpongeBob: The Clam Prix unpacked into `/LF/Cart`, the picker reports
`SetCartStatus 3`, loads **8 icons instead of 7**, and calls `EnableArrows` —
i.e. it built a second page for the cart.

Device ground truth (`reference/device-capture/leappad2-working/07-cart.txt`,
captured with no cart inserted — the port is broken):

```
/proc/mtd stops at mtd6 "Bulk"        <- mtd7 is the CARTRIDGE, only when present
cart_hotswap = "0\t0"                  <- two values, tab separated
/LF/Cart exists but empty;  /LF/System does not exist;  /cart_mounted absent
```

### OPEN: the cart tile has no artwork

```
ERROR: HomePickerState::LoadIconImage passed bogus GameInfo object: undefined
```

The slot exists but its GameInfo is undefined, so the tile does not render.
**The guest never opens anything under `/LF/Cart`** — verified by tracing a full
boot to the home screen with `PROBE_DEBUG=1 tools/probe-home.sh`. So the UI does
not read the cartridge's `GameInfo.json` from the mount point; the data must
arrive some other way. `libCartridgeMPI.so` references `/LF/System/Cartridge`,
which is also never touched and does not exist on the device either.

Next: find who supplies cart GameInfo to the picker — likely a `SystemPlugin`
call backed by CartridgeMPI. Note the cart's own `GameInfo.json` has no `Icon`
or `IconPADS` key, only `Title`, `LargeIcon`, `Audio` and `IconLable` (sic);
the icon proper is `Icon="Game_Icon.png"` in `meta.inf`.

NOTE: the game is `Device="LeapsterExplorer"`, `PackageID="LST3-..."`, and
declares `Depends="LST3-0x00170030-000001"` — a dependency we have not
satisfied and have not yet checked the meaning of.

### CORRECTION: the shim log cannot prove a file was never opened

I twice claimed "the guest never opens anything under /LF/Cart", based on the
`--debug` shim log. That is not a supportable conclusion. The shim intercepts
`open`/`open64`/`openat`/`fopen`/`fopen64` — but `stat`, `access`, `opendir`
and friends do not go through any of them, exactly like the `fopen` bypass in
4.3. Absence from the shim log means "the shim did not see it", not "it did not
happen".

Use `TADPOLE_STRACE=1 ./tadpole.sh ...` for real guest syscall tracing when the
question is whether a path was touched at all. Output is large; filter it.

Also note `CSystemData::GetCartPath()` does build `/LF/Cart/` — the string table
in libLightningJSON pairs Type names (`CartLTM`, `Avatar`, `DeviceAsset`, ...)
with install paths (`Rsrc/Cart`, `Bulk/ProgramFiles`, `Bulk/Downloads`, ...),
and `Cart/` sits with `System`/`Application` in the adjacent table.

### Strongest untested lead for the blank cart tile

`LTM::CMetaInfo::CheckCartDependencies()` exists in both `LeapFrogPlugin.so` and
`libLightningJSON.so`, and `LeapFrogPlugin.so` has a `_cartDependency` symbol.
The Clam Prix meta.inf declares:

```
Depends="LST3-0x00170030-000001","1.0.0.0"
```

That package is NOT installed, and has not been located in the LFC cache. If
CheckCartDependencies rejects the cart, a slot with no GameInfo is exactly what
you would expect. Worth checking before anything else.

Relevant: the owner reports real hardware ALSO shows the cart-with-an-X icon for
carts it cannot read, so this failure mode may not be emulation-specific at all.

## Installing cartridge backups as apps — WORKS

LFManager-style install beats emulating a cartridge, and it is what the owner
already had working on real hardware. Unpack a game tar straight into
`/LF/Bulk/ProgramFiles/<PackageID>/` and add `ProfileAccess=-1,0,1,2,3`.

SpongeBob: Fists of Foam (`LST3-0x00180010-000000`) installed this way appears
on the home screen as a "Game" tile with its own artwork, and LAUNCHES.

**This corrects an overstatement earlier in this file.** `getProgramFileApps`
does NOT only show packages named in
`/LF/Base/LpadAssets_en/Data/ProgramFileAppOrder.json` — FOF is absent from that
file and still appears. The sort file controls ORDERING. `ProfileAccess` is the
filter. (The original 3-tile symptom was real; the explanation was too strong.)

Note Leapster games declare `Device="LeapsterExplorer"` and a `LST3-` PackageID,
and the LeapPad2 runs them with an on-screen A/B/L/R overlay loaded from
`/LF/Base/EmeraldTitles/<PackageID>/ViewFrame.json`. We ship EmeraldTitles
ViewFrames for several titles but not necessarily for every game.

Clam Prix additionally declares `Depends="LST3-0x00170030-000001"`, which FOF
does NOT. That difference is worth remembering — FOF is known to run on
hardware, Clam Prix is untested.

## THE blocker for native 3D apps: no GPU

Both the Camera app and Fists of Foam now load fully, resolve their libraries,
and die in exactly the same place:

```
[0x5] eglInitialize()
[tadpole] open /dev/fb1
ERROR Msg : file src/egl/vr5_platform_fbdev.cpp, line 391.
The memory device has not been opened.<ASSERT>: [0x5] eglInitialize() failed
PowerDown (Assert) exit !!
```

`libvr5.so` (the VR5 GPU driver) opens three device nodes we do not provide:

```
/dev/mem  /dev/ogl_vr5  /dev/vmem
```

Our sysroot `/dev` has only `fb0 fb1 fb2 input mtd0 mtd1 mtd2`. `/dev/vmem` is
almost certainly "the memory device" in the assert. This is the single thing
standing between us and native games — and it is a real GPU interface, not a
one-line fake: OpenGL ES 1.x rendering would have to be translated to the host.

Flash titles are unaffected; they do not use EGL.

### Why some backups "install but never appear" — SOLVED

Real LFManager backups come in three shapes, and only the first has a
top-level meta.inf:

```
flat           meta.inf at the root        FOF, B10, GLB, UP, ClamPrix
self-wrapped   MIP/meta.inf                MIP
multi-package  COOK/meta.inf + lib/meta.inf   COOKING, pixar_pals
```

An installer that looks only for a top-level `meta.inf` finds nothing in the
last two and silently installs nothing — which is exactly the reported symptom
("LFManager sends it to the LeapPad2 but it never shows up as an installed
application"). The multi-package ones bundle a shared pack alongside the game:
`Base_lib` (Type=System) with COOKING, `PatchLibs` (Type=Download) with
pixar_pals.

`tools/install-game.sh` handles all three: it installs EVERY meta.inf in the
archive, strips the wrapper directory, routes by Type, and appends
ProfileAccess. All 8 backups install and all 8 appear — 15 home-screen icons
(7 base + 8 games) across multiple pages.

Still unmet: `LST3-0x00170030-000001`, required by ClamPrix, COOKING and
pixar_pals. None of the bundled packs provide it.

### Sizing the GL work

Union of EGL/GLES symbols imported by all installed games: **92** (3 `egl*`,
89 `gl*`); individual games use 28-44. GLES 1.x is fixed-function and maps
essentially 1:1 onto host Mesa, so this is the same library-impersonation
technique as `tadpole_asound.c`, just larger — NOT GPU emulation.

Note virgl is a qemu-SYSTEM virtio-gpu feature and does not apply here: Tadpole
is qemu-USER, with no kernel and no virtio bus.

**And the ALSA technique does NOT transfer directly.** `tadpole_asound.c` works
because it only writes bytes to a FIFO. A GL shim is ARM code running inside
qemu-user and CANNOT call host Mesa — there is no host-library boundary to
cross. So the shape is not "impersonate and forward"; it is "impersonate and
SERIALISE": encode each GL call into a command stream, and have the native
viewer replay it against host GL and blit the result into the framebuffer.
That is conceptually virgl, one layer up (GLES1 API instead of virtio).

The interception point is clean, which is the good news:

```
libEGL.so       -> libEGL.so.1.4        47 KB,    34 egl* entry points
libGLESv1_CM.so -> libGLESv1_CM.so.1.1   1.9 MB,  180 gl* entry points
libvr5.so                               202 KB,  hardware backend, 0 gl* exports
```

Both front-ends are ordinary versioned .so files exporting the standard APIs,
so the same SONAME-impersonation trick applies. Games import 92 of those 214,
and GLES 1.x is fixed-function — no shader compiler to reimplement.

`libvr5.so` is a genuine GPU driver: `EGL::MemoryHeap::m_hVMem` mmaps
`/dev/vmem`, and it builds command buffers (`EGL::Command::Blit`,
`EGL::VertexObject`) submitted through `/dev/ogl_vr5`. Replacing it wholesale at
the EGL/GLES boundary avoids ever having to understand that command format.

## HOW THE GL IMPLEMENTATION WORKS

Read this before the GL bug sections below. Those are a chronological record of
what went wrong; this is what the thing actually does.

### The shape of it

Native Brio apps (Camera, every Leapster game) render with OpenGL ES 1.x on
Nexell's VR5 GPU. We have no GPU, so we replace the graphics stack at the
STANDARD API boundary and rasterise in software, inside the guest.

```
  game / Brio  (ARM, inside qemu-user)
        |  eglInitialize, glDrawElements, glTexImage2D ...
        v
  runtime/shimlibs-gl/libEGL.so         <- tadpole/shim/tadpole_egl.c
  runtime/shimlibs-gl/libGLESv1_CM.so   <- tadpole/shim/tadpole_gles_core.c
        |                                  + tadpole_gles_stubs.c (generated)
        |  software rasteriser writes ARGB8888 pixels
        v
  g_back[480*272]        (our back buffer, plain memory)
        |  eglSwapBuffers -> tadpole_gl_present()
        v
  /dev/fb1  (mmap'd through tadpole_shim's fb emulation)
        |
        v
  /tmp/tadpole/fb0.bin   <- the shared arena the VIEWER maps
        |
        v
  tadpole-view composites the layers into the SDL window
```

**This all runs as ARM code inside the guest.** It cannot call host Mesa —
there is no host-library boundary to cross from in there. That is why this is a
rasteriser and not a "forward to host GL" shim, and it is why the host's GL
version, driver and distro are irrelevant to output. Every pixel is our
arithmetic: deterministic and identical on any machine.

An earlier plan was to serialise GL calls into a command stream and replay them
against host GL in the viewer (conceptually virgl, one layer up). That was
dropped: it needs a wire protocol, a host GL context, texture upload and
readback, and a blit back into the framebuffer the guest already shares with the
viewer. Rasterising in place skips all of it.

### Why replacing the stack is even possible

The stock front-ends are ordinary versioned ELFs exporting the documented APIs:

```
libEGL.so       -> libEGL.so.1.4          34 egl* entry points
libGLESv1_CM.so -> libGLESv1_CM.so.1.1   180 gl*  entry points   (1.9 MB)
libvr5.so                                 the hardware backend
```

Brio and the games between them import only 16 egl* and 89 gl*, and GLES 1.x is
FIXED-FUNCTION — a matrix stack, vertex arrays, one texture unit, alpha
blending. No shader compiler to reimplement.

Two practical consequences, both learned the hard way (see the bug sections):

* The stock libraries export non-GL symbols that other parts of the VR5 stack
  link against. We must define those too or the loader refuses to start
  AppManager at all.
* Build with `-fno-stack-protector`. uClibc 0.9.32 does not export
  `__stack_chk_guard`.

### Enabling it

OFF BY DEFAULT. `TADPOLE_GL=1 ./tadpole.sh` puts `runtime/shimlibs-gl` first on
`LD_LIBRARY_PATH`. The sysroot symlinks `/usr/lib/libEGL.so` and
`/usr/lib/libGLESv1_CM.so` must be switched TOGETHER with that flag, or
absolute-path lookups and LD_LIBRARY_PATH lookups disagree and you get two GL
stacks in one process (the same class of bug as the two libdl instances).

The build also creates `libEGL.so.1` / `libGLESv1_CM.so.1` symlinks: the loader
resolves DT_NEEDED by SONAME AS A FILENAME. libEGL depends on libGLESv1_CM (for
`tadpole_gl_present`), so GLES must link first.

### EGL (tadpole_egl.c)

Deliberately minimal. Displays, configs, surfaces and contexts are opaque
sentinel handles — callers only ever compare them against NULL and hand them
back. `eglInitialize` reports version 1.4 and succeeds, which is the single
thing that unblocks every native app (the stock EGL asserts because it cannot
open `/dev/vmem` and `/dev/ogl_vr5`).

The one call that does real work is **eglSwapBuffers**, which calls
`tadpole_gl_present()` in the GLES library to publish the frame.

### State the rasteriser keeps (tadpole_gles_core.c)

* **Matrix stacks** — separate MODELVIEW and PROJECTION, depth 16, column-major
  4x4 floats. `glMatrixMode` selects; anything that is not PROJECTION is
  treated as MODELVIEW.
* **Vertex arrays** — position, colour, texcoord. Each records pointer/offset,
  component count, type, stride, enabled flag, AND the buffer bound when it was
  set.
* **Buffer objects** — `MAX_BUFS` entries of malloc'd data. NOT optional: the
  games never pass client-side arrays for geometry, they upload to a VBO and
  pass a byte OFFSET (usually 0).
* **Textures** — `MAX_TEXS` entries, each converted to ARGB8888 at upload.
* **Fragment state** — texture enable, blend enable, blend is fixed at
  SRC_ALPHA/ONE_MINUS_SRC_ALPHA, alpha test (all eight compare functions),
  TexEnv MODULATE or REPLACE.

**Object names follow GL semantics**: textures and buffers have SEPARATE
namespaces, deleted names are reused, and binding an unused name creates the
object. The invariant `name == slot index + 1` is enforced for both — see the
names section below for why breaking it causes duplicate live objects.

**Fixed point.** Most entry points the UI and games use are the `x` variants
taking GLfixed (16.16), not float — `glOrthox`, `glTranslatex`, `glRotatex`,
`glColor4x`, `glTexEnvx`. `fx2f()` converts. This conveniently sidesteps the
guest's soft-float ABI for most of the API. Float variants exist too and share
the same implementations.

### The transform pipeline

For each vertex index, `build_vert()`:

1. Fetches x, y, (z) from the position array via `fetch()`, which resolves
   client pointer vs (buffer, offset) and converts the component type
   (GL_FLOAT, GL_FIXED, GL_SHORT, GL_UNSIGNED_BYTE, GL_BYTE).
2. Builds `MVP = projection[top] * modelview[top]` and transforms (x,y,z,1).
3. Fetches texcoords if the texcoord array is enabled, else (0,0).
4. Fetches vertex colour if the colour array is enabled, else the current
   `glColor4ub`/`glColor4x` value.
5. `to_screen()` divides by w, then maps NDC to pixels — noting GL's Y is UP
   and the framebuffer's is DOWN, so Y is flipped.

### The rasteriser

`raster_tri()` is a barycentric edge-function fill:

* Compute the signed area; skip degenerate triangles; swap two vertices if the
  winding is negative so the inside test is always "all three edge functions
  >= 0". This makes winding irrelevant, which matters because TRIANGLE_STRIP
  alternates it.
* Walk the bounding box, clamped to the screen. For each pixel, evaluate three
  edge functions; skip if any is negative.
* Normalise to barycentric weights and interpolate colour and texcoords.
* If a texture is bound and has data: sample nearest-neighbour with wrapping,
  then MODULATE (texel * vertex colour) or REPLACE.
* Alpha test, then blend against the destination if GL_BLEND is on.

Chosen over a scanline fill because it handles any winding without sorting and
makes per-pixel interpolation three multiply-adds.

Draw entry points: `glDrawArrays` and `glDrawElements` both funnel into
`draw_indexed()`, which handles GL_TRIANGLES, GL_TRIANGLE_STRIP (with
alternating winding) and GL_TRIANGLE_FAN. With an element buffer bound, the
`indices` argument is an offset into it.

### Presentation

Draws go into `g_back`, never straight to the panel. `eglSwapBuffers` blits it
to the visible framebuffer page.

This matters: Brio double/triple-buffers inside ONE framebuffer and flips with
FBIOPAN_DISPLAY. Drawing at whatever the current pan offset happens to be lets a
`glClear` and the draws following it straddle a flip and land on different
pages, so each page keeps content from two frames ago and animation smears.

### KNOWN GAPS — likely relevant to the remaining artefacts

* **NO CLIPPING AT ALL.** Vertices are projected and the triangle's bounding
  box is clamped to the screen, but there is no frustum or near-plane clipping.
  A vertex with w near zero or behind the eye projects to a wild coordinate and
  produces a huge malformed triangle. This is a plausible cause of the diagonal
  artefact in Clam Prix and should be investigated before more exotic theories.
* **No depth buffer.** `glDepthFunc`/`glDepthMask` are stubs. Clam Prix never
  enables GL_DEPTH_TEST so it does not matter there, but a 3D title would need
  it.
* **Vertex colour alpha is discarded** — `build_vert` packs colour with alpha
  forced to 1.0, so only texture alpha drives blending.
* **Nearest-neighbour sampling only**, always wrapping; `glTexParameterx`
  clamp modes are ignored.
* **Mip levels above 0 are dropped**; `glCompressedTexImage2D` is a stub.
* **No multitexturing** — one texture unit; `glActiveTexture` is a stub.
* **No lighting, fog, points or lines** — only triangles are rasterised, so a
  GL_LINES draw silently renders nothing.

### Adding an entry point

`tadpole_gles_stubs.c` is GENERATED and holds no-op stubs for everything the
guest imports but we do not implement, so the loader never sees an unresolved
symbol. To implement one: write it in `tadpole_gles_core.c`, then delete its
line from the stubs file (duplicate definitions will not link).

### Diagnostics

* `TADPOLE_GL_DEBUG=1` — trace the call sequence to stderr.
* `TADPOLE_GL_MAXDRAW=N` — ignore every draw after the Nth in a frame; the
  counter resets on glClear. Combine with `tools/fbshot.py` to bisect a frame
  down to the single draw that introduces an artefact.

When adding a probe, GATE IT PER FRAME (reset on glClear) rather than on a
global counter, or it will sample startup and tell you nothing about the frame
you are looking at. That mistake has been made here twice.

## GL shim — milestone 1 attempted, NOT yet usable

`tadpole/shim/tadpole_egl.c` (16 EGL entry points, real behaviour) and
`tadpole/shim/tadpole_gles.c` (89 GLES1 stubs, GENERATED) build into
`runtime/shimlibs-gl/`. Symbol coverage is complete: 16/16 egl*, 89/89 gl*.

**OFF BY DEFAULT.** Enable with `TADPOLE_GL=1 ./tadpole.sh`. Do not put these on
the default library path.

**Why: enabling them blanks the home screen.** `libLightning2D.so` imports EGL,
so the Flash/Lightning UI composites through GL too — not just the 3D games. A
GL stack whose gl* calls do nothing therefore draws nothing ANYWHERE, and the
UI that already worked goes black. Milestone 1 as designed ("let the app run,
draw nothing") is not reachable by stubbing, because the thing that has to keep
working is itself a GL client.

That is the real constraint on this work, and it was not obvious up front: the
shim cannot be a no-op placeholder that gets filled in later. It has to actually
RENDER from the first usable version, or it regresses the parts that already
work. The command-stream design still stands, but there is no cheap intermediate
step.

Ordering note: `/usr/lib/libEGL.so` and `/usr/lib/libGLESv1_CM.so` in the
sysroot must point at the STOCK libraries while the shim is off, for the same
absolute-path reason as libdl (see "Native Brio apps"). Point them at
`runtime/shimlibs-gl/` only when enabling the shim, or the two paths disagree.

### The GL subset is small, fixed-point, and 2D — revise the plan

The UI/compositor path (`libLightning2D`, `libLightningUI`, `libDisplayMPI`)
needs only **40** gl* calls, and all 40 are a subset of the 89 the games use.
So ONE implementation covers both; get the UI rendering and the games are
mostly already served.

```
matrix   glMatrixMode glLoadIdentity glPush/PopMatrix glOrthox glTranslatex
         glRotatex glScalex
arrays   glVertexPointer glColorPointer glTexCoordPointer
         glEnable/DisableClientState
draw     glDrawArrays glDrawElements glClear glClearColorx
texture  glGenTextures glBindTexture glTexImage2D glTexParameterx glTexEnvx
         glCompressedTexImage2D glDeleteTextures
buffers  glGenBuffers glBindBuffer glBufferData glBufferSubData glDeleteBuffers
state    glEnable glDisable glBlendFunc glDepthFunc glAlphaFuncx glShadeModel
query    glGetError glGetIntegerv
misc     glColor4ub glLineWidthx glPointSizex
```

Two facts that make this much easier than "emulate a GPU":

* **The `x` suffix means GLfixed (16.16), not float.** The UI path is almost
  entirely fixed-point, which sidesteps the guest's soft-float ABI for those
  entry points entirely — they take plain 32-bit integers.
* **No shaders.** GLES 1.x is fixed-function: a matrix stack, vertex arrays,
  one texture unit, alpha blending. This is a 1990s rasteriser, not a modern
  pipeline.

**REVISED RECOMMENDATION: rasterise in the guest, do not serialise to the
viewer.** The earlier suggestion (encode a command stream, replay against host
GL in the viewer) is more work and a worse fit:

* it needs a wire protocol, a host GL context, texture upload and readback;
* the viewer would have to blit GL output back into the shared framebuffer,
  which is a second copy of a path that already works.

A software rasteriser living inside the ARM shim writes STRAIGHT into the
framebuffer the shim already owns — the same memory the viewer already maps and
displays. No protocol, no readback, no new moving parts. The panel is 480x272,
which is tiny, and the guest already runs everything else under qemu at
acceptable speed.

Start with: clear, matrix stack, `glDrawArrays`/`glDrawElements` over
`GL_TRIANGLES`/`GL_TRIANGLE_STRIP` with `glVertexPointer` + `glColorPointer`,
then textures (`glTexImage2D` + `glTexCoordPointer`), then alpha blending. The
UI rendering is the acceptance test — if the home screen draws through the
shim, the foundation is right.

### GL rasteriser — WORKING: native games render geometry

`TADPOLE_GL=1 ./tadpole.sh` enables it. The home screen still renders 15 icons
with it active (always re-check that; the UI is a GL client too), and Ben 10
draws real geometry inside its ViewFrame. `shots/30-first-gl-render.png`.

Four distinct bugs stood between "written" and "working". Each looked like a
rendering failure and none of them was.

**1. Missing `-fno-stack-protector`.** Every other shim rule had it; the two GL
rules did not. clang emitted `__stack_chk_guard` references, uClibc 0.9.32 does
not export it, and the loader refused to start AppManager at all:

```
AppManager: symbol '__stack_chk_guard': can't resolve symbol
```

Four lines of log, and the whole "reaches no Flash state" mystery. When a shim
makes the guest fail to start, read the FIRST lines of the log, not the last.

**2. The stock GL libraries export non-GL symbols.** `libGLESv1_CM.so.1.1` is a
1.9 MB blob exporting 697 symbols, only 180 of them GL. Replacing it removed
seven that other parts of the VR5 stack link against:

```
libGLESv1_CM: DataConvert_to_enumv, DataConvert_to_intv,
              g_shader_es1_fs, g_shader_es1_vs
libEGL:       __vr5_set_swap_buffer_callback, __global_ftn_vg_dispatch_table,
              _ZN3EGL11g_pProcListE
```

Only libvr5.so and libGLES.so import them — dead code once our entry points
win — so they merely need to RESOLVE. They are defined as inert placeholders
with matching sizes. NOTE: an earlier attempt to CHAIN to the real library
instead was abandoned: it pulls in 101 more unmet symbols (EGL::FrameBuffer,
EGL::TextureLevel, EGL::ProgramContainer) because libGLESv1_CM, libEGL and
libvr5 share C++ internals. Chaining means shipping the GPU driver.

Also beware: when hunting "which symbols must I provide", do NOT filter out
underscore-prefixed names as linker artifacts. That is how
`__vr5_set_swap_buffer_callback` was missed on the first pass.

**3. qemu-user does not path-translate `chdir`.** `-L` rewrites paths for
open/stat and friends, but `TARGET_NR_chdir` passes the guest string straight
to the host. Leapster games chdir into their own package directory and then
open assets relatively:

```
open ./res/Sound/Ben10.soundproject failed: No such file or directory
 -> SoundProject::loadFromFile() gets no document
 -> TiXmlNode::FirstChildElement() dereferences NULL -> SIGSEGV
```

The file was present and correct; only the working directory was wrong. The
shim now intercepts `chdir` and tries the sysroot-relative path first. Same
family as 4.2 — assume nothing about which syscalls `-L` covers.

**4. The games render entirely through VERTEX BUFFER OBJECTS.** They never pass
client-side arrays. They upload to a VBO and call `glVertexPointer` with a byte
OFFSET, almost always 0 — which a naive implementation reads as a NULL array and
skips:

```
glBindBuffer GL_ARRAY_BUFFER 2 ; glBufferData 48000 bytes
glVertexPointer(2, GL_FIXED, stride, (void*)0)
draw guard: vtx.on=1 ptr=0        <- 31,768 draws silently discarded
```

An array reference is therefore (buffer, offset) when a buffer is bound and a
plain pointer when not; both forms are supported. Same for `glDrawElements`
indices with GL_ELEMENT_ARRAY_BUFFER bound. Buffer objects are NOT an optional
extra to add later — nothing draws without them.

Note the vertex format in use: `size=2, type=GL_FIXED` — 2D, fixed-point.

### Still to do

Geometry renders but is untextured (white/black), because `glTexImage2D` and
`glTexCoordPointer` are still stubs. Next steps, in order:

 1. Textures: store levels from glTexImage2D, sample per-pixel using the
    barycentric-interpolated texcoords already computed.
 2. Alpha blending (glBlendFunc) — needed before anything looks right.
 3. Depth buffer, if games turn out to need it; 2D titles may not.

Diagnostics: `TADPOLE_GL_DEBUG=1` traces the call sequence, including the draw
guard and the first triangles in screen coordinates. That trace is what found
bugs 3 and 4.

### Historical: the first attempt (superseded by the section above)

`shim/tadpole_gles_core.c` implements 31 entry points for real — framebuffer
mmap of /dev/fb1, 4x4 matrix stack (fixed AND float variants), glClear, vertex
and colour arrays, and a barycentric triangle rasteriser handling TRIANGLES /
TRIANGLE_STRIP / TRIANGLE_FAN with per-pixel colour interpolation. libm is not
available to a -nostdlib shared object, so sin/cos and sqrt are done with a
Bhaskara approximation and Newton iteration. `tadpole_gles_stubs.c` (GENERATED)
covers the remaining 61. Coverage is complete: 89/89.

**It builds and does not crash, but with `TADPOLE_GL=1` the guest reaches NO
Flash state at all** — not even SignIn, where without the shim it reaches the
home screen and draws 15 icons. So the failure is EARLIER than rendering,
during Brio/Lightning display init. No SIGSEGV, no assert in the log; it simply
stops.

Next session, in this order:
 1. `TADPOLE_GL=1 TADPOLE_STRACE=1` and find the last syscall before it stalls.
 2. Suspect `eglCreateWindowSurface` / `eglMakeCurrent` returning handles Brio
    then dereferences, and `glGetIntegerv` returning 0 for sizes the caller
    divides by or allocates from — a 0 there is a plausible hang or silent
    bail. Returning honest values for GL_MAX_TEXTURE_SIZE and the viewport
    query is cheap and worth trying first.
 3. Only then look at the rasteriser; nothing has reached it yet.

Stock GL remains on the absolute-path route (`/usr/lib/libEGL.so`,
`/usr/lib/libGLESv1_CM.so` -> rootfs). Switch BOTH those symlinks and set
TADPOLE_GL=1 together, or the two lookup routes disagree — same trap as libdl.

### Clam Prix: textured, plus what the remaining artefact is NOT

`shots/31-clamprix-textured.png` — SpongeBob: The Clam Prix renders its main
menu: Bikini Bottom seafloor, coral, water, and the game's own bamboo ViewFrame.
Textures and alpha blending both working. No crashes.

Implemented this round, each verified by tracing BEFORE writing code:

* **Textures.** `glTexImage2D` converting the three formats Clam Prix actually
  uploads — measured, not assumed: `GL_UNSIGNED_BYTE` (19 uploads),
  `GL_UNSIGNED_SHORT_4_4_4_4` (27), `GL_UNSIGNED_SHORT_5_6_5` (2), all GL_RGBA,
  no compressed. Converted once to ARGB8888 at upload so sampling has one path.
* **Per-pixel sampling** with barycentric texcoords, MODULATE against vertex
  colour (the GLES1 default TexEnv).
* **Alpha blending** — `glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA)` is
  what the game asks for, 255 times. Plus a zero-alpha early-out.
* `glLoadMatrixx`/`glMultMatrixx`/`glLoadMatrixf`/`glMultMatrixf`, `glColor4x`.

**UNSOLVED: streaky triangles across the upper half of the game view.** The
lower half (background) is correct. Do not re-tread these — all measured and
ruled out:

| hypothesis | how it died |
|---|---|
| no depth buffer | `GL_DEPTH_TEST` disabled 151x, NEVER enabled — the game is 2D painter's-order |
| matrix-palette skinning | `glMatrixIndexPointerOES`/`glWeightPointerOES`/`glCurrentPaletteMatrixOES` linked but NEVER called |
| `glLoadMatrixx` stubbed | implemented it; the game never calls it either |
| VBO vs client-array confusion | game unbinds correctly (645x `glBindBuffer(...,0)`); `boundbuf` is 1 for VBO draws and 0 for client draws |
| viewport ignored | `glViewport` never called |
| texture matrix corrupting modelview | only MODELVIEW (480) and PROJECTION (961) modes used, never `GL_TEXTURE` |
| matrix stack overflow | 0 dropped pushes, 0 underflows, max depth 1 |
| multitexturing | `glActiveTexture`/`glClientActiveTexture` never called |
| bad vertex coordinates | all 180 sampled triangles inside the 480x272 panel |
| bad texcoords | sampled UVs are exactly 0.0/1.0 full-quad values |

So geometry, transforms and texcoords are all sound where measured, and the
artefact still appears. The measurements sampled the FIRST triangles and a
window at 400-460, both of which look correct — the bad draws are elsewhere in
the stream and have not been isolated yet.

Next things to try, roughly in order of cost:
 1. Dump every triangle for one frame with its bound texture id, then correlate
    the streak colours against which texture is bound.
 2. `glTexEnvx` is still a stub — if the game selects REPLACE or ADD instead of
    MODULATE for some draws, colours would be wrong (though not smeared).
 3. `glAlphaFunc`/`glAlphaFuncx` are stubs and `GL_ALPHA_TEST` is enabled once;
    an unimplemented alpha test would draw texels that should be discarded.
 4. Render one draw call at a time to a PNG (fbshot) and bisect which call
    introduces the streaks.

Testing note: the other seven games are parked in `/tmp/tadpole-games-held` so
Clam Prix sits at a fixed tile position — fb(160,130), `probe-launch.sh game`.
Move them back into `LF/Bulk/ProgramFiles/` to restore the full library.

### Double buffering — fixed the smearing (partially)

Owner's observation cracked this one: "textures start spinning and fly up into
the air, leaving those weird lines behind". LINES BEHIND = a trail, and a trail
means the surface being drawn is not the surface being cleared.

The rasteriser was drawing at whatever the CURRENT pan offset happened to be
(`FBIOGET_VSCREENINFO`). Brio double/triple-buffers inside one framebuffer and
flips with `FBIOPAN_DISPLAY`, so a `glClear` and the draws following it can
straddle a flip and land on different pages. Each page then keeps content from
two frames ago, and an animation accumulates instead of replacing — exactly the
reported smear.

Fix: render into our own back buffer (`g_back`, 480x272 ARGB) and blit it to the
visible page from `eglSwapBuffers`. One surface, cleared and drawn consistently,
presented atomically. `tadpole_gl_present()` is exported from libGLESv1_CM and
called by the EGL shim, so libEGL now has a DT_NEEDED on libGLESv1_CM.so.1.

NOTE: the loader resolves DT_NEEDED by SONAME AS A FILENAME, so the build now
creates `libGLESv1_CM.so.1` and `libEGL.so.1` symlinks alongside the `.so`
files. Without them: `can't load library 'libGLESv1_CM.so.1'`. Build order also
matters now — GLES must link before EGL.

Result (`shots/33-clamprix-doublebuffered.png`): noticeably more of the frame is
correct, and content previously buried under the smear is now visible —
SpongeBob on his kart, the yellow PLAY text, clams on the seafloor.

**Streaks still present in the upper area.** Double buffering was a real bug and
a real fix, but not the whole story. Remaining suspects, given everything in the
elimination table above is already excluded:

 1. `glTexEnvx` is a stub — if some draws select REPLACE/ADD instead of the
    default MODULATE, colours will be wrong.
 2. `glAlphaFunc`/`glAlphaFuncx` are stubs and GL_ALPHA_TEST is enabled once;
    texels that should be discarded are being drawn.
 3. Draw modes other than TRIANGLES/STRIP/FAN (GL_LINES, GL_POINTS) are
    silently ignored by draw_indexed — that would LOSE content rather than
    smear it, but worth confirming which modes actually appear.
 4. Stale `g_tex.buf`: glTexCoordPointer captures the bound ARRAY_BUFFER at
    call time, same as real GL. If the game sets texcoords while a VBO is bound
    and then draws with a client-side vertex pointer, UVs would come from the
    VBO. Worth logging g_vtx.buf and g_tex.buf together at draw time.

### Host GL is NOT involved — worth knowing before debugging

Reasonable question: could a modern Arch/Mesa GL stack behave differently from a
2011 tablet? No. The rasteriser is SOFTWARE, running inside the guest as ARM
code; it reads vertices, does the arithmetic, and writes pixels into the
framebuffer. Mesa, the host GPU driver and the host GL version never touch the
game's rendering. SDL only uploads the finished framebuffer as one texture and
draws a quad.

So every artefact is our arithmetic: deterministic, reproducible, and identical
on any host. Do not go looking for driver-specific behaviour.

### Alpha test and TexEnv implemented; streaks persist

Measured, then implemented:
* `glAlphaFunc(GL_GREATER, ref)` — the game enables GL_ALPHA_TEST, so texels
  below the reference must be DISCARDED. All eight compare functions supported.
* `glTexEnvx(GL_TEXTURE_ENV_MODE)` — MODULATE (66 calls) and REPLACE honoured.
  NOTE the game also passes values that are NOT valid modes (0, and once a
  pointer-looking value); those are ignored rather than latched onto.

Neither fixed the streaks. Also fixed while reading the code: `glDrawArrays`
offset the vertex and colour arrays by `first` but not the TEXCOORD array —
latent today because every observed call passes first=0, wrong the moment one
does not.

Further eliminated by measurement this round:
* draw modes are only GL_TRIANGLES (4) and GL_TRIANGLE_FAN (6) — nothing is
  being silently dropped by draw_indexed;
* `g_vtx.buf` and `g_tex.buf` are correctly paired at draw time (0/0 for
  client-side draws, 1/2 for VBO draws).

### A caution about claiming visual fixes

I stated that double buffering revealed content "previously buried" (SpongeBob's
kart). That was NOT verified. Comparing the before/after screenshots shows 20%
of the frame differs — but THE MENU IS ANIMATED, so two captures at different
moments differ regardless. The comparison cannot support the claim.

Double buffering is still correct on its own terms (clear and draw can no longer
straddle a page flip), but do not attribute specific visual changes to it
without a controlled comparison. Screenshots of an animated scene are not a
diff.

### Next step: bisect, do not theorise

Nine hypotheses have now died to measurement. Stop generating candidates and
isolate the offending draw instead:

 1. Add a TADPOLE_GL_MAXDRAW=N env var that makes draw_indexed ignore every
    call after the Nth.
 2. Binary-search N with tools/fbshot.py until the streaks appear.
 3. Dump that draw's mode, count, buffers, matrices, texture id and vertices.

That converts an open-ended search into about a dozen runs.

## Clam Prix vs REAL HARDWARE — reference captures

`shots/spongebob run/real/*.jpeg` are photographs of the game on a working
LeapPad2; `shots/spongebob run/emulated/*.png` are our captures of the same
screens. Matched pairs for mainmenu, options, controls, credits, pause,
race_start (pick char / pick track), ingame and ingame_prerace. THIS IS GROUND
TRUTH — diff against it rather than guessing what a screen should look like.

What the main-menu pair shows:

* Real: six clean white circular "bubble" buttons with icons, angled yellow
  "Play" text, SpongeBob on his kart, bamboo frame.
* Ours: the bubbles render as CRESCENTS (partial arcs), and a clean DIAGONAL
  runs from upper-left to lower-right — below it the image is correct
  (SpongeBob, kart, seafloor, flowers), above it is garbage.

A clean diagonal across a rectangle is a TRIANGLE EDGE, i.e. one large triangle
drawn with bad texture coordinates painting over the top half — a single bad
draw, not a systemic sampling error.

### TADPOLE_GL_MAXDRAW=N — draw-level bisection

Implemented. Ignores every draw after the Nth; the counter resets on glClear,
so N is per-frame. Combine with `tools/fbshot.py` to binary-search the frame.

First result: **N=1 paints the whole game area SOLID WHITE**
(`shots/35-draw1-solid-white.png`). That is the "solid white" the owner has been
reporting since the beginning. Draw 1 is the background quad, and white is what
the sampler produces when it finds no texture data and falls through to the
default vertex colour (opaque white).

### GL object names — real bugs found and fixed

Two genuine deviations from GL semantics, both fixed:

 1. **Textures and buffers shared one counter.** GL gives them SEPARATE
    namespaces — texture 1 and buffer 1 are different objects.
 2. **Deleted names were never reused.** GL hands back the lowest unused name.
    The game's texture manager constantly loads and releases (its own log shows
    `GLTextureManager Release Texture ... Load Texture ...`), so our numbering
    drifted from the game's.

Also added: binding an unused name now CREATES the object, as GL does.

Invariant now enforced: **name == slot index + 1** for both textures and
buffers. Without it, `tex_slot()` dropping an arbitrary name into the first free
slot lets `glGenTextures` later hand out a name already in use — two live
objects with the same name. That bug was introduced and fixed within this
session; do not "simplify" the allocator back to first-free.

### CAUTION: a diagnostic that measured the wrong moment

The trace answering "does the bound texture have data at draw time?" was gated
on `g_tri_logged < 3` — a GLOBAL counter. It therefore only ever sampled the
first three triangles of the whole run, during startup, long before the
background texture is uploaded. It reported "no data" and that was true, but
irrelevant to the frame being looked at.

Confirmed by tracing the lifecycle: texture 22 is deleted and then re-uploaded
at 320x240 (the background), so it DOES have data by the time the menu renders.

Re-gate any such probe PER FRAME (reset on glClear, like g_draw_no) before
trusting it.

### Next step

 1. Re-gate the texture/state probe per frame, then re-run MAXDRAW=1 and ask
    what the background draw actually binds and samples in the RENDERED frame.
 2. Walk N upward (1, 2, 3, ...) capturing each frame until the diagonal
    appears; that draw is the culprit.
 3. Dump that draw's matrices, texcoords and texture id in full.

## Framebuffer roles (community intel, consistent with observation)

```
fb0  2D
fb1  3D
fb2  video playback / FMV
```

Consistent evidence: the Sneak Peeks app plays video AUDIO but shows no picture,
which is what you would expect if nothing handles the fb2 video layer. Our GL
rasteriser writes to **fb1**, which is the right layer for 3D.

## Culling is NOT the cause — measured, not argued

The most plausible outside suggestion was a winding-order/back-face culling
problem: 2D quads built from two triangles, one of which gets rejected. That
would explain "textures cut off diagonally" and circular buttons rendering as
crescents.

Instrumented directly — count triangles SUBMITTED to raster_tri versus triangles
that painted at least one pixel:

```
submitted: 292   painted: 292   rejected: 0
frames where submitted > painted: 0
zero-area rejects: 0
```

**Nothing is ever culled.** raster_tri normalises winding by swapping two
vertices when the signed area is negative, so orientation cannot reject a
triangle. Do not revisit this.

### The same measurement raises a bigger question

292 triangles across 297 frames is roughly ONE TRIANGLE PER FRAME. That is
nowhere near enough to draw a menu containing bubbles, text, a character and a
background — so most of what is on screen is NOT coming from our rasteriser.

Implication: the streaks and crescents may not be our output at all. They could
be another layer (fb0 2D, or fb2 video) or stale fb1 content that nothing
clears. Before debugging the rasteriser further, ESTABLISH WHICH PIXELS ARE
OURS.

Cheap decisive test: make raster_tri write a fixed, unmistakable colour (e.g.
magenta) instead of the sampled texel, run, and screenshot. Every pixel that
changes is ours; everything else belongs to another layer and no amount of
rasteriser work will fix it.

This also fits the owner's report that "3D doesn't really seem to render at all
(the race levels are 3D)" — if only a couple of triangles per frame reach us,
the 3D pipeline is barely being exercised yet.

### TADPOLE_GL_TINT=1 — which pixels are OURS

Paints a fixed magenta instead of the shaded texel. Result
(`shots/36-gl-tint-test.png`):

```
magenta = 76800 px = EXACTLY 320x240 = 58.8% of the 480x272 frame
```

Two hard facts:

1. **Our rasteriser owns exactly the 320x240 game viewport.** The bamboo
   ViewFrame, the A/B/L/R buttons and the surrounding art are a DIFFERENT LAYER
   (fb0, 2D). No amount of rasteriser work will change them, and any artefact
   there is not ours.
2. **The whole viewport is a single flat fill.** Per frame the game issues
   essentially one full-screen quad through GL and almost nothing else — which
   matches the measured ~1-2 triangles per frame.

So the menu's bubbles, characters and text are NOT being drawn through our GL
per frame. Combined with the frame counts (146 frames drawing 2 triangles, 151
drawing none), the likeliest explanation for the streaks and crescents is
ACCUMULATION: `g_back` persists between frames and is only reset when the game
calls glClear with GL_COLOR_BUFFER_BIT, so partial content from many frames
piles up.

Next diagnostics, in order:

 1. Log the glClear mask per frame. If COLOR_BUFFER_BIT is often absent, the
    game expects the surface to persist and something else (that we are not
    doing) refreshes it.
 2. Force `g_back` to be cleared at every eglSwapBuffers and see whether the
    streaks vanish and the menu simply goes mostly empty. That distinguishes
    "we accumulate rubbish" from "we are missing draws".
 3. If draws really are missing, find where the menu elements go instead — they
    may be rendered by the game into a texture, or drawn to the 2D layer
    through a path we do not intercept.

## GL debugging session: tooling fixed, seven more hypotheses eliminated

### TWO OF MY OWN INSTRUMENTS WERE SILENTLY BROKEN — read this first

Both were python string-replace patches whose anchor text did not match. The
edit reported nothing, the file built, and the tool quietly did the wrong thing.

1. **`g_tint` was stuck ON.** Declared `static int g_tint = -1` and used as
   `g_tint ? MAGENTA : shaded`. The initialisation was never inserted, and
   **-1 is TRUTHY in C**, so the debug tint was permanently enabled. Every
   dumped frame came out uniformly magenta.
2. **`TADPOLE_GL_MAXDRAW` was never implemented.** Only the declaration and the
   per-frame reset existed; the `if (++g_draw_no > g_max_draw) return;` was
   missing entirely. The limiter did nothing.

**This invalidated a reported finding**: "MAXDRAW=1 paints the game area solid
white, therefore draw 1 is the bug". MAXDRAW was inert, so that was simply an
ordinary frame captured mid-transition. Retracted.

**ALWAYS VERIFY A PATCH APPLIED.** Every patch in this file's tooling should
assert its anchor exists before replacing, and grep for the result afterwards.

### Working diagnostics (all verified)

* `TADPOLE_GL_DUMPFRAME=1` — writes OUR back buffer at every present to
  `$TADPOLE_DIR/frame_NNN.raw` (480x272 ARGB8888, 8-deep ring). This is the
  rasteriser's exact output: no compositing, no other layers, no pan ambiguity.
  **The single most useful tool here** — use it before reasoning about the
  window, which shows three composited framebuffers.
* `TADPOLE_GL_DUMPTEX=1` — writes every uploaded texture to
  `$TADPOLE_DIR/tex_<id>_<w>x<h>.raw`.
* `TADPOLE_GL_MAXDRAW=N` — now genuinely limits draws per clear interval.
  Verified: N=1 gives ~10.9k distinct colours vs ~20.7k unlimited.
* `TADPOLE_GL_TINT=1` — paint a flat magenta instead of the shaded texel, to
  see which pixels are ours.
* `TADPOLE_GL_NOCLIP=1` — disable the near-plane rejection.

### What is definitively CORRECT

* **Draw 1 renders a pixel-perfect Clam Prix title screen** — logo, SpongeBob,
  Patrick, Squidward, Gary on karts, no artefacts (`shots/verify-maxdraw1.png`).
  Rasteriser, texture sampling, MODULATE and blending all work.
* **All 46 uploaded textures are perfect** — see `shots/38-texture-atlas.png`:
  clean circular bubble buttons with icons, "Play" / "Tune Up Garage" text,
  characters, backgrounds. Format conversion (RGBA8888 / RGBA4444 / RGB565) is
  correct. The high-contrast ones are font atlases, not corruption.
* **The menu is composed of FULL-SCREEN TEXTURED QUADS** — 2 triangles each,
  which is why ~290 triangles per run draws an entire animated menu.
* **Our rasteriser owns exactly the 320x240 game viewport.** The bamboo
  ViewFrame and A/B/L/R buttons are a different layer (fb0).

### Eliminated by measurement (do not revisit)

| hypothesis | measurement |
|---|---|
| back-face culling / winding | 294 triangles submitted, 294 painted, **0 rejected** |
| degenerate (zero-area) triangles | 0 |
| off-panel geometry | 0 triangles reach outside [-200,700]x[-200,500] |
| long thin slivers | 0 |
| near-plane / w blowup | 0 triangles rejected with w < 1/1024 |
| texture corruption | all 46 dumped and visually perfect |
| texture coordinates | **0** triangles with any UV outside 0..1 |
| glClear not clearing | 294 clears performed, 0 skipped for either reason |
| render-to-texture / FBO | the game imports no such entry point |
| vertex alpha discarded | was real, now fixed — did not change the artefact |

### Fixed this session

* **Per-vertex alpha** is now read from the colour array instead of being forced
  opaque (`fetch()` returns 1.0 for a missing 4th component, so 3-component
  arrays still come out opaque). A genuine bug; not the cause of the streaks.
* **Near-plane rejection** added as a crude stand-in for clipping. Measures 0
  rejections here, so it changes nothing today, but a 3D title will need REAL
  clipping that splits triangles rather than discarding them.
* GL object-name semantics (separate namespaces, name reuse, implicit creation
  on bind, `name == slot index + 1`).

### Where this leaves the artefact

Every measurable INPUT to the rasteriser is correct: geometry, texcoords,
textures, winding, clears, alpha. The OUTPUT is still wrong. That combination
says the bug is in something not yet measured — most likely the SEQUENCE: which
texture is bound when a given quad is drawn, or state leaking between draws.

Next step, and it is mechanical: log a complete per-draw record for ONE frame —
draw index, mode, count, bound texture id, the three screen positions and the
three UVs — then reconstruct that frame by hand from the log and compare against
`shots/spongebob run/real/mainmenu.jpeg`. The draw that does not belong will be
obvious, and every quantity needed to explain it will already be in the record.

## SOLVED: the element-buffer offset-zero bug

**One line.** `draw_indexed()` resolved the index buffer like this:

```c
if (indices && g_bound_elem) {          /* WRONG */
    indices = eb->data + (u32)(unsigned long)indices;
}
#define IDX(n) (indices ? read(indices, n) : (u32)(n))
```

With a GL_ELEMENT_ARRAY_BUFFER bound, `indices` is a BYTE OFFSET into that
buffer — and the offset is almost always ZERO, which arrives as a NULL pointer.
Testing `indices &&` therefore skipped the resolution for every normal draw,
leaving `indices` NULL, so `IDX(n)` fell back to sequential 0,1,2,3,...

For a quad (4 vertices, 6 indices) that means:

```
triangle 1 = vertices 0,1,2   <- accidentally CORRECT
triangle 2 = vertices 3,4,5   <- 4 and 5 are PAST THE END of the buffer
```

So exactly one triangle of every quad degenerated. Backgrounds rendered as a
diagonal half, circular buttons rendered as crescents, and the garbage read past
the vertex buffer produced the streaks.

Fix: gate on the BINDING, never on the pointer value.

```c
if (g_bound_elem) { ... }
```

**This is the SAME trap already fixed for `glVertexPointer`** — offset 0 looks
like NULL — and it was reproduced verbatim for indices a few hours later. If a
GL entry point takes a pointer that doubles as a buffer offset, the binding is
the only safe thing to test.

Result: `shots/43-clamprix-FIXED.png` — the Clam Prix main menu renders
correctly. Full circular bubble buttons with icons, "Play", SpongeBob on his
kart, the Bikini Bottom background. Matches `shots/spongebob run/real/mainmenu.jpeg`.
Home screen still 8 icons, no crashes.

### Why it took so long, and what actually found it

Every hypothesis that died did so because the measurement was of an INPUT
(geometry, texcoords, textures, winding, clears, alpha) and every input was
genuinely fine. The defect was in how indices were READ, which no input check
covers.

Two things cracked it:

1. **TADPOLE_GL_CLEARSWAP=1.** Clearing the back buffer after each present
   removed accumulated leftovers, and the underlying defect — half of every
   quad missing along a diagonal — became obvious instead of being buried under
   streaks. When an artefact looks like noise, first remove anything that can
   persist between frames.
2. **Logging BOTH triangles of a draw**, not just the first. The first triangle
   of every quad was correct, which is why sampling "the first N triangles"
   showed nothing wrong for hours. `tri 2: vA(480,0) vB(0,19785) vC(0,19788)`
   made it immediate.

The owner's own description — "2D textures get cut off diagonally" and circular
buttons rendering as crescents — described a missing triangle per quad exactly,
and predated every measurement here.

## SOLVED: the game must render into its ViewFrame window, not the whole panel

Symptom, reported by the owner as soon as the menu rendered correctly: *"I don't
think the game is scaled correctly. It is supposed to fit in that box with the
Leapster emulated controls around it... some of the menu elements get cut off."*

Exactly right. `shots/43-clamprix-FIXED.png` fills all 480x272, so the bubbles on
the right run off the edge and the ViewFrame art is completely hidden. Compare
`shots/46-viewport-auto.png` — the fix — against
`shots/spongebob run/real/mainmenu.jpeg`.

### A Leapster title does NOT own the screen

AppManager draws a **ViewFrame** on fb0 — bamboo border, `= ↑ ?` buttons, A/B,
L/R — and gives the game a smaller window on fb1. The rect comes from
`/LF/Base/EmeraldTitles/<PackageID>/ViewFrame.json`:

```json
{ "ViewFrameConfigs": { "Default": {
    "png": "SB_racing.png", "x": 15, "y": 17, "w": 320, "h": 240 } } }
```

The hardware MLC composites the layer at that rectangle. We have no MLC, so the
rasteriser has to do it.

**It varies per title — never hardcode it.** Across the 26 stock ViewFrames:

| shape | titles | rect |
|---|---|---|
| Leapster games | most | 320x240 at ~(16,16) |
| reading titles | UP, PetPals, JediReading, PrincessFrog, GetPuzzled, Digging4Dinos | **250x250 at ~(76,11)** |

Clam Prix is the odd one at x=15,y=17.

### Where the rect actually comes from at runtime

Not from parsing the JSON. The guest pushes the window down to the fb driver,
and the shim already sits on those ioctls. Two of them, both on **fb1**:

```
[tadpole] fb1 PUTVAR req 320x240 virt 480x2176      <- FBIOPUT_VSCREENINFO
[tadpole] fb1 posioctl 40046d03: f 11 <ptr> 14f 101 <- LF1000FB_IOCSPOSTION
                                 ^  ^        ^   ^-- bottom 257 = 17+240
                                 |  |        `----- right  335 = 15+320
                                 |  `-------------- top     17
                                 `----------------- left    15
```

* **size** ← the *requested* `xres`/`yres` on `FBIOPUT_VSCREENINFO`. Must be read
  **before** `fill_var()` replaces it with the panel size — that overwrite is
  why `state.bin` used to record fb1 as 480x272 and the size looked unavailable.
* **origin** ← words 0 and 1 of the `IOCSPOSTION` payload.

Word 2 holds a pointer, so the payload is *not* the flat `{left,top,right,bottom}`
the name suggests. Words 3 and 4 do read as right/bottom and agree with the JSON
on all five samples captured, but the size is taken from `PUT_VSCREENINFO`
anyway, whose meaning is unambiguous. Verified against fb0 too: `0 0 1 1e0 110`
= the full 480x272 panel at the origin.

Dead ends, so nobody repeats them:

* **The EGL native window is useless for this.** `eglCreateWindowSurface`'s
  window handle begins `480 272` — the panel, not the title's window.
* **`SetWindowPosition` in Brio's log has no offset**: it prints
  `0,0 .. 320,240`. The (15,17) exists only in the JSON and in the ioctl.

### How it is wired

`struct layer_state` in `tadpole_shim.c` gained `win_x, win_y, win_w, win_h`,
published in `state.bin`; the GL core mmaps that file and re-reads layer 1 once
per frame from `tadpole_gl_present()`. Once per frame matters: the rect is set
while the title loads, which races the first draw, and it reverts to full-panel
when the app exits and the Flash UI takes fb1 back.

In `tadpole_gles_core.c` the rect drives three things — NDC→pixel mapping in
`to_screen()`, the raster bounding box clamp, and `glClear`. Everything outside
stays untouched, which is what lets fb0's ViewFrame show through. Confirmed by
`TADPOLE_GL_DUMPFRAME`: ~17,000 distinct colours inside the window, **exactly
one (zero) outside it**.

`TADPOLE_GL_VIEW="x,y,w,h"` overrides it for bisection.

### THREE COPIES OF struct layer_state MUST AGREE

`tadpole_shim.c`, `tadpole_view.c`, and **`tools/fbshot.py`** each declare the
layout independently. Adding four fields changed the stride from 36 to 52 bytes
and `fbshot.py` still used `20 + i*36` with `<9I`, so it read every layer past
the first from the wrong offset and captured a blank screen — while the
rasteriser was working perfectly the whole time. `fbshot.py` now derives its
stride from a single `LAYER_FIELDS` tuple and prints a loud warning when
`state.bin` is not the size it expects.

### Two self-inflicted detours worth recognising

1. **`fbshot.py` crashed after the fix and the failure was invisible.**
   `probe-launch.sh` runs it as `fbshot.py ... >/dev/null 2>&1 || true`, so a
   traceback left the *previous* `after.png` in place and every capture looked
   unchanged. When a capture looks stale, run the capture tool by hand first.
2. **`fbshot.py` with no `-d` reads `$TADPOLE_DIR`, defaulting to `/tmp/tadpole`.**
   Running it manually outside `probe-launch.sh` silently inspected a stale
   directory from an older session and reported a 128-byte `state.bin` that no
   current build could produce. Two dead-end theories came from that; both
   evaporated once the env var was passed.

Also: `pkill -f "TADPOLE_DIR=$D"` **kills the shell that runs it**, because that
string is in its own command line. `probe-launch.sh` guards with
`[ "$pid" = "$$" ] && continue`; ad-hoc one-liners must do the same.

## SOLVED: white textures — glCompressedTexImage2D was resolving to the dead driver

Symptom: *"a lot of textures flat out just don't render. they render as white
squares"* — the tune-up minigame's background, the studio splash, most large
images. Sprites (wrench, lettered bolts, coin, timer) were always fine.

`glCompressedTexImage2D` was defined **nowhere in the shim**. It did not fail to
link, because our `libGLESv1_CM.so` keeps a `DT_NEEDED` on the renamed stock
library, so the call fell through to Nexell's VR5 driver — which can do nothing
without `/dev/ogl_vr5`. The texture kept the opaque white that the `NULL`
`glTexImage2D` path fills in.

**A missing symbol that RESOLVES to something useless is worse than one that
fails to link.** Nothing reports an error, the app runs, the output is merely
wrong. Check `llvm-nm -uD App.so` against what the shim actually exports —
`llvm-nm -D runtime/shimlibs-gl/libGLESv1_CM.so` — rather than assuming the
stub file covers everything. The stubs only cover names someone thought of.

### What Clam Prix actually uses

```
$ llvm-nm -uD .../LST3-0x00180025-000000/App.so | grep -oE '\bgl[A-Za-z]+'
... glCompressedTexImage2D glTexImage2D glTexParameteri ...   # NO glTexSubImage2D
```

Both compressed uploads are full-screen 320x240 backgrounds, and the byte counts
confirm the layout exactly:

| internalformat | imageSize | palette | indices | texels |
|---|---|---|---|---|
| `0x8B92` PALETTE4_R5_G6_B5 | 38432 | 16x2 = 32 | 38400 @ 4bpp | 76800 = **320x240** |
| `0x8B97` PALETTE8_R5_G6_B5 | 77312 | 256x2 = 512 | 76800 @ 8bpp | 76800 = **320x240** |

`OES_compressed_paletted_texture` is core in GLES 1.x: a palette of 2^n entries
in the named base format, then the index stream for level 0, then each smaller
level. PALETTE4 packs two indices per byte, **high nibble first**, as one
continuous stream — rows are not byte-aligned. `level` is non-positive (it
encodes how many mip levels follow); only the base level is decoded.

Verified: `tex_23_320x240` decodes to the Virtuos studio logo, and **zero of the
47 dumped textures are all-white** (`TADPOLE_GL_DUMPTEX=1`, then convert the raw
ARGB8888 dumps to PNG). Menu unchanged, no crashes.

### Two more texture bugs fixed in passing (not used by this title)

* **`glTexSubImage2D` was a no-op stub.** Any title that allocates with a NULL
  `glTexImage2D` and fills with sub-uploads would get the same white squares.
  Clam Prix does not import it, so this was NOT the cause here.
* **`glTexImage2D` ignored `format` entirely** and read every `GL_UNSIGNED_BYTE`
  upload as 4 bytes per texel. `GL_RGB` is 3 and `GL_ALPHA`/`GL_LUMINANCE` are 1,
  so those walked diagonally and ran off the end of the caller's buffer. Both
  now share one `convert_row()`, with the fixed-function expansion rules
  (ALPHA -> RGB 0, LUMINANCE -> RGB = texel, A = 255, etc).
* **`glTexParameteri`** was stubbed while `glTexParameterx` was not; wrap and
  filter modes set through the int entry point were silently lost.

## SOLVED: ~5 seconds of audio delay — buffer CAPACITY is not buffer DEPTH

Long-standing, finally measured. The chain is:

```
guest snd_pcm_writei -> FIFO (64 KB) -> viewer ring (512 KB) -> SDL
```

At the negotiated **32 kHz stereo 16-bit = 128000 bytes/sec**:

| stage | bytes | seconds |
|---|---|---|
| viewer ring | 524288 | 4.10 |
| FIFO | 65536 | 0.51 |
| **total worst case** | | **4.61** |

Which is the "roughly 5 seconds" that was reported. Nothing drained it, either:
the cursors only converge if the guest UNDER-produces. A title bursts audio
ahead while it loads, the ring pins to full, and the lag becomes permanent.

The capacity was never the problem — a deep ring usefully absorbs jitter. What
was missing is a bound on how far AHEAD of the speaker we run. `audio_cb` now
trims the backlog above `ADEPTH_MAX_MS` (260) down to `ADEPTH_TARGET_MS` (120),
dropping the OLDEST audio.

* **Trimming in the callback is what makes it race-free** — the callback is the
  only writer of `g_atail`. Advancing it from the main loop would race.
* **Hysteresis** (trim only above MAX, but all the way down to TARGET) keeps it
  rare: one small discontinuity when the guest has run ahead, not a continuous
  stutter.
* **Underrun risk is unchanged**: TARGET still holds ~4 SDL periods (32 ms each)
  and short gaps already fill with silence.

`g_atrims` counts how often it fires — if that climbs steadily rather than
settling, the guest is consistently outrunning realtime and the cap is the wrong
tool.

## STILL OPEN: 3D geometry does not render

`shots/spongebob run/emulated_post_patch/in_race_3d_stuff.png` — the race view is
almost entirely black. 2D HUD survives (minimap outline, "5th", coin count, LAP
and timer), so the rasteriser and the viewport are fine; it is specifically the
perspective-projected track geometry that is missing. Note the hugely magnified
yellow/orange texture at the top left, which suggests geometry landing at the
wrong scale rather than not being submitted at all.

Untested against the paletted-texture fix — worth re-capturing before digging,
since the track's textures were among the white ones.

## The front end (tadpole/viewer/tadpole_ui.c)

The viewer used to be something `tadpole.sh` started. It is now the application:
a menu bar, settings dialogs and a file chooser, so the shell command is no
longer the only way in. `tadpole.sh` still works exactly as before and the
scripted harnesses (`probe-launch.sh` et al) are unaffected — command-line flags
still override saved settings.

```
File                        Options                     Help
  Run System Menu             Audio Settings...           About Tadpole
  Launch .swf...              Graphics Settings...
  Install Package...          Controller Settings...
  Setup System Firmware...
  Stop Emulation
  Quit
```

Plus a **ROT** button at the right of the bar: one click per 90 degrees, applied
live and saved. Ctrl+R still does the same thing.

### The coordinate contract — read this before touching the layout

The bar lives INSIDE the renderer's logical space, which is now

```
width  = panel width (rotated)
height = UI_BAR_H + panel height (rotated)
```

A click at logical `y < UI_BAR_H` is chrome; anything lower is the guest's,
shifted up by `UI_BAR_H` before `event_to_fb()`. Doing it this way means
`event_to_fb()` — which cost a long hunt to get right — did not change at all.

`--selftest` was updated to build its window the same way, so it still tests
what actually ships. All four rotations pass.

### Everything is hand-rolled, on purpose

No SDL2_ttf and no zenity/kdialog on this machine, so the font, the widgets and
the file chooser are all local. That is not a workaround: a 5x7 bitmap font and
1px bevels are the look, and they scale as pixel art with the window instead of
going blurry. `tadpole.png` is decoded by a small inline PNG reader (8-bit RGBA
only, which is what the file is) rather than adding SDL2_image for one icon.

The font is authored as **ASCII art** in `tools/genfont.py` and generated into
`viewer/tadpole_font.h`, so the glyphs stay readable and correctable in source.
Run it with a second argument to also write a proof sheet.

Two glyph lessons: descenders (g p q y j) must start at row 2 or they read as
capitals, and anything with a curve — a rotation arc — is unreadable at 5x7 and
came out as the letter C. The rotate button says `ROT 90` for that reason.

### --ui-shot: capturing the interface

The desktop here is Wayland, and a Wayland compositor will not route
XTEST-synthesised clicks into an XWayland client — so "open the menu and
screenshot it" cannot be driven with xdotool. Instead:

```
tadpole-view --ui-shot <state> out.png     # idle file options help gfx
                                           # audio pad about files pkg running
```

renders one frame of a named UI state and writes a PNG. Deterministic, and far
more repeatable than aiming a synthetic pointer. Companion to `--selftest`.

### BUG FOUND BY THIS WORK: the guest shut itself down after 12 seconds

The input FIFOs are created by the SHIM, so on a cold start they do not exist
until a guest has booted. The viewer opened them once at startup — all it ever
needed, because `tadpole.sh` used to create them (via the boot-logo step) before
launching the viewer. Now the viewer starts first, every `g_evfd` stayed at -1,
and all input was silently discarded.

That is not a subtle failure. AppManager arms a **12-second shutdown timer** at
boot and expects to be told it is on external power:

```
[0x280] Shutdown Timer set for 12000 [ms] timeout
[0x280] USB Device Watchdog set for 12 [sec] timeout
...
[tadpole] fcreate .../runtime/sysroot/tmp/shutdown
[0x0] Displaying the shutdown screen now.
```

With the FIFO never opened the power message was dropped, the timer fired, and
the guest wrote `/tmp/shutdown` and exited by itself. Symptom: "it boots and
then quits". `ev_open_missing()` now retries every frame and the announcement
waits for the pipe to exist.

### Firmware setup

`File -> Setup System Firmware` runs `tools/setup-firmware.sh`. From
OpenLFConnect: **`.lf2` is a tar.bz2 and `.lfp` is a zip**, despite the opaque
extensions. The script unwraps the outer container, extracts each `.lf2`, and
installs a rootfs tree if it finds one. It is **UNTESTED against a real firmware
zip** — none was available — and reports what it found at each step rather than
half-installing. If the bundle holds a raw UBI image instead of a plain tree it
says so, because extracting that needs ubireader, which is not vendored.

### Settings

`~/.config/tadpole/ui.cfg`, plain `key value` lines. Graphics settings become
environment variables for the launched guest (`TADPOLE_GL`, `TADPOLE_GL_DEBUG`,
`TADPOLE_GL_DUMPFRAME`, `TADPOLE_GL_DUMPTEX`, `TADPOLE_TOUCH_DEBUG`), which is
why GL only applies at the next launch. Audio latency is live: it drives the
`audio_cb` trim thresholds directly.

## GL is ON by default, and the checkbox now works from either launch path

Reported: the Options -> Graphics checkbox appeared to do nothing —
`./tadpole.sh` asserted in the stock EGL, while `TADPOLE_GL=1 ./tadpole.sh`
worked.

Both were true, and the front end was innocent. Verified by reading the child's
`/proc/<pid>/environ`: launching from **File -> Run System Menu** does pass
`TADPOLE_GL=1` and does put `shimlibs-gl` on the path. The failing case is the
OTHER entry point — starting via `./tadpole.sh` directly, where the script chose
GL from *its own* environment and never looked at the checkbox.

Now:

1. **GL defaults ON.** Without it every native Brio title dies in the stock
   libEGL (`vr5_platform_fbdev.cpp:391`, "The memory device has not been
   opened"). `TADPOLE_GL=0` restores the stock stack.
2. **`tadpole.sh` reads `~/.config/tadpole/ui.cfg`** when `TADPOLE_GL` is not
   already set, so the checkbox governs both paths. Precedence, all verified:
   environment > config file > default(1).
3. Toggling while a guest is running says "GL: reboot to apply" — the setting is
   applied when the guest is launched, so it cannot take effect mid-session.

## Frame pacing: one missing wait, several symptoms

`FBIO_WAITFORVSYNC` returned IMMEDIATELY. On hardware it blocks until the
panel's next refresh, and for a Brio title that is the only thing pacing the
render loop. Uncapped, a title runs as fast as qemu allows AND generates audio
per frame far faster than it can be played — so the viewer's latency trim keeps
dropping the backlog and the sound skips forward, fast and garbled.

Two places now pace, because a title only reaches one of them:

* **`vsync_wait()` in tadpole_shim.c** — the 2D/Flash path.
* **`pace_frame()` in tadpole_gles_core.c**, at the end of
  `tadpole_gl_present()` — a GL title's loop is bounded by `eglSwapBuffers`,
  which our EGL used to return from instantly. It never reaches the fb ioctl.

Both use a monotonic deadline, resync when they fall behind rather than
accumulating debt (which would make the guest sprint to catch up), and never
sleep when already late — so heavy 3D cannot be made slower. `TADPOLE_HZ=0`
disables pacing; `TADPOLE_HZ=n` sets the rate.

### MEASURED, and it changes the picture

`TADPOLE_GL_DEBUG=1` now reports the achieved rate. The Clam Prix main menu runs
at **8–15 fps**, not hundreds:

```
fps x100 over 60 frames 1210   ->  12.1 fps
                        1500   ->  15.0 fps
                         830   ->   8.3 fps
```

So the software rasteriser is ALREADY the limit for a GL title, and pacing never
engages there. The "runs ridiculously fast" reports must come from the 2D/Flash
path, which is what `vsync_wait()` addresses. Do not assume the GL pacing fixed
anything that was fast — measure it.

A related measurement: `vsync_count` does not advance at all on the home screen
(0 ticks in 15 s), and a GL title never pans. Anything keyed off that counter is
effectively dead code — see the audio bug below.

## SOLVED: audio "too fast and garbled" in games, fine on the home screen

`audio_poll_fmt()` — which reopens the SDL device when the guest negotiates a
new PCM format — was called only when `(vsync_count & 0x1F) == 0`. That counter
is advanced by the guest's flips, and per the measurement above it can sit still
for an entire session. So the test was either never true or always true
depending on where the counter stopped.

Consequence: a title that opens the PCM at a **different rate from the home
screen** never got the device reopened, and its audio played at whatever rate
the previous app had negotiated — fast and garbled, while the home screen
sounded correct. Exactly the reported split.

It now polls on a 200 ms wall clock. Never key housekeeping off a counter the
guest may not advance.

## Audio dropped by the ALSA shim is now counted, not silent

`snd_pcm_mmap_writei` retries for 250 x 2 ms and then **discards whatever is
left while telling the caller every frame was written**. That truncates the
sound mid-phrase — "Rinse your p-" is this, not a decoder fault. It only happens
when the guest outruns real time. `g_dropped` now accumulates and, with
`TADPOLE_DEBUG=1`, logs `audio DROPPED n bytes (total n)` so the condition is
observable instead of inferred.

## New tooling

* **`tools/key.py`** — inject button presses (D-pad, A/B, L/R, Home, Esc).
  Leapster titles are BUTTON driven: taps on the menu bubbles do nothing, the
  selection ring moves with the D-pad and A confirms. `tap.py` alone cannot get
  past a title's first screen.
* **`tools/probe-seq.sh`** — boot, sign in, launch, then run a SEQUENCE of steps,
  capturing a screenshot after each. Steps are `X,Y[,delay]` for a tap or
  `key:NAME[,delay]` for a button. The per-step captures are the point: a wrong
  guess is visible instead of silently derailing the run.

  The label under the selected bubble names it, which is how the main menu was
  mapped: gears = MicroMods, star = Badges — **not** Options.

### The ancestor-kill trap, again

`reap()` in both probe scripts used `pgrep -f "TADPOLE_DIR=$TADPOLE_DIR"`, which
also matches the SHELL THAT INVOKED THE SCRIPT, because that string is in its
command line too. Killing a parent kills the pipeline; the symptom is a run that
produces a short log and exit 1 for no visible reason. Guarding with `$$` is not
enough — both scripts now walk `/proc/<pid>/stat` and skip the whole ancestor
chain. The same trap bit ad-hoc `pkill -f` one-liners twice this session.

## SOLVED: audio ran at 161x real time — the root cause of nearly everything

Reported: "the spongebob game is running EXTREMELY FAST and sounds terrible",
sound effects clipped, some sounds fine and others garbled, Brio games worst of
all — while the home screen sounded good.

`libAudio.so` imports **no** `snd_pcm_wait`, **no** `snd_pcm_avail_update` and
**no** `snd_pcm_delay` (checked with `strings`). Its entire pacing comes from
`snd_pcm_mmap_writei` BLOCKING once the device buffer is full, which is how any
real ALSA playback loop is throttled.

Our shim never blocked meaningfully. It dumped into a FIFO that the viewer
drains into a deep ring, so Brio's mixer thread rendered as fast as the emulated
CPU allowed, and when the pipe finally backed up we spun for 500 ms and threw
the remainder away.

MEASURED, with a reader draining as fast as possible
(`TADPOLE_AUDIO_PACE=0` disables pacing, which is how the A/B was taken):

| | bytes/sec into the FIFO | vs real time |
|---|---|---|
| before | 20 651 950 | **161x** |
| after  | 128 007 | **1.00x** |

Correct is 128000 (32 kHz x 2ch x 2 bytes).

`pace_pcm()` now models the device: bytes drain at exactly the frame rate, and
the writer is held until the in-flight amount fits inside one device buffer
(~128 ms at the negotiated 4096 frames). If the guest goes quiet long enough for
the buffer to drain, the clock RESYNCS rather than accumulating credit —
otherwise a silent passage would earn the right to dump a burst afterwards and
sound fast all over again. The give-up threshold rose from 250 to 1500 spins,
since with pacing ahead of it the pipe should rarely be full.

### This also explains the "fast AND slow at the same time" paradox

Brio drives game logic from its audio callback thread. So the LOGIC was running
161x too fast while the software rasteriser only managed 8-15 fps — which is
exactly the pair of symptoms reported: "ridiculously fast" and "so slow they are
unplayable", simultaneously, in the same title. Pacing the audio paces the game.

### Consequence worth knowing

If audio is disabled in the viewer, nothing reads the FIFO, `open()` fails, and
`snd_pcm_mmap_writei` returns early WITHOUT pacing — so the guest goes back to
running unthrottled. That is also why headless probes run fast. The frame pacing
in `pace_frame()`/`vsync_wait()` is the independent floor for that case; keeping
both matters.

With production at 1.00x the latency cap no longer has to fight anything, so the
Audio Settings value can come back down (120-180 ms) from whatever it was raised
to.

## Two dead ends, recorded so they are not retried

* **Concurrent PCM streams sharing one FIFO.** `g_pcm[4]` all point at
  `$TADPOLE_DIR/audio`, which looked like an obvious mixing bug. It is not what
  was happening: the guest opens exactly ONE playback handle, `name=plugdmix`,
  and Brio mixes internally before writing. Verified by logging every
  `snd_pcm_open`.
* **`snd_pcm_avail_update` lying** (it returns `period_size` unconditionally).
  Only `libMicrophone.so` imports it — the capture path. The playback path never
  calls it, so it cannot be the pacing fault.

Also: with no reader on the FIFO, `open_fifo()` is retried on EVERY write —
190 787 times in one headless run. Harmless but wasteful; worth a backoff if it
ever shows up in a profile.

## GL failure diagnostics are now unconditional

A log sent to diagnose the white Options/Credits screens contained zero `[gl]`
lines, because `TADPOLE_GL_DEBUG=1` had not been set — so the round trip was
wasted. The three conditions that mean the output is definitely wrong now print
through `warn2()` regardless of the debug flag:

```
[gl] WARN UNTEXTURED DRAW: bound name / has-slot <id> <0|1>
[gl] WARN UNSUPPORTED compressed format / size <enum> <bytes>
[gl] WARN glGenTextures EXHAUSTED (MAX_TEXS), fails so far <max> <n>
```

Each is one-shot or near enough, so a healthy run stays silent and any ordinary
log already carries the evidence.

### Still unresolved: the white Options/Credits screens

Not reproduced here. Every screen reachable with `tools/probe-seq.sh` — main
menu, Badges, MicroMods — renders correctly and reports none of the three
warnings. Menu navigation by key count is not deterministic (the selection ring
starts somewhere different depending on animation state), so Options was never
landed on. The label under the selected bubble names it, which is how the rest
were identified.

## SOLVED: vertex alpha was discarded — "textures render white" across titles

The Clam Prix credits screen was a solid white rectangle. So were Options and,
per the owner, screens in other titles.

### The defect

In the fragment loop in `raster_tri()`:

```c
R = interpolated from the vertex colours;
G = interpolated;
B = interpolated;
A = 255;                 /* <-- the bug */
```

The vertex colour's **alpha was thrown away and forced opaque**. For a TEXTURED
draw the MODULATE step immediately below overwrote `A` with the texel's alpha,
which hid the mistake completely. For an UNTEXTURED draw nothing restored it, so

* `if (A == 0 || !alpha_passes(A)) continue;` never discarded anything,
* `if (g_blend_on && A < 255)` was never true, so blending never ran,
* and every flat triangle was written fully opaque.

A title fades between screens by drawing a translucent full-screen quad with
texturing off. With alpha ignored, a fade becomes permanent paint. Hence a
finished screen buried under an opaque rectangle.

Fixed by interpolating alpha like the other three channels.

### How it was found, since three earlier hypotheses were wrong

The three warnings added for this (`UNTEXTURED DRAW`, `UNSUPPORTED compressed
format`, `glGenTextures EXHAUSTED`) all stayed silent — the screen was not a
missing texture at all. What actually located it was per-frame accounting:

```
[gl] FRAME draws/tris-in 28 212
[gl] FRAME tris-on-screen/pixels 138 164140      <- 164k pixels into a 76800 window
[gl] FRAME textures used: 3 7 57 58 59 60 61 62 63 64 65  untextured-tris 70
```

Two facts fell out immediately: the credits TEXT textures (57-65) were being
sampled correctly, and ~70 triangles per frame carried no texture. Then
`TADPOLE_GL_NOFLAT=1`, which drops every flat-shaded triangle, took the window
from **100% white to 0% white** and showed the finished credits screen — art and
animation team names over the background. That isolated the culprit to flat
geometry in one run, after which the hardcoded `A = 255` was obvious on reading.

Ruled out along the way, each by measurement rather than argument:

* **Blend mode.** The title only ever requests SRC_ALPHA / ONE_MINUS_SRC_ALPHA
  (1972 calls, no other pair) — exactly what we implement.
* **Vertex colour arrays.** `GL_COLOR_ARRAY` is never enabled; colour comes
  entirely from `glColor4ub`/`glColor4x`, both of which were already applied.
* **GL_FIXED vertex data.** `glVertexPointer(2, GL_FIXED, ...)` is used
  throughout and `fetch()` handles both the type and the missing z correctly.
* **Multitexturing.** `glActiveTexture` is never called at all.
* **A white placeholder texture.** Zero NULL uploads in the whole run.

`glActiveTexture` did turn out to be a no-op that ignored the unit, and
`GL_TEXTURE_2D` enable is per-unit in GLES 1.x, so that is now tracked properly —
correct, but not the cause here.

### New diagnostics, all behind TADPOLE_GL_DEBUG

* `FRAME draws/tris-in`, `FRAME tris-on-screen/pixels` — separates "no draws
  arrive" from "draws arrive and are rejected" from "painted in the wrong
  colour". Three different bugs that all look like "nothing renders".
* `FRAME textures used: ... untextured-tris N` — which textures a frame actually
  SAMPLED. "A texture is bound and has data" says nothing if no draw reads it.
* `FRAME biggest flat tri WxH at (x,y) argb` — the screen-coverers come LAST in
  a frame, so a head-of-frame log limit never catches them.
* `TADPOLE_GL_NOFLAT=1` — drop flat-shaded triangles. A one-run bisection for
  "something opaque is covering the content".

## Testing harness: the D-pad is ROTATED

Owner's tip, and it explains what looked like menu non-determinism. The
LeapPad2's D-pad codes assume the device is held PORTRAIT; a Leapster title is
played turned on its side, so **physical "up" is the game's "left"**:

```
physical up -> game left,  right -> up,  down -> right,  left -> down
```

`tools/key.py -g <dir>` now takes GAME-space directions and rotates them.
Without this, "press down four times" moved the ring sideways and the menus
looked random.

Touch DOES work in these menus, contrary to an earlier note here — it is just
imprecise, and the bubbles drift, so a tap aimed at a remembered coordinate
often misses.

### tools/nav-label.py — close the loop on the label

Counting key presses still does not work, because the ring starts wherever the
entry animation left it. The selected item's NAME is drawn on screen, so:
press, capture, compare the label, repeat.

Two things had to be right for the comparison to work:

1. **Compare only the yellow text**, not the strip. The bubbles float and
   SpongeBob animates, so a raw pixel diff scored ~22 for the CORRECT label
   against wrong labels at 29-42 — no usable threshold. A per-column count of
   saturated yellow pixels gives 0.14 for the same label in a different frame
   versus 1.85-3.27 for a different one.
2. **The region must span both label heights** — the main menu draws it around
   y=120, the Options screen around y=155. With the narrow box, Controls and
   Credits differed by 0.04 because it caught neither.

Also: these menus are a 2D grid that does NOT wrap, so repeating one direction
parks the ring against an edge and the label stops changing — twelve presses of
the same key left the selection on "Play" throughout. `--keys a,b` cycles.

Menu map, read off the labels: gears = **Options** (not MicroMods, as an earlier
A-press test wrongly suggested), star = Badges, flag = Play. Options contains
Controls, Credits and Records.

The confirming A press is still not fully reliable — it is sometimes swallowed
while the selection animates, and a run then ends on the menu. `nav-label.py`
retries it up to three times; runs still occasionally miss.

## Depth buffer: implemented (there was none)

`glDepthFunc` only traced; `glDepthMask`, `glDepthRangef`, `glClearDepthf` and
`glClearDepthx` were no-op stubs; `GL_DEPTH_TEST` was not tracked at all. Every
fragment was written unconditionally, so the visible result was purely
submission order. That is a prerequisite for the race tracks rendering at all,
and it is why "3D is completely dead" is not surprising.

Now: a `float g_zbuf[FB_W*FB_H]`, cleared by `GL_DEPTH_BUFFER_BIT`, all eight
compare functions, and `glDepthMask` honoured. Depth is the post-divide NDC z
mapped to 0..1, which interpolates linearly in screen space, so plain
barycentric interpolation is correct.

Verified not to regress 2D: the Clam Prix main menu, Badges and MicroMods all
still render correctly afterwards.

## glIsEnabled and glGetIntegerv were lying

```c
GLboolean glIsEnabled(GLenum c) { (void)c; return 0; }              /* before */
void glGetIntegerv(GLenum p, GLint *v) { (void)p; if (v) *v = 0; }  /* before */
```

Answering "0" and "not enabled" to everything is worse than not implementing
them. Any renderer that preserves state does

```c
GLboolean was = glIsEnabled(GL_TEXTURE_2D);
... draw ...
if (was) glEnable(GL_TEXTURE_2D); else glDisable(GL_TEXTURE_2D);
```

and a glIsEnabled stuck at 0 turns every restore into a DISABLE. Both now answer
from the state we track, with plausible limits for the rest.

HONEST SCOPE: Clam Prix never calls either one (the added `UNHANDLED` traces
stayed silent), so this did NOT fix the credits screen. It is a real correctness
bug that would bite any title doing state save/restore — fixed on its merits,
not as the cause here.

## Clam Prix credits: characterised precisely, NOT yet fixed

Reproduced reliably. `TADPOLE_GL_NOFLAT=1` takes the window from 100% white to
0% and reveals a correct screen (art/animation team names over the background),
so the content renders and something opaque covers it.

The exact call sequence, from the frame that paints it:

```
glColor4x  argb 00000000       transparent
glDisable  GL_BLEND / GL_TEXTURE_2D / GL_DEPTH_TEST
glDrawElements TRIANGLES 192   64 tris, transparent  -> now discarded (alpha fix)
glEnable   GL_BLEND
glColor4ub argb ffffffff       OPAQUE WHITE
glDrawElements TRIANGLES 6     2 tris, 320x240 at (15,17) -> COVERS THE WINDOW
glDrawElements TRIANGLES 6     ...and several more
glEnable   GL_TEXTURE_2D
```

State at the covering triangle, measured rather than inferred:

```
[gl] FRAME biggest flat tri 320x240 at (15,17) argb ffffffff curcolor ffffffff
     draw#25 tex2d=0 blend=1 depth=0 alpha=0
```

So it is an untextured, opaque-white, full-window quad with blending enabled and
both tests off. By every piece of state we can see, hardware would paint it white
too — which means the remaining question is why the app asks for alpha 255.

**Leading hypothesis: a fade stuck at full opacity.** A white full-screen quad
drawn last, over finished content, is a cross-fade. `glColor4ub` is only ever
seen with `ffffffff` — the fade never animates. If its alpha comes from a game
timer, then a broken clock pins the fade at 100%. Note the probes are HEADLESS,
so nothing reads the audio FIFO, `snd_pcm_mmap_writei` returns early and the
audio pacing that now regulates game time does NOT engage — the guest runs
unpaced in exactly these runs. Worth re-testing WITH the viewer attached before
assuming a GL fault.

Ruled out for this screen: missing textures, unsupported compressed formats,
texture-name exhaustion, blend mode, colour arrays, GL_FIXED vertex data,
multitexturing, white placeholder textures, depth state, and the state-query
bug above.

## The D-pad rotates with the display

The LeapPad2's D-pad codes assume the device is held PORTRAIT; a Leapster title
is played on its side, so physical "up" is the game's "left". The viewer now
derives the key rotation from the DISPLAY rotation rather than adding a separate
setting — whatever makes the picture upright also makes the arrows agree with it,
so the ROT button fixes the controls in the same click.

Directions clockwise are up, right, down, left, and the shift is
`(4 - rotate/90) % 4`:

| rotate | up sends | right sends | down sends | left sends |
|---|---|---|---|---|
| 0   | up    | right | down  | left  |
| 90  | left  | up    | right | down  |
| 180 | down  | left  | up    | right |
| 270 | right | down  | left  | up    |

270 is the orientation Leapster titles are played in, and there visual up sends
physical RIGHT — which is the inverse of "physical up is the game's left", as it
should be. `TADPOLE_RAW_DPAD=1` disables the rotation.

`tools/key.py -g <dir>` does the same thing for the harness.

## PERFORMANCE: measured, and the ceiling is architectural

### What was done

Three per-pixel costs hoisted out of the rasteriser inner loop:

1. **Incremental edge functions.** The edge function is linear in the pixel
   centre, so three full evaluations per pixel (~12 float ops) become three
   ADDS: dE/dx and dE/dy are constant across the triangle.
2. **Reciprocal area.** `w/area` three times per pixel was three float DIVIDES,
   the most expensive thing available under soft-float qemu. One reciprocal per
   triangle turns them into multiplies.
3. **Uniform vertex colour.** These titles never enable `GL_COLOR_ARRAY`
   (measured: zero calls in a whole run), so all three vertices carry the same
   glColor and interpolating it spent ~20 float ops per pixel computing a
   constant. Resolved once per triangle instead.

Barycentrics are now computed only when texcoords or depth actually consume them.

**Result: 8.24 -> 11.74 fps on the Clam Prix main menu (+42%).** Benchmark
repeatability is ±0.05 fps over three runs, so that is real.

A second round — replacing `x*y/255` with a shift-based exact equivalent, a
direct-texel fast path for "texture modulated by opaque white", scoping the depth
clear to the layer window, hoisting the alpha-test flag — produced **no
measurable change** (11.5 vs 11.74, i.e. within noise or marginally negative).
Worth knowing: the arithmetic was not the bottleneck, so further micro-tuning of
it is not where the time goes.

### Where the time actually goes

`TADPOLE_GL_NORASTER=1` submits all geometry and paints no pixels, which
separates the two costs that get conflated:

```
total        87.0 ms/frame   (11.5 fps)
guest only    8.6 ms/frame   (116.7 fps, no per-pixel work)
rasteriser   78.4 ms/frame   = 90% of the frame
throughput   1.07 Mpx/s      (93194 pixels/frame)
```

**The guest is not the problem.** Game logic, Brio, Flash, our vertex transform
and qemu's translation of all of it together sustain ~117 fps — comfortably above
the 60 fps cap. Ninety percent of the frame is our own per-pixel loop.

### Why that is a structural ceiling, not a tuning problem

The rasteriser is ARM code living INSIDE the guest, so every pixel it touches is
paid for through qemu's translation, with soft-float VFP on top. It is on the
wrong side of the emulation boundary. Micro-optimising ARM code that a JIT then
re-translates has a hard floor, which the second optimisation round already hit.

The fix is the one HANDOVER has described from the start: **move rasterisation to
the host.** The guest-side shim becomes a thin encoder that serialises GL calls
into shared memory, and the native viewer replays them — either into its own
rasteriser compiled for x86, or onto host OpenGL. Same shape as virgl, one layer
up: at the GLES1 API instead of virtio.

Two things make this attractive here rather than speculative:

* The command stream is small. A frame is ~28 draw calls and a few hundred
  triangles; the vertex data already lives in buffer objects we own, so most
  frames would send state changes and offsets, not geometry.
* The guest's 117 fps ceiling means if rasterisation became free, frames would
  hit the 60 fps cap. The headroom is measured, not hoped for.

Until then, `TADPOLE_HZ=n` caps the frame rate (60 by default) and
`TADPOLE_GL_NORASTER=1` is the isolation switch for re-measuring this split after
any change.

## HLE (host-GPU replay): feasibility PROVEN, encoder not yet built

The software rasteriser is 90% of the frame and its ceiling is architectural (see
the performance section). The fix is to stop rasterising inside the guest and
replay the GL stream natively. `TADPOLE_GL_HLE=1` / Options -> Graphics -> "Host
GPU replay (EXPERIMENTAL)" is the switch. GL itself is no longer experimental —
it is simply how titles render, and the label was updated to say so.

### Correcting an earlier claim in this file

`tadpole_egl.c` said host GL "cannot work" because guest ARM code cannot call
host libraries. The first half is true and still is — the guest cannot dlopen
Mesa. The conclusion was too broad: SERIALISING the GL stream into shared memory
and replaying it in the native viewer crosses the boundary perfectly well. Same
shape as virgl, one layer up: at the GLES1 API instead of virtio.

### What the probe measured

`tadpole/viewer/hle_probe.c` (`make hle-probe`) exists to test the design's
assumptions before any of it is built, and to re-test them on other hardware:

```
GL_VERSION   4.6 (Compatibility Profile) Mesa 26.1.2
GL_RENDERER  AMD FirePro W4100 (radeonsi)
fixed function: glVertexPointer=yes glMatrixMode=yes glTexEnvi=yes glAlphaFunc=yes
FBO 480x272 with 16-bit depth: complete
glReadPixels 480x272 BGRA:  0.103 ms/frame   (software raster: 78 ms)
clear+draw+readback:        0.338 ms/frame   -> ~2960 fps ceiling
centre pixel = texture colour, corner = clear colour   (it really drew)
```

Three things that were genuine risks, now settled:

1. **Fixed function is available natively.** GLES 1.x has no shaders, so a
   core-profile-only host would have meant reimplementing the entire
   fixed-function pipeline in GLSL. The compatibility profile exposes
   `glVertexPointer`, `glMatrixMode`, `glTexEnv`, `glAlphaFunc` and the rest, so
   the mapping is nearly 1:1.
2. **Readback is not the bottleneck.** This was the main worry — GPU rendering is
   pointless if getting pixels back costs more than software rasterising them.
   0.103 ms against 78 ms is a factor of ~750.
3. **A second GL context coexists with the viewer's SDL_Renderer.** The replay
   context lives on a hidden window and renders to an FBO, so the existing
   three-layer compositor keeps working untouched — the FBO is read back into the
   fb1 arena exactly where the software rasteriser writes today.

### Expected gain, from measurements not extrapolation

The probe also runs a frame-SHAPED load: 28 draw calls (the measured Clam Prix
figure) and 913920 painted pixels, which is deliberately ~10x a real frame's
93194 — so it is a conservative bound rather than a replica.

```
FRAME-SHAPED load: 28 draws, 913920 px, + readback
  0.585 ms/frame          throughput 1563 Mpx/s
software rasteriser       throughput    1.07 Mpx/s
a real 93000 px frame would cost ~0.060 ms host-side
```

Putting that against the measured frame budget:

| | ms/frame | fps |
|---|---|---|
| software rasteriser (measured) | 87.0 | 11.5 |
| HLE, encoder costs 0.5 ms | 9.7 | 103 -> capped at 60 |
| HLE, encoder costs 2.0 ms | 11.2 | 89 -> capped at 60 |
| HLE, encoder costs 5.0 ms | 14.2 | 70 -> capped at 60 |

The pixel work is **~134x cheaper on the GPU**, and the guest's own 8.6 ms
becomes the floor. So the realistic outcome for 2D titles is the 60 fps cap —
about 5x today — with the LIMITER MOVING from the renderer to the guest.

Three honest caveats:

1. **The encoder cost is estimated, not measured.** It is the one unknown, and it
   is the reason the table brackets it. It should be small: HLE trades O(pixels)
   work in the guest for O(calls), and a few hundred small packets is nothing
   against 93000 pixels each doing 10-30 float operations. But it is guest ARM
   code under qemu, so it is not free.
2. **Per-draw-call cost is the new scaling axis.** 28 draws cost ~0.5 ms
   host-side, so ~16 us each. A title issuing 500 draws a frame would spend ~8 ms
   there. Still fine, but it is what to watch instead of pixel count.
3. **3D gains far more than 5x.** A full-screen 3D track is many more pixels with
   a depth test on each, which at 1.07 Mpx/s is hopeless — that is why the race
   levels are unplayable rather than merely slow. At 1563 Mpx/s it is free. The
   2D speedup is capped by the guest; the 3D speedup is the difference between
   impossible and trivial.

### Remaining work

The transport and the encoder/decoder. Roughly:

* A ring buffer in `$TADPOLE_DIR/glcmd.bin`: header with magic/version/size and
  monotonic head/tail cursors, then a byte stream of `{u16 op, u16 pad, u32 len}`
  packets. Head/tail as monotonic counters (not indices) makes full-versus-empty
  unambiguous, the same convention the audio ring already uses.
* ~30 opcodes, which is the complete set these titles touch: clear/clear-colour,
  the matrix stack (mode, load, mult, push, pop, ortho, translate, rotate,
  scale), enable/disable, blend/depth/alpha/texenv state, colour, texture bind +
  upload + parameters, buffer bind + data, the three array pointers, client-state
  enable/disable, draw-arrays/draw-elements, and present.
* **GL_FIXED conversion.** Desktop GL has no `GL_FIXED` vertex type, and these
  titles use `glVertexPointer(2, GL_FIXED, ...)` throughout — the 16.16 data has
  to become float, either at buffer-upload time or when the pointer is set.
* Texture and buffer payloads cross by value. That is affordable: uploads are
  infrequent, the largest texture seen is 320x240 and the largest buffer 48000
  bytes, while a steady-state frame is ~28 draw calls of state and offsets.

Until it exists, `TADPOLE_GL_HLE=1` prints

```
[gl] WARN TADPOLE_GL_HLE requested but the encoder is NOT built yet; still rasterising in software
```

and falls through to the software path, rather than being a toggle that appears
to work and changes nothing.

## HLE is BUILT: encoder, transport and host replayer

`TADPOLE_GL_HLE=1` (Options -> Graphics -> "Host GPU replay") now encodes the GL
stream in the guest and replays it on the host GPU. Round-trip verified end to
end; not yet correct in a live title — see the open issue at the end.

Three new pieces:

* **`shim/tadpole_glcmd.h`** — the wire format, included by both sides. 8 MB ring
  in `$TADPOLE_DIR/glcmd.bin`, `{u16 op, u16 pad, u32 len}` packets, payloads
  padded to 4. head/tail are MONOTONIC byte counters, not indices, so full and
  empty are unambiguous. No pointer ever crosses: arrays travel as (buffer name,
  byte offset), pixels and vertices by value.
* **`shim/tadpole_gles_hle.c`** — the guest encoder, ~40 calls.
* **`viewer/tadpole_hle.c`** — the host replayer: hidden GL window, FBO at panel
  size, replay, then `glReadPixels` straight into the fb1 arena where the
  software rasteriser writes, so the three-layer compositor is untouched.

The core forwards state, matrix, texture and buffer calls IN ADDITION to doing
its normal work, and only the DRAW calls skip the software path. That keeps the
software rasteriser fully live as a reference to diff against — flip the flag and
compare.

### make hle-selftest — the test that made this tractable

Drives the REAL encoder and the REAL replayer in one process, no guest and no
visible window, then checks the pixels:

```
host replayer init                                   ok
guest encoder attaches to the ring                   ok
host replays the frame and presents                  ok
no protocol desync                                   ok
replayed 1 frame(s), 22 packet(s)
corner 000D2113 (the clear colour)                   ok
centre 00FFFF00 (a texel colour)                     ok
quad differs from background — GL_FIXED converted    ok
ring head 488 tail 488 sent 1 done 1                 ok
PASS host-GPU replay round-trips
```

It covers the two things most likely to be silently wrong — the 16.16 -> float
vertex conversion, and the ARGB8888 texture upload arriving as GL_BGRA — and it
runs in a second instead of a five-minute boot. Both halves are compiled from the
shipping sources, so a wire-format change that lands on only one side fails here.

### Two bugs the first live run found

1. **GL context contamination.** `SDL_GL_CreateContext` MAKES THE NEW CONTEXT
   CURRENT. `SDL_RENDERER_ACCELERATED` is an OpenGL renderer here with its own
   context, so after init our `glViewport`/`glScissor` leaked into SDL's and the
   ENTIRE viewer output — menu bar included — was squeezed into a 320x240 corner.
   Our own replay was meanwhile running against SDL's context, where our FBO does
   not exist. Fixed by capturing the previous context before creating ours and
   restoring it around every entry point (`ctx_enter`/`ctx_leave`).

   The giveaway was that the MENU BAR was scaled too. A game-scaling bug cannot
   move the chrome; that made it a renderer-wide state problem, not a viewport
   calculation.

2. **A runaway tail.** One bad packet advanced tail past head. `head - tail` is
   unsigned, so it wrapped to ~4 billion, every subsequent read was garbage, and
   each garbage length advanced tail further — ending at 2 634 025 991 against a
   head of 84. The guest saw a ring that never drained and fell back to software,
   which is why the frame rate read 0 and the picture was still the software
   rasteriser's.

   Two fixes. The host now validates `op < TADGL_OP_COUNT` and a sane length
   before trusting either field, and on violation resyncs `tail = head` and logs
   once instead of crawling forward. And MEMORY BARRIERS were added around the
   handoff: the guest publishes payload bytes before advancing head, the host
   reads head with a barrier before treating those bytes as a packet. `volatile`
   does not order a volatile store against a preceding non-volatile `memcpy`,
   and without that ordering the host can read stale ring contents as a header —
   the most likely origin of the bad packet.

### WORKING: Clam Prix renders through the GPU at ~57 fps

Menu, all three boot logos (Nickelodeon, Virtuos, the title card), 0 desyncs,
0 GL errors. Against the software rasteriser's measured 11.5 fps, that is the
~5x the projection called for, and the limit is now the 60 Hz cap rather than
rendering.

Six more bugs stood between "the protocol round-trips" and that picture. Every
one of them produced a plausible-looking frame rate with wrong or missing pixels,
which is why the per-frame accounting below earns its keep.

**1. GL context contamination.** `SDL_GL_CreateContext` MAKES THE NEW CONTEXT
CURRENT, and `SDL_RENDERER_ACCELERATED` is an OpenGL renderer here with its own
context. So after init our `glViewport`/`glScissor` leaked into SDL's, and our
replay ran against SDL's context where our FBO does not exist. Fixed with
`ctx_enter`/`ctx_leave` around every entry point.

The diagnostic that cracked it was in the screenshot: the MENU BAR was scaled
into the corner too. No game-side bug can move the chrome, so it had to be
renderer-wide state.

**2. `GL_INVALID_ENUM` x801 from glTexEnv.** The titles pass values that are not
valid modes — the software path already ignored them — and forwarding them
verbatim left the GL error flag set for everything after. Only the combination we
honour is forwarded now.

**3. Culling.** `glEnable(GL_CULL_FACE)` was forwarded but `glCullFace` and
`glFrontFace` were not, so the host culled with ITS defaults and dropped every
triangle: 14 draws a frame, no GL error, nothing but the clear colour. The
software rasteriser never culls, so the host no longer does either. MATCH THE
REFERENCE RENDERER rather than out-implementing it — same for GL_LIGHTING.

**4. THE BIG ONE: blob length taken from a header field instead of the packet.**
`glBufferData(target, size, NULL)` allocates without initialising, so the guest
sends name+size and NO payload — an 8-byte packet whose size field still reads
38400. The host trusted the size field:

```
op 32 (BUFFERDATA) len 8   tail 60 -> 38468   (consumed 38408)
```

38 KB swallowed for an 8-byte packet. tail passed head, `head - tail` underflowed
(both unsigned), and the stream was destroyed — taking every texture upload that
followed with it. THE PACKET LENGTH IS AUTHORITATIVE; all four blob-carrying
opcodes now derive payload size from `p.len`.

This was **deterministic** — `tail 38468 past head 17636` reproduced digit for
digit across runs — which is what made it findable. A ring of the last ten
packets, dumped on desync, named the opcode immediately. Chasing it as a
concurrency bug first was wasted effort; identical numbers across runs should
have ruled that out sooner.

**5. Client-side vertex arrays.** The wire carries arrays as (buffer name, byte
offset), because a guest pointer is meaningless on the host. That covers the
menus, which upload to VBOs — which is exactly why the menus worked while the
logos did not. The logos pass vertices DIRECTLY with no buffer bound: measured,
59 of 60 draw-elements packets during the logo window had no usable vertex array,
so the host skipped them and the screen stayed black.

Fixed without adding an opcode, by uploading client data into RESERVED buffer
names (100-103) and referencing those. The guest's own tables only use names
1..64, so there is no collision, and mirroring, GL_FIXED conversion and the
resync all apply unchanged. The guest must scan the indices for the highest one
to know how much to copy — the host cannot, because it never sees a client array.

**6. A resync trigger that fired on normal GL.** `glGenTextures` ->
`glBindTexture` -> `glTexImage2D` is the ordinary idiom, so "bound with no image
yet" is the expected state for every texture a title creates. Treating it as an
error requested a full resync on each creation — 11 of them, each re-sending
EVERY texture — flooding the ring with megabytes of redundant uploads and
blacking out the frames drawn during the flood. That is why the first two logos
failed and the third, after things settled, worked. The check now sits at the
DRAW, which is the only place the gap is observable.

### The frame limiter had to stay

`hle_present()` waits for the host, so the guest inherits the VIEWER's loop rate.
On a 120 Hz display that is 120 Hz, and a Brio title ties its logic to the frame
it just drew — Fists of Fury ran at literal double speed. `pace_frame()` is
therefore still called in the HLE path: the panel is 60 Hz regardless of what the
host monitor does.

### Diagnostics worth keeping (TADPOLE_HLE_DEBUG=1)

```
hle: frame 1260: 14 draws, 2079/3528 px non-black, glerr 0x0000 x0
     | drawelem pkts 869 skipped-nobuf 0 no-array 0
```

Each field caught a distinct bug: `glerr` found the TexEnv rejection, `draws`
found the culling, `skipped-nobuf` found the missing buffer mirror, `no-array`
found the client arrays, and `px non-black` is the only one that distinguishes
"60 fps" from "60 fps of nothing".

### A METHODOLOGY MISTAKE WORTH NOT REPEATING

Every capture was taken 30 seconds after launch, which only ever shows the
steady-state menu. The boot logos are a transient ~13-second window, so the
frames that were actually broken were never in a single screenshot — the owner
had to report them repeatedly before the harness could see them at all.
`probe-hle.sh` now burst-captures 24 frames at 1 s intervals from the moment the
title is tapped.

The follow-on trap: "% near-black" is a useless metric for a WHITE-ON-BLACK logo,
where 94% black is the correct answer. Counting BRIGHT pixels distinguishes "logo
present" (3-7%) from "nothing" (0%).

### Still open

* FMVs proper are untested. The boot logos turned out NOT to be video at all —
  they are paletted GL textures — so the fb2 video layer remains unexercised.
* Letter Factory was reported completely black. Not yet retested since the
  client-array and packet-length fixes, both of which are strong candidates.
* Buffer/texture deletion is not forwarded, so the host's mirror only grows.
  Harmless at these sizes, but unbounded over a long session.

## HLE: falling back must never be silent, or permanent

Reported as "HLE isn't working, it shows 0 fps and performance is as bad as
software". The log said otherwise:

```
[hle] encoding to host GPU
[hle] host stopped presenting; using software raster
```

It had rendered 1039 frames (~17 s) on the GPU and then given up — and
`hle_give_up()` disabled HLE for the REST OF THE SESSION. A transient stall was a
one-way door.

Three changes:

* **The heartbeat is a COUNTER, not a flag.** A flag cannot tell "the viewer is
  alive but slow" from "the viewer died and left a stale 1 behind", and that is
  the whole difference between waiting and abandoning. Only a heartbeat that has
  stopped entirely counts as dead.
* **It says so, loudly.** A banner in the log plus the status bar switching to
  `HLE FELL BACK - software`. A bare `0 fps` reads as "HLE is broken" when the
  truth is "HLE stopped being used", and that cost a confusing round of testing.
* **`TADPOLE_HLE_STRICT=1`** turns the fallback into a deliberate crash with a
  core dump, for when you would rather know instantly than infer it from a frame
  rate. Used exactly once and it earned its keep — see below.

### The trigger was ROTATING THE DISPLAY

Found by the owner, confirmed by the instrumentation:

```
hle: STALL — 4808 ms since the last pump (guest is waiting on us)
```

`UI_ACT_RELAYOUT` calls `SDL_SetWindowSize`, which blocks on a window-manager
round trip for nearly five seconds. The guest sat waiting on a frame, concluded
the host was gone, and fell back every single time. The ring is now drained on
BOTH sides of the resize, and "dead host" needs ten seconds of total silence.

## The D-pad needed a constant quarter turn as well

A Leapster title is landscape on a portrait device, so the game's axes are a
quarter turn from the D-pad's REGARDLESS of how the picture is being displayed.
That constant was missing: rotate=0 mapped identity, and the owner measured

```
visual up -> game LEFT,  right -> UP,  left -> DOWN
```

which is exactly one step out. The shift is now the display correction PLUS
`DPAD_GAME_TURN`. `TADPOLE_DPAD_SHIFT=0..3` overrides it, because controls cannot
be verified without someone holding the keyboard and a wrong guess is worse than
an adjustable default.

## Harness: three tools, and two ways I fooled myself

* **`probe-hle.sh`** — boots WITH the viewer, which `probe-launch.sh` and
  `probe-seq.sh` cannot do. HLE lives in the viewer, so `--no-viewer` means no
  heartbeat and the guest correctly falls back; those harnesses can never
  exercise it. Takes the same step vocabulary as probe-seq.
* **`framestats.py`** — scores a burst: lit %, distinct colours, frame-to-frame
  change, and a verdict of BLACK / flat / static / scene. For 3D the useful
  questions are not "is it black" but "is anything drawn", "does it look like a
  scene rather than flat fill", and "does it CHANGE" — trailing shows up as high
  content with near-zero change.
* **`watch-race.sh`** — samples the arena while a HUMAN plays. Scripted
  navigation into a race stopped paying for itself: the selection ring is
  animation-dependent, a race is several menus deep, each attempt costs five
  minutes, and a person gets there in seconds.

### THE MEASURING INSTRUMENT PERTURBED THE MEASUREMENT, TWICE

1. **`import -window` grabs the X window** and blocks the viewer for about a
   second. Result: 54 stalls and 31 rendered frames in a whole run — the harness
   throttled the emulator to ~1 Hz, and it would have been reported as an
   emulator fault.
2. **`fbshot.py` encodes a PNG in pure Python.** Doing that once a second
   starved the viewer of CPU and produced the same symptom by a different route.

`watch-race.sh` copies the raw 512 KB arena and analyses it afterwards, which
costs the emulator nothing. If a run ever shows an inexplicably low frame count,
suspect the harness before the emulator.

### And a timing mistake worth not repeating

A step sequence fired `key:a` immediately after tapping the title's tile — but a
title spends ~15 seconds on boot logos first, so the press went nowhere and the
burst captured a static logo. Anything driving a title has to wait for the state
it expects, not for a fixed delay.

## Firmware installation, front-end-only startup, and the setup wizard

Three changes that turn Tadpole from a script into something a stranger could
install.

### tools/install-firmware.sh

Written against the real LFConnect cache in `sources/`, not from guesswork —
an earlier `setup-firmware.sh` was written blind and has been deleted.

What the packages actually are:

* `.lf2` is a **bzip2 tar**, `.lfp` is a **ZIP**, despite the extensions
* each holds a package directory with a `meta.inf` manifest
* the cache is hash-named, so the ONLY way to know what a file is, is to read
  the manifest inside it — the installer scans all of them

The one that matters is `Type=DiskImage, Name=Firmware-Base`:

```
Firmware-Base/4,2268688,kernel.bin
Firmware-Base/5,53477376,C4G-E1M-W4K-erootfs.ubi
Firmware-Base/meta.inf            Version="4.6.0.784"
```

That version matches the rootfs already in the tree, i.e. this is exactly where
it came from.

**The root filesystem is a UBIFS volume**, so it needs `ubi_reader`. That is not
vendored: writing a UBIFS reader is a project in itself and the tool exists. When
it is missing the installer says precisely what to install rather than half
completing.

One bug worth remembering: `field()` matched `Version="..."` unanchored, which
also matches `MetaVersion="1.0"` — so it cheerfully reported the firmware as
version 1.0. Anchor manifest field matches at line start.

### tadpole.sh no longer boots anything

The default mode is now `front`: it opens the viewer and waits. `--boot` restores
the old behaviour, and the viewer passes `--boot` when the user picks
File -> Run System Menu.

Auto-starting made the emulator feel like a script rather than an application,
and it meant a first-time user with no firmware got a wall of missing-file errors
instead of an explanation.

### The setup wizard

Windows-style on purpose — banner down the left, one idea per page,
Back/Next/Cancel bottom right — because that arrangement needs no explaining.
Five pages: Welcome, What you need, System files, Games, Ready.

It opens automatically when there is nothing to boot, and lives on Help ->
Setup Wizard otherwise. Every page RE-TESTS real state rather than remembering
that it ran, so it doubles as a repair tool.

A bug caught by looking at the render: the firmware check globbed
`rootfs/*/ubi_rfs`, but the real layout is
`rootfs/stock-4.6.0.784/1221351650/ubi_rfs` — one level deeper. It would have
insisted firmware was missing on a perfectly good install, and popped up on every
launch. `--ui-shot wiz0..wiz4` renders the pages for inspection.

### README.md

Covers dependencies (including the compatibility-profile requirement, since HLE
maps GLES1 onto fixed-function desktop GL), how to obtain firmware and games
legally from hardware you own, the controls, the environment variables, and a
troubleshooting section keyed to the messages the application actually prints.

Every claim in it was checked against the tree rather than written from memory —
`--list`, `--boot`, `make check`, both installers and all three environment
variables were verified to exist.

## The front end must survive a bare checkout

Reported from a clean `git clone`:

```
$ ./tadpole.sh --ui
missing: .../rootfs/stock-4.6.0.784/1221351650/ubi_rfs
run runtime/setup-sysroot.sh first
```

Exactly the wall of text the setup wizard exists to replace — `tadpole.sh` had a
hard precondition check that exited BEFORE the viewer could start. Two faults:

* The precondition now applies only to modes that actually boot something. The
  `front` mode starts the viewer regardless, because a first-time user has no
  firmware and the wizard is the whole point.
* `ROOTFS` was hardcoded to `stock-4.6.0.784/1221351650/ubi_rfs`. It is now
  discovered by globbing `rootfs/*/ubi_rfs` and `rootfs/*/*/ubi_rfs`, since the
  version depends on whatever the user's own device shipped with.

The shim-built check moved behind the same condition, and a missing VIEWER now
says "run: cd tadpole && make" rather than failing obscurely later.

## Run System Menu greyed out on a working install

`guest_external()` reads `$TADPOLE_DIR/.lock` and treats a live pid as "a guest
is running", which disables the `needs_idle` menu items. But `tadpole.sh` writes
its OWN pid to that lock and then, in front-end mode, starts the viewer and
waits — so the viewer always saw a live lock and greyed out Run System Menu and
Launch .swf permanently.

The lock means "this TADPOLE_DIR is in use", not "a guest is running".
`tadpole.sh` now exports `TADPOLE_LOCK_PID=$$` and the viewer ignores a lock
matching it.

### --ui-shot could not see this bug, and now can

`ui_shot()` rendered before anything evaluated the guest state, so every shot
showed the idle case and a greyed-out File menu was unreproducible. It now calls
`ui_set_running(guest_external())` first, which made the difference measurable:

```
no override (the bug) : bright=0    dim=1100 -> GREYED OUT
with override (fixed) : bright=1100 dim=0    -> ENABLED
```

Note the remaining limitation: `--ui-shot idle` explicitly clears the modal, so
it cannot show an AUTO-OPENED wizard. Check the status bar for "setup needed"
instead — only the auto-open branch sets it.

## Erase System Firmware

`tools/erase-firmware.sh`, and File -> Erase System Firmware with a confirmation.
For testing the wizard, which is otherwise hard to reach once an install works.

It MOVES rather than deletes by default. Re-installing needs the firmware
packages and `ubi_reader`, and if either is missing a real `rm -rf` leaves you
unable to run anything; a rename is reversible in one command. `--really-delete`
opts in.

The backup PRESERVES relative paths (`.erased-<stamp>/rootfs/<ver>`,
`.erased-<stamp>/runtime/sysroot`) so restoring is a single `cp -a`. The first
version flattened everything into one directory, which lost where each piece came
from and made the printed restore command wrong.

Never touches `games/` or `sources/`.

## AppImage

`tools/build-appimage.sh` assembles `build/Tadpole.AppDir`, which is runnable as
`AppRun` without packing. It calls `appimagetool` if present and otherwise says
what to install — appimagetool needs squashfs-tools, and neither is here.

What is bundled: the viewer, the cross-compiled ARM shim libraries, tadpole.sh,
tools/, and SDL2.

What is NOT, and why:

* **OpenGL and X11** — bundling those breaks against the user's driver. Every
  AppImage relies on the host for the graphics stack.
* **qemu-arm** — large, with its own library tail, and packaged everywhere.
  `AppRun` checks for it and prints the exact package name per distribution.
* **runtime/libs and runtime/sysroot** — GENERATED. `runtime/libs` is a directory
  of ABSOLUTE symlinks into wherever the firmware was extracted, so shipping it
  would bake in the build machine's paths.

### Building one

```sh
./tools/build-appimage.sh          # assembles build/Tadpole.AppDir
```

The AppDir is runnable immediately as `build/Tadpole.AppDir/AppRun` — packing is
only for distribution. To pack it you need `appimagetool` AND `squashfs-tools`
(appimagetool shells out to `mksquashfs`); the script says so and exits cleanly
rather than half-producing a file.

Verified end to end with `XDG_DATA_HOME=/tmp/xdgtest`: the data directory is
seeded, the viewer starts from it, and `TADPOLE_PROJECT` points at it.

An AppImage is read-only and Tadpole writes a lot, so `AppRun` seeds
`$XDG_DATA_HOME/tadpole` with the program parts and runs from there, with
`TADPOLE_PROJECT` pointing at it. A stamp file means an upgraded AppImage
refreshes the program without touching firmware, games or settings.

`TADPOLE_PROJECT` now OVERRIDES the argv[0]-derived project directory, which is
what makes this possible — and it is also how one tree's viewer can be tested
against another tree's data.

Verified end to end: data directory seeded, viewer running, `TADPOLE_PROJECT`
correct.

## Documentation tone

The legal section was rewritten from prohibitive to descriptive, because Tadpole
is GPL software and a free-software licence cannot add usage restrictions on top.

It now describes working with the files already on your own device, states that
copyright in the system software and games is unaffected and that what you do
with your copies is your responsibility, and carries an AS IS warranty
disclaimer. It gives no links to vendor servers or archives and asks
contributors not to add any. Same change in the wizard's page 2.

`README.md` references a `LICENSE` file that does not exist yet — choosing GPLv2
or GPLv3 is the author's decision, not one to make on their behalf.

## The wizard vanished when you picked a firmware archive

Reported: selecting `LFC_downloads_full.zip` in the setup wizard made the whole
wizard disappear.

It did, and there was nothing to replace it. `fb_open()` sets `g_modal = M_FILES`
over the wizard, `fb_enter()` then sets `g_modal = M_NONE` and emits the action,
and `tool_run()` spawned the installer as a background child whose output went to
the viewer's stdout — invisible behind the window. So the user saw setup abandon
itself, with no way to tell working from finished from failed. An install takes
minutes: unzip, scan 70 packages, extract a 53 MB UBIFS volume.

Three parts to the fix.

**A progress modal.** `ui_progress_begin/line/done` and `M_PROGRESS`. The tool's
output is captured and shown live, nine lines at a time, scrolling.

**A moving bar, deliberately not a percentage.** The steps have wildly different
and unknowable durations, so a percentage would be fiction. The bar says "still
working"; the log lines say what it is working on.

**Close is DISABLED while it runs** — both the button and Escape — so a
half-extracted rootfs cannot be walked away from.

`spawn_script()` grew an optional pipe carrying the child's stdout and stderr,
set non-blocking so the UI keeps drawing, drained a line at a time each frame.

### The browser now returns where it came from

`g_fb_return` remembers whether the browser was opened from the wizard, so
Cancel and a finished install both go back to setup rather than to an empty
screen.

### And it can pick a DIRECTORY

`install-firmware.sh` accepts an LFC_Downloads folder or a single archive, and
the README documents the folder — but a file browser that only ever opens
directories cannot express "I mean this one". There is now a **Use folder**
button, shown when the browser has no extension filter (i.e. when picking
firmware).

## ubi_reader needs lzallright, and hiding its error cost a run

With ubi_reader installed the extraction still failed, and the script said only
`ubireader_extract_files failed`. The real message was three frames deeper:

```
ModuleNotFoundError: No module named 'lzallright'
```

The LeapPad's UBIFS volume is LZO-compressed, and `ubireader/ubifs/misc.py` does
a hard `from lzallright import LZOCompressor` with no fallback — so ubi_reader
alone is not enough. A one-line fix, invisible because the script sent stderr to
`/dev/null`.

It now captures the tool's output, prints it on failure, and recognises this case
specifically to name the package. **Never discard a subprocess's stderr on the
failure path**; the generic message is always worse than the real one.

README lists `lzallright` alongside `ubi_reader` for the same reason.

### Still unverified

Extraction past that point has never run to completion here, so the rest of
`install-firmware.sh` — locating the root inside ubi_reader's output tree,
copying it into `rootfs/<version>/ubi_rfs`, and the content-package pass — is
written but untested against real output.

## Dependency checking: report the whole list, not one at a time

Installing firmware failed three times in a row, each on a different missing
Python module:

```
ModuleNotFoundError: No module named 'lzallright'
ModuleNotFoundError: No module named 'cryptography'
```

ubi_reader imports its dependencies LAZILY, so each one surfaces only when
extraction is already running — minutes in, after the zip is unpacked and 70
packages scanned — and the first failure hides the next. Three round trips for
something knowable at the start.

`tools/check-deps.sh` checks everything at once and prints one install command
for the detected distribution. `install-firmware.sh` runs it before doing any
work, so it now fails in two seconds with the full picture instead of minutes in
with a fragment.

### Getting the list right, rather than guessing it

The authoritative source is ubi_reader's own package metadata:

```
Requires-Dist: cryptography (>=44.0.2,<49.0.0)
Requires-Dist: lzallright   (>=0.2.1,<0.3.0)
Requires-Dist: zstandard    (>=0.25.0,<0.26.0)
```

Walking the imports statically found the same three plus two false positives
(`ConfigParser` in a Python-2 script path, and a word matched out of prose), so
metadata beats regex here.

Which are AUR and which are in Arch's official repos was checked with
`pacman -Si`, not assumed:

```
extra: python-cryptography, python-zstandard
AUR:   python-ubi-reader, python-lzallright
```

The first version suggested `yay -S python-cryptography`, which sends people to
the AUR for a package sitting in `extra`.

### Note for the future

`cryptography` is needed only for UBIFS ENCRYPTION, which the LeapPad image does
not use — `ubifs/decrypt.py` is imported unconditionally regardless. Nothing to
do about it from here, but worth knowing that the dependency is not doing any
work for us.


## The lock deadlocked the front end against its own child

Reported: Run System Menu failed with

```
tadpole: another instance (pid 3653998) is using /tmp/tadpole
```

Front-mode `tadpole.sh` took the lock and then waited. The viewer's Run System
Menu spawns `tadpole.sh --no-viewer --boot`, and that child hit its own PARENT's
lock and refused. Two processes deadlocking over a directory, one of which was
running no guest at all.

**The lock protects a running GUEST, not the directory.** Front mode now takes no
lock. `--boot` still does, and still exports `TADPOLE_LOCK_PID` so a viewer it
starts does not mistake its own launcher for a guest.

Verified both halves: front mode leaves no lock file, and a child launch
afterwards reaches AppManager.

## Booting is now gated on the system files existing

The same report noted that Run System Menu was OFFERED with no sysroot, which
then failed in a terminal the user may not be looking at — exactly what the
wizard exists to prevent. `struct mitem` grew a `needs_sys` flag; Run System
Menu, Launch .swf and Erase require it.

The check is cached (it stats the filesystem, and is asked once per item per
frame while a menu is open) and invalidated by `ui_invalidate_prereqs()` when a
tool finishes, which is the only thing that can change the answer.

Measured both ways: ENABLED with the system files present, greyed out with the
sysroot removed.

## The wizard could report a problem it could not fix

Page 3 said "Sysroot not built" and offered only Browse — which installs
firmware, not a sysroot. The two can get out of step (an interrupted install, or
an Erase) and then the page was a dead end.

There is now a **Build sysroot** button, shown only when the rootfs exists and
the sysroot does not, running `runtime/setup-sysroot.sh`. Confirmed that script
works standalone against an existing rootfs.

## The AppImage packs now

`appimagetool` and `squashfs-tools` became available, and the whole path works:

```
build/Tadpole-x86_64.AppImage    1.3 MB
```

Verified by running the PACKED file, not just the AppDir: it seeds
`$XDG_DATA_HOME/tadpole`, starts the viewer, and sets `TADPOLE_PROJECT`. The
refresh path was checked too — an existing data directory with an older stamp
gets the new program files while firmware, games and settings are left alone.

1.3 MB is small because the emulator IS small: the guest shim libraries are a
few hundred KB of ARM code and the viewer is one binary. Only SDL2 is bundled;
GL, X11 and qemu-arm come from the host.

## A fresh firmware install does not boot yet — what is fixed and what is not

The wizard path now runs end to end: extraction works, packages install, the
sysroot builds. **AppManager starts and runs deep into startup**, then the
12-second shutdown timer fires and the boot ends at the shutdown screen.

### Fixed on the way here

**`runtime/libs` was never generated by anything.** It is 168 symlinks into the
rootfs and it is on `LD_LIBRARY_PATH`, but it had been made by hand early in the
project — nothing in the tree recreated it. A fresh install therefore had an
empty `libs/` and could not start:

```
AppManager: can't load library 'libVideoMPI.so'
```

An error that points at a library rather than at the missing directory.
`setup-sysroot.sh` now builds it from `/lib`, `/usr/lib`, `/LF/Base/lib`,
`/LF/Base/Brio/lib` and `/LF/Base/Flash/lib`. Verified against the hand-made
original: same 168 entries, nothing missing.

**Execute bits.** `ubireader_extract_files` only preserves permissions with
`-k`, which requires root, so everything extracts 0644 — including AppManager,
which then fails with a baffling "Exec format error". Rather than demand root,
`install-firmware.sh` marks ELF files and shebang scripts executable. Checked
against the known-good tree: reproduces 537 of its 538 executables, the miss
being a `meta.inf` that nothing runs.

**`TADPOLE_DEBUG=0` turned debug ON, in every run ever.** `tadpole.sh` passed
`${debug:+-E TADPOLE_DEBUG=$debug}`, and `${x:+...}` expands for `"0"` because it
is non-empty; the shim then tested presence rather than value. One headless boot
produced **2 112 816 log lines**. Both sides now test the value: 244 lines.

**No backoff on the audio FIFO.** With no reader, `open_fifo()` retried on every
write — the bulk of those 2.1 million lines. It now retries every 64th attempt.

**`setup-sysroot.sh` had the same hardcoded rootfs path** that `tadpole.sh` did
(`stock-4.6.0.784/1221351650/ubi_rfs`), so "Build sysroot" failed with "no
rootfs" on a machine that plainly had one. Both discover it now.

**The AppImage stamp was a timestamp**, so editing a tool without rebuilding left
users on the old copy — which is exactly how the fixed `setup-sysroot.sh` failed
to reach the AppImage. It is a content hash now.

### The remaining difference, and the next thing to check

The freshly extracted rootfs has **1611 files against the known-good tree's
1635**. All 24 missing files are in one directory:

```
LF/Base/LST3-0x0017000B-000004/    libAccelerometerMPI.so, libButtonMPI.so,
                                   libCameraMPI.so, libGameViewFrame.so, ...
```

Symlink counts match exactly (301 each), so this is not a general extraction
failure — one directory is absent. Whether ubi_reader missed it or it was added
to the original tree by other means is NOT established, and that is the first
thing to determine: if the firmware image genuinely contains it, this is an
extraction bug; if not, the known-good tree has provenance the wizard cannot
reproduce and the wizard path can never match it.

Note that directory is NOT on the library search path (the original
`runtime/libs` draws from five other directories), so it is not obviously the
cause of the shutdown — but it is the one concrete difference between a tree that
boots and one that does not.

### Also unresolved

Announcing external power repeatedly rather than once did NOT prevent the
shutdown timer, so the race theory is unconfirmed. The announcement is still
worth keeping — sending it once was a genuine race — but it is not the cause.

## CORRECTION: the "12-second shutdown timer" was never real

Everything above about a shutdown timer firing on every boot describes a
symptom that did not exist. A long bisection ran on that premise — restoring
`LST3-0x0017000B-000004`, testing `--debug`, repeating the power announcement,
restoring the whole known-good rootfs — and each result was read as "still
shuts down", which was taken as evidence of a regression in the shim.

The emulator was booting the entire time. Nothing in that section should be
treated as a finding, and the power-announcement change it produced is
unmotivated by anything observed.

The lesson worth keeping: **confirm the symptom is reproducible before
bisecting for its cause.** Three eliminations in a row that all "fail" the same
way is a signal the premise is wrong, not that the next hypothesis is due.

## User accounts — SOLVED by transplanting real `/LF/Bulk`

Account creation appeared to succeed but produced a nameless profile with the
default background, and settings did not persist. Fixed on the device side: a
LeapPad2 in developer mode, FTP, and the whole of `/LF/Bulk` copied into
`runtime/sysroot/LF/Bulk`. Accounts and settings then work and save.

That dump is now the most valuable artefact in the tree and it is not
reproducible from firmware — `setup-sysroot.sh` is careful (`mkdir -p`
throughout, `[ ! -f ]` around `UIData.json`) and will not clobber it, but
`erase-firmware.sh` moves the entire sysroot including Bulk. Keep a copy
outside the project.

What Bulk supplies that a generated sysroot does not is still undiffed, and is
the path to making account creation work natively rather than by transplant.

## Crash reports: `tadpole_crash.c` and `tools/crash-triage.py`

79 of the 112 installed packages are native Brio apps (`App.so`) sharing one
engine — LST3 33, MULT 21, PADS 13, LPAD 11 — against 15 Flash titles and one
on the newer Rio runtime (`App.Rio.*.so`). At that ratio a single fault in a
shared library reproduces across dozens of unrelated titles, so the question is
never "did it crash" but "did it crash in the SAME PLACE".

### Why the cores are useless, and what to do instead

qemu writes a core on every guest fault, but it cannot be symbolised:

* every `r-x` segment in it has `p_filesz == 0` — qemu does not dump executable
  file-backed pages, so the code the PC points into is not in the file
* there is no `NT_FILE` note, so there is no mapping table either
* `elf_siginfo.si_signo` is left zero; the signal is in `pr_cursig`
* gdb resolves the guest's recorded paths against the HOST filesystem, so guest
  ARM libraries appear as x86-64 objects

Inside the process all of this is available from `/proc/self/maps`, so the shim
catches SIGSEGV/BUS/ILL/FPE/ABRT, resolves the addresses itself, and re-raises
so qemu still writes its core. Reports go to stderr and to
`$TADPOLE_DIR/crash.log`, which is not wiped between launches.

    === tadpole: guest crashed ===
      signal   SIGSEGV (11)
      cwd      .../ProgramFiles/LST3-0x00180025-000000
      fault    0x00000000  <unmapped>
      pc       0x4a2b1c40  libLightningBase.so+0x0004bc40
      lr       ...
      stack (executable words, most recent first):
        ...

`cwd` identifies the title: AppManager `chdir()`s into the package directory
before calling `CreateApp`, which is also why stray core files land inside the
game's own folder.

The backtrace is a STACK SCAN, not an unwind — the guest libraries carry no
usable frame pointers or `.ARM.exidx` — so it reports every stack word pointing
into executable memory. It over-reports, because dead values from earlier calls
survive, but it reliably contains the real chain, which is enough to tell
whether two crashes share a path. Verified against a purpose-built three-deep
test: `_start -> level_one -> level_two -> level_three`, all four recovered.

### Three traps this hit while being written, all silent

* **`sigaction()`'s struct layout is a C-library build choice.** libc puts a
  `sigset_t` between the handler and the flags, and its size (glibc 1024 bits,
  kernel 64) is not knowable from here. Guess wrong and `sa_flags` lands at the
  wrong offset, so `SA_SIGINFO` never reaches the kernel — the handler is then
  called as a plain `void(int)`, its third argument is garbage, and the first
  read of the ucontext faults. `SA_RESETHAND` is lost the same way, so the
  second fault goes straight to `SIG_DFL` and the process dies with no output
  at all. Use `rt_sigaction` (syscall 174) directly: handler, flags, restorer,
  64-bit mask, with `sigsetsize` passed explicitly.
* **Do not call the shim's own hooked `open()`.** It rewrites absolute paths
  into the sysroot, so the report went to `<sysroot>/tmp/tadpole/crash.log` and
  failed to create, and reading `/proc/self/maps` through it fails identically,
  which loses every symbol. The shim passes its `real_open` in.
* **No division.** ARM has no divide instruction here and the shim links
  `-nostdlib`, so `v / 10` becomes a call to `__aeabi_uidiv`. Nothing in the
  shim's own dependency set provides it — it resolves at runtime only because
  AppManager happens to load `libgcc_s.so.1` — so formatting decimals uses
  repeated subtraction of powers of ten instead.

### Using it

    ./tools/crash-triage.py                  # $TADPOLE_DIR/crash.log

Groups by faulting library+offset, names the affected titles, and prints
per-library totals underneath to catch the looser case of one library faulting
in several places. Play normally; the log accumulates.

### What it does NOT catch

White screens and softlocks raise no signal. `SIGABRT` covers failed
assertions, which is the common "asserts at startup" case, but a genuine hang
needs a watchdog and is a separate problem.

## LeapDog — differential tracing against real hardware

`tools/leapdog.py`. Compares what a real LeapPad2 does against what Tadpole
does, using a channel that costs nothing to open.

### The serial console

The device has no accessible console over USB — plugging it in puts it into a
"computer connected" mode. The way in is UART: wires onto a cartridge edge and
a CP2102 at **115200 8N1**, which gives a root shell and the full boot log with
no USB involvement at all, so the connected screen never appears.

For the record, the USB path was mapped before the serial hack made it moot:

    vbus-monitor        watches an input device named "LF2000 USB"
      -> vbus-actions 1    avahi-autoipd --refresh usb0
                           /etc/init.d/dftpdevice start
    dftpdevice          writes /tmp/usb_events_socket
    libUSBDeviceMPI     /LF/System/USBDevice, /tmp/monitoring_socket
    AppManager          CUSBDeviceMPI::GetUSBDeviceState()

`vbus-actions` starts DFTP separately from networking, and `usbether`,
`telnetd` and `vsftpd` are independent init scripts — so stopping
`dftpdevice` and `vbus` should leave telnet up with the device usable. Untested;
serial made it unnecessary.

### Why the console is a free differential channel

AppManager and the Flash UI narrate themselves:

    trace:  ----------HomePickerState::KeyDown----------
    [0x200] SystemPlugin::ResetTouchscreenSampleRate(): false

Tadpole runs the same binaries and emits the same lines. No injection, no
patched libraries, no risk to the hardware.

### Two failure modes, two instruments

`tadpole_crash.c` catches anything that raises a signal. It is blind to white
screens and softlocks, which hang without dying. Those produce *silence*, and
silence is measurable — hence `--stall`, which timestamps every line and flags
gaps. Between them the two cover both halves of the bug list.

### Use `term`, not `capture`

Two readers on one serial port split the byte stream, so each gets a random
half and the log looks corrupted rather than contended — the same trap as two
guests holding the ev2 FIFO. `term` replaces minicom instead of competing with
it: it forwards the keyboard too, so the shell stays usable. Quit with Ctrl-].

    sudo ./tools/leapdog.py term -o dev.log        # or join group uucp once
    ./tools/leapdog.py stalls dev.log --min 2
    ./tools/leapdog.py diff dev.log emu.log --in-game

### ANCHOR IN-GAME, or the diff is worthless

The device and the emulator do not have the same titles installed, so boot and
the home screen differ for reasons already understood — different icon counts,
a different app enumeration. Diffing from t=0 reports the icon list as the
first divergence and buries the real one. `--in-game` anchors on
`LaunchApp|ReplaceTopApp|LoadNewApp` and drops icon chatter; `--from`, `--to`
and `--ignore` are the general forms.

Only the FIRST divergence has a cause. Everything after it is consequence.

### What will legitimately differ, and what will not

The game is the same ARM binary executing the same instructions, so the call
sequence is deterministic until it reads something back. Divergence traces to
what we feed it: `glGetString(GL_EXTENSIONS)` steering a different render path,
`eglChooseConfig` picking another visual, a nonzero `glGetError` where hardware
returns zero, and frame timing — a racing game integrates physics against frame
delta, so at 57 fps against 30 every matrix downstream differs legitimately.

So compare in three phases, not one:

| phase | expectation | value |
|---|---|---|
| init / setup | near-identical | highest — extension negotiation, config choice, texture upload; deterministic, no timing |
| per-frame structure | same call sequence | state-machine bugs: a missing enable, a texture bound at the wrong moment |
| argument values | will differ | first frame only; after that timing makes it meaningless |

### No Python on the device

Confirmed on the serial console. Anything that must run on the LeapPad itself
has to be busybox `sh` or a compiled ARM binary. LeapDog is entirely host-side,
and `pyserial` is deliberately not a dependency — `stty` configures the line and
the device node is then just a file.

## 3D surface pitch — REAL DIVERGENCE, but the obvious fix is WRONG (reverted)

The first substantive divergence between a real LeapPad2 and Tadpole running
Clam Prix, captured with `tools/leapdog.py` over the serial console and
anchored at the app launch:

    device    [0x5] CreateHandle: 0xaa5a0: 320x240 (1280) @ 0x408e5000
    emulator  [0x5] CreateHandle: 0xab7e8: 320x240 (1920) @ 0x82400000

Both agree the 3D surface is 320x240. They disagree on its PITCH. 1280 is
320 x 4, tightly packed. 1920 is **480** x 4 — the full panel width. Tadpole
was handing the game the 2D layer's stride for a 320-wide surface, so every
row of 3D output landed 640 bytes further along than the game believed.

`fill_fix()` had `line_length = g_w * (g_bpp / 8)` unconditionally, for every
layer, despite the shim already tracking each layer's width in
`layer[].win_w` from FBIOPUT_VSCREENINFO.

### Why the viewer had to change with it

The viewer computed its own pitch the same way, `w * bpp/8`, in both the
compositor and the HLE blit. Fixing only the shim would have desynchronised
them; worse, 2D titles that work today set `win_w` for the ViewFrame viewport,
so recomputing a pitch on the host could have broken what already works.

So the shim now RECORDS what it told the guest, in a new
`layer_state.line_length`, and the viewer reads that. Neither side derives the
number independently, so they cannot disagree regardless of the order in which
the guest issues PUT_VSCREENINFO and GET_FSCREENINFO. Zero means "the guest
never asked", and the viewer falls back to the panel width, so the change is
inert until a layer is actually configured narrower.

Verified at the ioctl boundary with a purpose-built ARM test rather than by
running the game:

    before PUT (full width): line_length=1920      unchanged, 2D unaffected
    after  PUT 320x240:      line_length=1280      matches hardware

### Still open from the same capture

* `ExitPopUnloadApp: OGL context still active after unloading` appears in the
  emulator and NEVER on hardware. Same family as the `/tmp/3dlockup` reboot
  LeapFrog shipped for "unhandled exceptions that leave OGL locked up".
* The 3D surface lives at `0x408e5000` on hardware — ordinary mapped memory,
  allocated by the NEXEL GPU driver through EGL — but at `0x82400000` in the
  emulator, inside the framebuffer arena. Different allocation source, not yet
  understood, and possibly the deeper cause.
* `OpenGL ES vendor` is `NEXEL` on hardware and `Tadpole` here; extensions are
  empty on both, which is the important half.
* `DaemonControl socket connect failed ret=-1` is emulator-only.

### Traps in reading these logs

* Two readers on one serial port split the byte stream. The first 40 seconds
  of the first capture are interleaved garbage because minicom was still
  attached — the log looks corrupted rather than contended.
* The device and the emulator do not have the same titles installed, so the
  home screen diverges for known reasons. Anchor on the app launch or the diff
  reports the icon list as the first divergence.
* Free-memory figures in `LoadNewApp: before/after` differ by three orders of
  magnitude (12820 KB against 1152612 KB) and are pure noise.

### THE FIX ABOVE WAS REVERTED — it regressed scaling across many titles

Deriving `line_length` from `layer[].win_w` is wrong, and the ioctl-level test
that "verified" it could not have caught why.

**`win_w` is the layer's ON-PANEL WINDOW RECTANGLE — the ViewFrame box — not
the width of its source buffer.** The shim stores both concepts in that one
field. For the 3D surface they coincide at 320. For every 2D title that scales
its viewport they do not: the buffer stays 480 wide while the window shrinks.
Keying the pitch off `win_w` told those games their rows were 1280 bytes apart
while they carried on writing them 1920 apart, and content rendered far too
large. Clam Prix was affected too. This is a bug that had been fixed long ago
and was reintroduced by this change.

Why the ARM test passed anyway: it asserted that the shim returns 1280 after a
PUT of 320x240, which it did. That is a test of the mechanism, not of the
premise — it never asked whether 320 was the source width or the window width,
which was the entire question. **A test built from the same wrong assumption as
the change cannot falsify it.**

The divergence itself is real and still unexplained: hardware genuinely reports
1280 for that surface and Tadpole reports 1920. A correct fix needs a source
width tracked SEPARATELY from the window rectangle, which the shared layer
state does not currently distinguish. Note also that on hardware the surface
lives in GPU memory (`0x408e5000`, allocated by the NEXEL driver through EGL)
rather than in the framebuffer arena (`0x82400000`), so the pitch may be a
symptom of the surface having a different owner entirely, not a cause.

Do not re-attempt this without first establishing which of the two widths the
guest actually uses for its writes.

## Native 3D titles were never using our OpenGL at all

`libopengles_lite.so`. That is the whole bug.

On the device it is a SYMLINK to `libGLESv1_CM.so` — the same library under a
second name — and native titles link that second name. Clam Prix's DT_NEEDED
says `libopengles_lite.so`, not `libGLESv1_CM.so.1`.

The loader resolves DT_NEEDED **by filename**. `runtime/shimlibs-gl/` shipped
`libGLESv1_CM.so`, `libGLESv1_CM.so.1`, `libEGL.so` and `libEGL.so.1` — and no
file called `libopengles_lite.so`. So the search fell through shimlibs-gl to
`runtime/libs/libopengles_lite.so`, which points at the STOCK VR5 driver.

It then fails to link, because our libEGL replaced the stock one and does not
provide the stock driver's C++ internals:

    symbol '_ZN3EGL6Object9OnReleaseEi': can't resolve symbol
    symbol '_ZN3EGL18g_CommandContainerE': can't resolve symbol
    symbol '__cxa_pure_virtual': can't resolve symbol

Native 3D titles have been running on a half-linked dead driver. Nothing our
GL implementation did — software rasteriser, HLE replay, any of it — was ever
reached by them. The EGL init lines in the log that say `vendor = Tadpole` come
from Brio's own DisplayMPI, which links `libGLESv1_CM.so.1` and so did get
ours; that is why the logs looked fine.

### Verified, not inferred

A test binary linked exactly as Clam Prix is (`-l:libopengles_lite.so`), run
under the emulator's real LD_LIBRARY_PATH:

    before:  symbol '_ZN3EGL6Object9OnReleaseEi': can't resolve symbol  (x17)
    after:   GL_VENDOR = Tadpole GLES 1.1

All 58 GL/EGL symbols Clam Prix imports are exported by our libGLESv1_CM +
libEGL, so with the alias present the game gets a fully resolved Tadpole GL.

The fix is one symlink, now created by a `gl-links` target rather than as a
side effect of the build recipe — the old arrangement only ran when the .so was
out of date, so a missing alias could not be restored by rebuilding, which is
exactly how this one stayed missing.

### Do not conclude 3D now works

The alias means our GL is finally *reached*. Whether it renders Clam Prix
correctly is a separate question, and the API-surface diff says we implement 94
of the device's 180 GL/EGL entry points. Clam Prix's own imports are covered,
but other titles' may not be — and the float/fixed split is suspicious: we have
`glFrustumx` but not `glFrustumf`, `glTexEnvx` but not `glTexEnvf`/`glTexEnvi`,
`glLightxv` but not `glLightfv`. Check imports per title before assuming.

## GL state survived across games — the "melting", and the resync flood

Reported as: a title renders as smeared bands ("on drugs, melts away"), and once
that happens it STAYS broken across relaunches and gets worse in other titles.
Accompanied by `[hle] host asked for a state resync` repeating without end.

One cause. **AppManager does not exit between games.** It dlopen()s the title's
`App.so`, runs it, and `UnloadModule()`s it, so every static in
`tadpole_gles_core.c` is process-lifetime. And the EGL teardown entry points
were no-ops:

    u32 eglTerminate(void *dpy)                 { return EGL_TRUE; }
    u32 eglDestroyContext(void *dpy, void *ctx) { return EGL_TRUE; }

Nothing ever freed a texture slot. `glGenTextures` names a texture after its
slot index and scans for a free one; with the previous title still holding all
of them it returned **name 0**, and a title drawing with name 0 renders
untextured. Each launch left the table fuller than the last, which is exactly
why the damage accumulated rather than merely persisting.

The resync flood is the same bug seen from the host. `want_tex_if_missing()`
sets `want_resync` before any draw whose bound texture has no image here, and
the guest's `hle_sync_state()` can only resend textures it still holds a
decoded copy of. A texture it never tracked can never be resent, so the request
is unsatisfiable and the next draw asks again — forever.

Brio had been reporting this all along, in the one log line that appears in
Tadpole and never on the device:

    ExitPopUnloadApp: OGL context still active after unloading

### The fix

`tad_gl_context_reset()` at the end of `tadpole_gles_core.c`, called from
`eglTerminate` and `eglDestroyContext`: frees every texture and buffer, clears
the array and binding state, and sends the new `TADGL_RESET` opcode so the host
drops its mirrors too. Clearing the host's `g_tex_have[]` is the important half
— a stale entry makes the next title's recycled name silently sample the
PREVIOUS game's image instead of asking for the right one.

Verified without needing the game, by doing what AppManager does:

    title A allocated 192 textures, then table full
      next alloc without teardown = 0      <- renders untextured
    -- eglDestroyContext --
    title B first alloc = 1                <- fixed

Caps raised at the same time, since Clam Prix alone loads ~11.8 MB of textures:
guest `MAX_TEXS` 192 -> 512, host `MAX_TEX` 256 -> 576, `MAX_BUF` 128 -> 256.
**The host's MAX_TEX indexes by guest name and must stay larger than the
guest's MAX_TEXS.** If `[gl] WARN glGenTextures EXHAUSTED` still appears in a
log, the cap is still too low — that warning was already being printed and is
the single most useful line for this failure.

### Also fixed: a stale frame at startup

The framebuffer arena is a plain file that the shim only ftruncates, so a fresh
launch opened showing whatever was on screen when the previous one closed.
Harmless in itself, but it makes a dead frame indistinguishable from a live one
when reading screenshots — which cost real time here. `tadpole.sh` now removes
`fb?.bin` alongside `audio.fmt`.

## Clam Prix race scene: what is noise and what is not

With the `libopengles_lite.so` alias and the context reset in place, Clam Prix
reaches `Scene::Initialize` and loads the full track — ~11 MB of textures, no
`glGenTextures EXHAUSTED`. Raising MAX_TEXS to 512 was independently worthwhile:
Letter Factory had been hitting that limit too.

### The RacingEngine warnings are NOT a lead

Checked against the hardware capture rather than assumed, and the counts are
identical:

| | device | emulator |
|---|---|---|
| `getInt32 with name groupid does not exist` | 16 | 16 |
| `getResource(skybox) does not exist` | 1 | 1 |
| `Scene::Initialize` | 2 | 2 |
| `loadCars` | 2 | 2 |

They are the game's own resource chatter and the real device prints exactly the
same. That the device log contains `loadCars` also means the hardware capture
DID reach the race, so there is a baseline to diff the race scene against.

### `GL error 0x0500` IS a lead

`GL_INVALID_ENUM`, raised during race scene init. A rejected call draws nothing
and raises no further symptom, so this is the strongest remaining candidate for
the black 3D.

`check_gl()` samples once per frame — correct for the steady state, since
glGetError is a pipeline flush point, but useless for diagnosis: "at frame"
means "somewhere among thousands of commands". Under `TADPOLE_HLE_DEBUG=1` the
replay loop now checks after EVERY opcode and names it, once per distinct
(opcode, error) pair so a call failing on every draw reports once:

    hle: GL error 0x0500 from TEXENV (op 10, len 12) at frame 37

`g_opnames[]` carries a compile-time size assertion against `TADGL_OP_COUNT`.
Add an opcode and forget the table and every name after the insertion point
shifts by one — a diagnostic that lies is worse than none.

Suspects for INVALID_ENUM, given the GLES1-to-desktop mapping: a `TEXENV`
pname/value combination desktop GL does not accept, a GLES-only enum forwarded
verbatim, or a client-array type (`GLES_FIXED` 0x140C) reaching a call that was
not converted.

## THE 3D bug, identified: client-side ELEMENT arrays are dropped

From a `TADPOLE_HLE_DEBUG=1` capture of a Clam Prix race:

    hle: drawelements elembuf=0 (MAX_BUF 256) data=(nil) size=0
    hle: frame 2580: 2 draws, 0/3528 px non-black
         | drawelem pkts 2566 skipped-nobuf 2439

**2439 of 2566 draw calls are skipped.** `elembuf=0` means no
GL_ELEMENT_ARRAY_BUFFER is bound: the racing engine passes its indices as a
CLIENT-SIDE array. `TADGL_DRAWELEMENTS` carries only a buffer name and an
offset, so the host has nothing to draw from and skips the call. The scene
renders nothing, which is the black 3D.

This is the same class of problem already solved for VERTEX arrays — client
arrays are uploaded to reserved buffer names 100-103 — and simply never done
for the element array. The fix is symmetric: upload the index data to a
reserved name at encode time and reference it.

The resync flood is downstream of this, not separate: every skipped draw calls
`want_tex_if_missing()`/sets `want_resync`, and no resend can satisfy a request
caused by a missing index buffer.

Also seen in the same capture, lower priority:

* `GL error 0x0500 from TEXPARAM (op 30, len 8)` — a glTexParameter pname or
  value desktop GL rejects.
* `GL error 0x0500 from ENABLE (op 4)` and `from DISABLE (op 5)` — a GLES-only
  capability forwarded verbatim. `glEnable(0x0B44)` (GL_CULL_FACE) is already
  special-cased and ignored; something else is not.

## CORRECTION: the context reset must be DEFERRED, not immediate

Resetting on `eglTerminate`/`eglDestroyContext` crashed the guest:

    [hle] encoding to host GPU
    hle: context reset — mirrors dropped
    qemu: uncaught target signal 11 (Segmentation fault) - core dumped

Three runs out of five, on sign-in and the home screen. **Brio keeps using GL
after destroying a context**, so freeing the texture and buffer tables there
hands it dangling pointers. Real hardware survives because its driver keeps the
objects alive until the memory is genuinely reclaimed.

Destruction now only sets `g_reset_pending`; the tables are cleared at the next
`eglCreateContext`, when the old context is gone and the new one has not been
handed out yet. The cross-game protection is unchanged — verified with the same
ARM test, extended to the real sequence:

    title A allocated 512 textures, then table full
      next alloc without teardown = 0
    -- eglDestroyContext --
    -- eglCreateContext (next title starts) --
    title B first alloc = 1

**The lesson: a teardown hook is not permission to free.** Guest code may hold
references past the call that nominally ends their lifetime, so reclaim at the
start of the next lifetime instead of the end of the previous one.

## tadpole.sh clears the runtime directory on every launch

`rm -rf "$TADPOLE_DIR"`, plus reaping guests still bound to it, but ONLY when
this invocation owns the viewer (`use_viewer=1`). The child spawned by "Run
System Menu" (`--no-viewer --boot`) must skip it: removing the arena while the
parent's viewer has it mapped gives the guest a NEW inode and leaves the viewer
reading the old unlinked one — black forever, home screen included.

The reap excludes our own ANCESTORS, not merely `$$`. The pattern appears in the
command line of whatever launched us, and killing a parent kills the pipeline.

## Buffer-object exhaustion — the actual cause of black 3D in races

`MAX_BUFS` was **64** on the guest, and `buf_slot()` returned NULL above that
SILENTLY — unlike glGenTextures, glGenBuffers said nothing at all. The chain,
read straight out of a `TADPOLE_HLE_DEBUG=1` capture:

1. the race scene allocates 64 buffers and fills the table
2. `glGenBuffers` returns **name 0**
3. `glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0)` clears `g_bound_elem`
4. `glBufferData` finds no slot and stores nothing
5. `glDrawElements(..., NULL)` sends `elembuf=0`
6. the host has no indices and skips the draw

    hle: drawelements elembuf=0 (MAX_BUF 256) data=(nil) size=0
    frame 2580: 2 draws, 0/3528 px non-black
                | drawelem pkts 2566 skipped-nobuf 2439

2439 of 2566 draws discarded. The giveaway was in the host's mirrored-buffer
dump: it stops dead at name 64.

Client-side element arrays were NOT the problem — that path already exists
(`HLE_CLIENT_IDX`) and works. It simply never ran, because the guard is
`if (!ebuf && indices)` and `indices` was NULL.

Fixed: `MAX_BUFS` 64 -> 256, host `MAX_BUF` -> 1024, and glGenBuffers now warns
on exhaustion the way glGenTextures does.

**The reserved names had to move.** `HLE_CLIENT_VTX..IDX` were 100..103 only
because the guest's own names could never exceed 64. At MAX_BUFS 256 a real
buffer would have collided with a client-array upload — geometry and vertex data
overwriting each other, worse than the exhaustion being fixed. They are now
1000..1003, with a compile-time assertion that they stay above MAX_BUFS.

## Symbol completeness became mandatory, and Pet Pals 2 proved it

    AppManager: can't resolve symbol 'glLightx'

Pet Pals 2's startup logo went black after the `libopengles_lite.so` alias
landed. Before the alias those titles fell through to the stock driver, which
exports all 180 entry points — dead, but PRESENT, so they always loaded. Against
our library one missing entry point is fatal at load time.

`tools/gen-gl-stubs.py` regenerates `tadpole_gles_stubs.c` from the difference
between the device's exports and our built library's. It reads what we have from
the SYMBOL TABLE, not by scanning source: an earlier source-regex version
claimed `glGetIntegerv` was missing when we plainly export it, and emitting a
stub for it would have broken the link with a duplicate symbol.

    device exports        180
      real implementations 64
      no-op stubs          115
      MISSING ENTIRELY     0

### A stub is not an implementation, and the numbers say so

64 real out of 180. The tool prints, separately, the entry points that are
stubbed but need real behaviour — a stub lets a title load and then draw the
wrong thing, which is harder to notice than a failure:

    glColor4f  glCullFace  glFrustumx  glLightxv  glNormalPointer
    glReadPixels  glScissor  glShadeModel  glTexEnvfv

**`glFrustumx` is a no-op.** The projection matrix is never set, which on its own
is enough to explain geometry that never appears. That is the next thing to fix,
along with `glFrustumf` (newly stubbed, also needed).

## Perspective: glFrustum was a no-op, and it looked exactly like that

With the buffer cap raised, a Clam Prix race finally rendered geometry at 26 fps
— flattened into a thin horizontal band across the top of the viewport, scenery
strung out along a single line.

That shape IS the diagnosis. `glFrustumx` was a no-op stub and `glFrustumf` did
not exist at all, so the projection matrix was never multiplied in: the pipeline
stayed orthographic, every depth collapsed onto one plane, and the scene
squashed into a strip. No amount of fixing buffers or textures could have helped
while the projection was missing.

`frustum_f()` now builds the standard GL frustum matrix and `mat_apply()`s it,
mirroring `ortho_f()` beside it, and forwards to `hle_frustum` — the wire
opcode and the host handler for `TADGL_FRUSTUM` already existed and had simply
never been fed. `glFrustumx`, `glFrustumf` and `glFrustumfOES` are all real now;
the OES spelling matters because titles link whichever their SDK emitted and a
stub there flattens the scene just as thoroughly.

Guard: near and far must be positive and the three ranges non-empty. A zero
divisor would poison the whole matrix stack instead of failing visibly.

## Texture flicker: deletions were never mirrored

Reported as textures flickering constantly during a race. Two halves, both
silent:

* **The guest never told the host about a deletion.** `glDeleteTextures` freed
  its own copy and sent nothing, so the host's mirror outlived the texture.
  Names are handed out by slot index and Clam Prix cycles textures continuously
  during a race — its own log is full of `Release Texture` / `Load Texture` —
  so a name was reused almost immediately and any draw between the delete and
  the next upload sampled the PREVIOUS texture's pixels.
* **The host's delete handler never cleared `g_tex_have[]`.** It destroyed the
  GL object and zeroed `g_tex[n]` but left the have-flag set, so
  `want_tex_if_missing()` believed an image was still present and the draw
  sampled nothing rather than asking for the new upload.

`hle_deletetexture()` had existed in the encoder and been declared in the core
all along. It was simply never called — the same shape of bug as
`libopengles_lite.so`: the mechanism was built and never wired up.

## NEXT: why the player character does not render

State on stopping: Clam Prix races render — track, banners, kart, perspective,
culling, texture matrix all correct. The PLAYER character (SpongeBob) is absent.
Squidward and other AI drivers reportedly DO render, which is the single most
useful fact available: the CPU skinning path works, so this is an ordinary bug
in our code and not the missing GL_OES_matrix_palette extension.

### 1. Look at the palette entries the draw ACTUALLY uses  (do this first)

The measurement that produced "the matrix is corrupt" printed `g_palette[0]`
and `g_palette[1]`. The skinned draw uses bones **3 and 2**. Those are different
matrices and were never examined. The whole "float bits in a fixed-point entry
point" line of inquiry rests on entries the character does not reference, and
may be a red herring end to end.

Print, at the first skinned draw: `g_palette[3]` and `g_palette[2]`, and the
skinned OUTPUT of vertex 0 beside its input. Input was 0.317, -0.323, 1.449; if
the output is a similar magnitude the blend is fine and the problem is
elsewhere, and if it is enormous or zero the matrices are the problem.

### 2. Diff a working character against the broken one

Squidward renders and SpongeBob does not, in the same frame, through the same
code. Log every skinned draw with vertex count, bone count and the range of
palette indices touched, then compare. A player-only difference — more bones, a
higher palette index, a second UV set — falls straight out.

Reaching an AI driver needs a proper race rather than Driving School; the route
file will need a different menu path.

### 3. Verify the modelview at the moment that matters

`glLoadPaletteFromModelViewMatrixOES` copies `g_mv[g_mv_sp]`. A correct load was
observed at stack level 0 and the snapshot read level 7. Confirm the app really
is at depth 7 when it snapshots, by logging push/pop depth around the palette
loads. If our depth has drifted from the app's — a dropped push past
STACK_DEPTH, an unbalanced pop — we are copying the wrong matrix and everything
downstream is noise. STACK_DEPTH is 16 and `g_push_drop`/`g_pop_under` already
count violations but are never reported; print them.

### What NOT to repeat

* `pgrep -f <pattern>` matches the shell whose command line CONTAINS the
  pattern. It killed this session's own background jobs twice, silently. Match
  on a process NAME (`pgrep -x tadpole-view`) or let probe-race.sh do the
  reaping — it has the ancestor guard.
* The device's `glLoadMatrixx` and `glLoadMatrixf` are at different addresses,
  so the driver genuinely converts fixed-point. Do not "fix" our conversion.
* `glGetIntegerv` capability queries: answered now, and Clam Prix never asks.
  Ruled out.

### Other open threads, in rough priority order

* `GL error 0x0500 from TEXPARAM` — the last remaining GL error in a race.
* 108 GL entry points are still no-op stubs. Lighting and fog are the visible
  ones and are also why the host still filters GL_LIGHTING and GL_FOG.
* A segfault where tadpole_crash.c did NOT produce a report. A crash handler
  that silently fails is worse than none; find out why before trusting it.
* Matrix-palette skinning is implemented but unverified against hardware.
