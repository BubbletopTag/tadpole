# Leapster GS — porting notes

Working notes in the spirit of `ULTRA.md` and `LEAPPAD3.md`: what was
measured, what was inferred, and the command behind each.

**Nothing here has been booted.** This is a firmware survey, not a port. Every
claim below is either a measurement against the downloaded image or is marked
as an inference. No `runtime/devices/leapstergs.conf` was written and no code
was changed — the profile in §4 is a draft for whoever does the work.

Firmware surveyed: **3.6.2.305**, built 2013-11-06, `Device="LeapsterGSExplorer"`.
Board **LUCY**, platform family **LEX**, 320x240.

---

## The premise, in one line

The Leapster GS is **the LeapPad2 with a smaller screen**. Not "similar to" —
the same firmware line, built the same week, running byte-identical binaries
where it matters. `/usr/bin/app` is the same file. `AppManager` is the same
file. The whole OpenGL ES stack is the same file.

This is the opposite situation to the Ultra and the LeapPad3, both of which
were a different shell (Qt/AppServer) on a different SoC. Here the shell is
`AppManager`, the graphics are Brio + Lightning, and there is no Qt anywhere in
the image.

That makes it, on the evidence, the cheapest device Tadpole could add.

---

## 1. The manifest, and whether the firmware is still there

`EnglishLeapsterGSExplorer.xml` is an LFConnect package list of the same shape
as `EnglishLeapPad2.xml`: 50 unique package IDs across firmware, widgets, base
apps, UI themes, per-title patches and CDN content.

The interesting part is the bottom of the file. Where the LeapPad2 has one
firmware entry, the GS has **three**, one per NAND vendor, differing only in
erase/write geometry:

```xml
<!-- Micron -->  eraseSize="1048576" writeSize="4096"   GAM2-0x00210004-000000
<!-- Samsung --> eraseSize="1048576" writeSize="8192"   GAM2-0x00210004-001000
<!-- Hynix -->   eraseSize="2097152" writeSize="8192"   GAM2-0x00210004-002000
```

All three are live. So is everything else in the list.

### Probing it

`tools/fetch-firmware.py` cannot be pointed at a device it has no profile for —
it reads `runtime/devices/<id>.conf` for the CDN directory — so the probe was
run by importing the tool as a module and forcing `DEVICE_DIR`, which keeps the
candidate-URL order identical to what the shipped tool would try:

```python
# scratch script, not committed
import importlib.util, sys, time, os
TOOLS = ".../leappad-emu-3/tools"
sys.path.insert(0, TOOLS)
spec = importlib.util.spec_from_file_location("ff", TOOLS + "/fetch-firmware.py")
ff = importlib.util.module_from_spec(spec); spec.loader.exec_module(ff)
ff.DEVICE_DIR = "GAMFW"
for pid, desc, types in ff.packages("EnglishLeapsterGSExplorer.xml"):
    for u in ff.candidates(pid):
        n = ff.head(u); time.sleep(0.15)
        if n is not None: print("OK", pid, n, u); break
```

Result, 2026-08-17:

```
50 available, 0 not on the CDN, 188,251,008 bytes total (179.5 MB)
236 HEAD requests issued
```

**Fifty out of fifty.** The LeapPad3 had two dead packages; the GS has none.

The single-key check anyone can repeat without the script:

```
$ curl -sSI https://digitalcontent.leapfrog.com/packages/GAMFW/GAM2-0x00210004-000000.lfp
HTTP/2 200
content-length: 34215420
last-modified: Sun, 23 Feb 2020 15:31:26 GMT
```

### Where things live

`GAMFW` was already recorded as the GS's CDN directory in
`tools/fetch-firmware.py`'s comment and in `docs/ULTRA.md`. That is confirmed,
and it holds far more than the firmware:

| layout | example | what uses it |
|---|---|---|
| `packages/GAMFW/<id>.lfp` | `GAM2-0x00210004-000000.lfp` — 34,215,420 | the three firmwares, Surgeon, Bulk Empty |
| `packages/GAMFW/<id>.lf2` | `GAMS-0x00210001-000000.lf2` — 1,342,772 | **41 of the 50** — apps, widgets, themes, patches |
| `packages/<middle>/<id>.lf2` | `0x00210008/GAM2-0x00210008-300000.lf2` | four stragglers, mostly `MULT-*` |
| `packages/LFCC/<id>.lfp` | `LFCC-0x001A0003-100002.lfp` | three of the four `LFCC-*` content packages |
| `packages/GAMS/<id>.lf2` | `GAMS-0x00180004-DA0000.lf2` | one, the Wheel Works device asset |

That fourth row is worth flagging: three `LFCC-*` packages resolve under the
**four-letter system name** `LFCC`, which is `candidates()`'s third and last
fallback. It works today, but only because that fallback exists.

Sizes of the pieces a port needs first:

```
GAMFW/GAM2-0x00210004-000000.lfp   34,215,420   Firmware Base (Micron)
GAMFW/GAM2-0x00210004-001000.lfp   34,219,791   Firmware Base (Samsung)
GAMFW/GAM2-0x00210004-002000.lfp   34,177,611   Firmware Base (Hynix)
GAMFW/GAM2-0x00210003-000000.lfp    6,228,879   Surgeon
GAMFW/GAM2-0x00210004-000002.lfp       18,997   Firmware Bulk Empty
GAMFW/GAMS-0x00170030-000001.lf2   26,976,904   Language Pack EN
```

34 MB against the LeapPad2's firmware and 141 MB against the LeapPad3's. The
whole device — firmware, every app, every patch — is 179.5 MB.

### Good-guest note

Every request in this survey was an exact key derived from the manifest. 236
HEADs for the probe, then four GETs (one firmware, three small content
packages). No listing, no prefix walk, no filename guessing.

---

## 2. What the machine is

Downloaded and opened with the project's own tools:

```
$ curl -sS -o GAM2-0x00210004-000000.lfp \
    https://digitalcontent.leapfrog.com/packages/GAMFW/GAM2-0x00210004-000000.lfp
$ unzip -l GAM2-0x00210004-000000.lfp
        173  Firmware-Base/packagefiles.md5
        304  Firmware-Base/meta.inf
    2270472  Firmware-Base/4,2270472,kernel.bin
   53477376  Firmware-Base/5,53477376,C2G-E1M-W4K-erootfs.ubi
```

`.lfp` is a plain ZIP, and the payload is the LeapPad2's exact shape: a uImage
plus a UBI volume. `install-firmware.py` needs no change to read it — its
`find_firmware()` matches `Type="DiskImage"` / `Name="Firmware-Base"`, and its
extractor globs for `*erootfs*.ubi` and `*kernel.bin`, both of which hit.

The `C2G-E1M-W4K` in the volume name decodes against the manifest: **C**hip
**2G**B, **E**rase **1M**, **W**rite **4K** — the Micron row's `eraseSize` and
`writeSize` exactly. So the vendor variants differ in flash geometry only, and
any of the three yields the same rootfs.

```
Firmware-Base/meta.inf:
  Device="LeapsterGSExplorer"     <- this is what device.sh autodetect matches
  Type="DiskImage"
  ProductID=0x00210004
  Version="3.6.2.305"
  BuildDate="11/06/2013"
```

The kernel names the board outright:

```
$ file Firmware-Base/4,2270472,kernel.bin
u-boot legacy uImage, Angstrom/2.6.37+svn/lflucy, Linux/ARM,
  Load Address: 0X80208000, Entry Point: 0X80208000

$ file rootfs/stock-4.6.0.784/kernel.bin        # the LeapPad2, for comparison
u-boot legacy uImage, Angstrom/2.6.37+svn/valencia, Linux/ARM,
  Load Address: 0X80208000, Entry Point: 0X80208000
```

Same Angstrom, same kernel version, same load address, three months apart. Only
the board tag differs: `lflucy` against `valencia`.

Extraction is the ordinary path:

```
$ python3 -c "import sys; sys.path.insert(0,'tools'); import pkgtool; \
    pkgtool.cmd_ubi('Firmware-Base/5,53477376,C2G-E1M-W4K-erootfs.ubi','gsrfs')"
```

65 MB, 1712 files.

### ARM Linux, uClibc, Brio — the same runtime, not a similar one

```
$ file LF/Base/bin/AppManager
ELF 32-bit LSB executable, ARM, EABI5, interpreter /lib/ld-uClibc.so.0, stripped
```

The rootfs was compared file-by-file against the installed LeapPad2 image at
`rootfs/stock-4.6.0.784/ubi_rfs`. The results are the whole argument for this
port:

**`/usr/bin/app` is byte-identical.** `diff` reports nothing. Same
AppManager relaunch loop, same `vnotify 6` splash, same `/tmp/3dlockup`
reboot hack, same `/tmp/bam-wrapper` hook.

**`/etc/profile` is byte-identical.** Which also means the GS has no
`QWS_DISPLAY`, no tslib environment and none of the LeapPad3's twelve
load-bearing variables to transcribe. There is nothing to transcribe.

**`/etc/init.d/rcS` differs by three hunks, all trivial:**

```
24a25   > fbctrl /dev/fb1 set blank 1
47c48   < VideoDaemon 750 &        > VideoDaemon 750
76c77,81  the startup-chime else-branch became an if/else that plays the
          same file either way — a no-op edit
```

`/etc/init.d/*` have the same names on both. `make_dev_nodes.sh` and
`/etc/init.d/mounts` are byte-identical.

**`AppManager` and `VideoDaemon` are byte-identical** — 18,528 and 15,196
bytes, matching md5s. The shell binary Tadpole launches is literally the same
program it already launches.

**30 of 31 Brio libraries are byte-identical.** The exception is
`libImageIO.so` (16,088 against the LeapPad2's 18,208). Everything else —
`libKernelMPI`, `libDisplayMPI`, `libButtonMPI`, `libAudioMPI`,
`libCartridgeMPI`, `libavcodec`, `libtheora`, `libvorbisidec` — matches to the
byte.

**The entire OpenGL ES stack is byte-identical:**

```
usr/lib/libGLESv1_CM.so.1.1   1,948,724   md5 5f29dc66e5c2…   both
usr/lib/libEGL.so.1.4            47,700   md5 52a7938ecb0b…   both
usr/lib/libGLESv2.so.2.0        131,400   md5 c5f7e858563c…   both
usr/lib/libopengles_lite.so   1,948,724   md5 5f29dc66e5c2…   both
```

This matters more than anything else here. Tadpole's GLES shim and its HLE
forwarding were written, instrumented and hardened against *this exact binary*.
The GL work does not need redoing. (The vr5 GPU it drives is the same part —
the `/tmp/3dlockup` comment in the shared `/usr/bin/app` is about the same
hardware.)

**The libraries that do differ, differ by four bytes.** `libuClibc`,
`libasound`, `libdbus-1` all report `DIFFERS` on md5 and identical sizes;
`cmp -l` gives four differing bytes at one offset, which is a build stamp:

```
$ cmp -l usr/lib/libdbus-1.so.3.5.7 <LP2>/usr/lib/libdbus-1.so.3.5.7
174130 253 305
174131  35 233
174132 274 356
174133 146  24
```

Only `bin/busybox` differs substantively (539,284 against 547,484 — a different
applet config), and `usr/bin` is missing exactly three tools the LeapPad2 has:
`ipcrm`, `ipcs`, `ldd`.

**What is NOT there:** no Qt, no AppServer, no ConnMan, no wifi. `LF/Base/Qt`
does not exist; there is no `libQtGui*` anywhere in the image. A search for
wireless drivers, `wpa_supplicant` or any `*wifi*`/`*wireless*` path returns
nothing. The only D-Bus service in `usr/share/dbus-1/system-services/` is
`org.freedesktop.Avahi.service` — the same USB-ethernet "Connect" plumbing the
LeapPad2 has, and nothing more.

### The shell

`LF/Base/bin/` holds `AppManager`, `VideoDaemon` and a `BadWords` directory —
that is all. AppManager is an 18 KB stub; the actual logic lives in
`LF/Base/lib/libLightningBase.so` (its strings include
`LightningBase/Src/AppManager.cpp`).

The shell's own UI is Flash. `LF/Base/LUCY/` holds 54 `.swf` files —
`Splash.swf`, `EnterName.swf`, `Calibrate.swf`, `SelectTheme.swf`,
`Trailers.swf` — against the LeapPad2's 41 in `LF/Base/LPAD/`. Played by
`LF/Base/Flash/lib/libflashlite.so`, same as the LeapPad2.

The five `LF/Base/lib/` Lightning libraries are the GS's own builds (sizes
differ by tens to thousands of bytes), and `libGalleryIO.so` and
`libPlayerProfile.so` are byte-identical.

**How the shell knows which device it is on — and this is load-bearing.**
`libLightningBase.so` carries the strings for *all three* devices:

```
LPAD/main.swf   L3X/main.swf   LUCY/main.swf   LpadAssets_   LucyAssets_
```

so the asset root is a runtime choice, not a compile-time one. The selector is
in `LF/Base/Brio/lib/libUtility.so` — byte-identical on both devices — which
exports `LeapFrog::Brio::GetPlatformName()`, `GetPlatformFamily()` and
`GetPlatformID()`, and whose strings name what they read:

```
/sys/devices/system/board/platform
/sys/devices/system/board/platform_family
/sys/devices/system/board/system_rev
/sys/devices/platform/lf1000-gpio/board_id
/flags/board_id
```

So `DEV_PLATFORM=LUCY` in a GS profile is not decoration and not a guess about
what `rcS` wants — `rcS` is never run. It is the input to
`GetPlatformName()`, and it is what makes the shell load `LUCY/main.swf` and
`LucyAssets` instead of `LPAD/main.swf` and `LpadAssets`. Get it wrong and the
shell looks for a directory that is not in the image.

I did not disassemble `GetPlatformName()`, so the exact precedence between
those five paths is **not measured**. `/flags/board_id` looks like an override
and may matter.

### 320x240, landscape, and not rotated

Every boot asset in `/var/screens` is 320x240:

```
$ for f in var/screens/*.png; do echo "$(basename $f): $(file -b $f)"; done
Lucy-Boot-logo.png:      PNG image data, 320 x 240
Lucy-Connected.png:      PNG image data, 320 x 240
HEALTH_AND_SAFETY.png:   PNG image data, 320 x 240
LOW_BATTERY.png:         PNG image data, 320 x 240
… all 19 of them, plus 26 Lucy-sparkle-pngs frames, all 320 x 240
```

as is every full-screen asset in `LF/Base/LucyAssets/Art` (`UI_DarkMatte.png`,
`UI_BatteryCharging.png`, …).

**No `CW` variant exists, and that is the evidence for no rotation.** On the
LeapPad2 the panel is portrait, so the assets ship twice — the stored portrait
original and a quarter-turned copy with `CW` in its name:

```
Madrid-Boot-logo.png                    272 x 480    <- as stored
Madrid-Boot-logoCW.png                  480 x 272    <- turned
Madrid-Boot-StaticConnectScreen-01.png  272 x 480
Madrid-Boot-StaticConnectScreen-01CW.png 480 x 272
```

The GS ships one copy of each, landscape, with no `CW` anywhere in
`/var/screens`. Reading `Lucy-Boot-logo.png` directly confirms it: a LeapFrog
logo, upright, landscape. So `DEV_UI_ROTATE=0`.

### Touchscreen, buttons, audio

It **does** have a touchscreen. `rcS` runs `/usr/bin/setcal` unconditionally,
`setcal` writes to `/sys/devices/platform/lf2000-touchscreen`, and MfgTest
ships `calibrateLucy.swf` and `LinearityTestLucy.swf`.

Buttons: `usr/bin/recovery-functions` names the full set —
`UP DOWN LEFT RIGHT L R RED ESC A B M(Home) H(Hint) P(Pause) X(Brightness)`.
The viewer already has every one of these mapped (`tadpole_view.c` 162-179,
730-742). Nothing new is needed.

Audio: `LF/Base/MfgTest/MfgTest_ReleaseNotes.txt` quotes the device's own log,
the same source `LEAPPAD3.md` used —

```
/LF/Base/M2KMfgTest # [0x1] Found codec: socaudiolfp100, legacy=0
```

`socaudiolfp100`, the **same codec as the LeapPad2**. So `DEV_CODEC` needs no
guessing, and the existing ALSA shim faces the same `libasound` (four build-
stamp bytes apart) driving the same part.

Incidentally that line settles a `VERIFY` in `PLAN.txt:367` — the GS is
**M2K**, and its MfgTest logs identify as "Emerald App", the Leapster family
name.

### Kernel device names

`libUtility.so` probes both SoC generations (`lf1000-alvgpio` *and*
`lf2000-alive`, `lf1000-touchscreen` *and* `lf2000-touchscreen`), exactly as on
the LeapPad2. Names appearing in the GS's own scripts and libraries:

```
lf1000-gpio (16)  lf1000-nand (18)  lf1000-dpc (4)  lf1000-power (4)
lf1000-touchscreen (6)  lf1000-aclmtr (2)  lf1000-nor (4)  lf1000-clock (2)
lf2000-alive (16)  lf2000-nand (6)  lf2000-power (3)  lf2000-touchscreen (4)
lf2000-aclmtr (1)
```

`setcal` uses `lf2000-touchscreen`. `libLightningBase` reads
`/sys/devices/platform/lf1000-gpio/power`. That is the same split the LeapPad2
profile already encodes, so the `DEV_*_DEV` block copies across unchanged.

---

## 3. What a `leapstergs.conf` needs

**A draft, not a finished profile.** Written from the survey above; the
`UNVERIFIED` marks are the honest ones.

```sh
DEV_ID=leapstergs
DEV_NAME="Leapster GS"
DEV_LONGNAME="LeapFrog Leapster GS Explorer"

# Firmware-Base/meta.inf and the rootfs's own Firmware/meta.inf both say this.
DEV_META_DEVICE=LeapsterGSExplorer

# LOAD-BEARING, see §2: libUtility's GetPlatformName reads these and the
# Lightning shell picks LUCY/main.swf + LucyAssets from the answer.
DEV_PLATFORM=LUCY
DEV_PLATFORM_FAMILY=LEX          # rcS's LEX branch selects Lucy-Boot-logo.png
DEV_SYSTEM_REV=0x310             # UNVERIFIED — copied from the LeapPad2
DEV_LCD=320x240                  # every boot asset; no CW variant exists
DEV_LCD_TYPE=UNKNOWN             # UNVERIFIED — nothing in the image names it
DEV_LCD_MFG="K&D-1"              # UNVERIFIED — copied from the LeapPad2

DEV_GPIO_DEV=lf1000-gpio
DEV_DPC_DEV=lf1000-dpc
DEV_POWER_DEV=lf2000-power
DEV_ACLMTR_DEV=lf2000-aclmtr
DEV_TOUCH_DEV=lf2000-touchscreen

DEV_SHELL=/LF/Base/bin/AppManager   # byte-identical to the LeapPad2's
DEV_SHELL_ARGS=""
DEV_FIRST_APP=                      # AppManager takes none, as on the LeapPad2
DEV_ASSETS=LucyAssets
DEV_SPLASH=/var/screens/Lucy-Boot-logo.png

# rcS's PLATFORM==LUCY branch, transcribed. All four targets exist in the
# image — checked — which the shipped /var/sounds symlinks do NOT: they point
# at LPTx_SplashAnimation.ogg, shutdown.ogg, TransitionAnimation.ogg and
# LF/Base/L3B_Video/splash.wav, none of which are in the rootfs. rcS repoints
# them at boot and we never run rcS, so this table is the whole fix.
DEV_SOUNDS="StartupVideo.ogg    LF/Base/LucyAssets/Video/StartupVideo.ogg
ShutdownVideo.ogg   LF/Base/LucyAssets/Video/ShutdownVideo.ogg
TransitionVideo.ogg LF/Base/LucyAssets/Video/TransitionVideo.ogg
powerdown.wav       LF/Base/LucyAssets/Video/powerdown.wav"

# NOT a guess. Derived the same way the LeapPad2's was: the PackageID in the
# shell directory's own manifest.
#   LP2  LF/Base/LPAD/meta.inf  PackageID="PAD2-0x1F1E0002-100000"  = DEV_UIPKG
#   GS   LF/Base/LUCY/meta.inf  PackageID="GAM2-0x00210008-100000"
DEV_UIPKG=GAM2-0x00210008-100000

DEV_CODEC=socaudiolfp100            # MfgTest release notes, quoted above

DEV_FW_DIR=GAMFW
DEV_FW_PKG=GAM2-0x00210004-000000   # Micron; -001000 Samsung, -002000 Hynix
DEV_PKGLIST=EnglishLeapsterGSExplorer.xml

# UNVERIFIED. Only CMfgData::Init reads this, and only for MfgData0, so the
# three-line LeapPad3-style table is probably enough. The GS is 2 GB NAND
# against the LeapPad2's 4 GB, so the LeapPad2's Bulk and RFS sizes are wrong
# here even though nothing reads them.
DEV_MTD="mtd0: 0007e000 00001000 \"NOR_Boot\"
mtd1: 00001000 00001000 \"MfgData0\"
mtd2: 00001000 00001000 \"MfgData1\""

DEV_HAS_WIFI=0     # measured: no wireless driver, no wpa_supplicant, no ConnMan
DEV_HAS_QT=0       # measured: no LF/Base/Qt, no libQtGui anywhere

DEV_ENV=""         # /etc/profile is byte-identical to the LeapPad2's, which
                   # sets nothing the guest needs — there is nothing to carry

DEV_UI_ROTATE=0    # landscape-native; no CW asset exists
```

`DEV_ASSETS` is worth a note: it is documented in `runtime/devices/README.md`
and mentioned in a `setup-sysroot.sh` comment, but **nothing reads it** —
`grep -rn DEV_ASSETS` finds only the comment. The sounds loop it describes uses
`DEV_SOUNDS`. Filling it in is harmless; relying on it is not.

The package list would need copying to `tools/packagelists/`, as the LeapPad3's
was.

---

## 4. What would break

The LeapPad3's three blockers were a D-Bus service that fails activation on
purpose, a GL command-ring limit, and unmixed audio streams. **None of those
three can occur here** — no D-Bus package manager (only Avahi), the same GL
binary Tadpole is already tuned for, and the same single-stream Brio audio
path.

What is left is smaller and more clerical. In descending order of confidence:

### 4a. MEASURED — the software rasteriser is 480x272 at compile time

`tadpole/shim/tadpole_gles_core.c`:

```c
#define FB_W 480
#define FB_H 272
…
static u32   g_back[FB_W * FB_H];      /* line 289 */
static float g_zbuf[FB_W * FB_H];      /* line 371 */
…
g_fb = mmap(NULL, FB_W * FB_H * 4 * 3, …);
dst  = … + (u32)y * FB_W;              /* every row write */
```

The *framebuffer emulation* is already per-device — `tadpole_shim.c` 655-656
reads `TADPOLE_W`/`TADPOLE_H`, which `tadpole.sh:514` sets from `DEV_LCD`. It
is only the software GL rasteriser that is frozen. At 320x240 the guest's
stride is 320 and every row would be written 480 pixels apart: a diagonal
shear, and reads past the end of the visible buffer. These are static arrays,
so it is not a one-line `#define` change — the buffers have to become
runtime-sized or over-allocated to a maximum.

The viewer's HLE path (`tadpole_hle.c`) has **no** hardcoded 480/272 and takes
its geometry from the shared guest state, so the host-GL path is probably
already fine. The 480/272 constants in `hle_selftest.c`, `hle_probe.c` and
`tadpole_view.c:915` are test harnesses, not the runtime path.

There is an irony worth noting: 320x240 is *exactly* the Leapster title
viewport the same file's header comment describes — Leapster games already
render 320x240 inside the LeapPad2's 480x272 panel. The GS is that viewport
made the whole screen.

### 4b. MEASURED — three hardcoded `LpadAssets` paths in the installers

```
tools/install-firmware.py:543   LF/Base/LpadAssets/Video   -> the /var/sounds links
tools/install-firmware.py:704   LF/Base/LpadAssets_en/Data/ProgramFileAppOrder.json
tools/install-content.sh:199    the same sort file
```

The GS has `LucyAssets` and `LucyAssets_en` and **no `LpadAssets` at all** —
not even a symlink; `find -name "*Assets*"` returns exactly those two
directories.

Consequences, both of which present as something other than their cause:

* line 543 skips silently, so `/var/sounds/StartupVideo.ogg` and friends stay
  dangling. `setup-sysroot.sh`'s own comment records what that does: VideoDaemon
  exits immediately instead of serving its socket.
* line 704 hits the `if not os.path.isfile(sort)` branch, prints "(no sort file;
  skipping profile access)", and no installed title gets a `ProfileAccess` line
  — so every title installs correctly and then does not appear on the home
  screen. That is the exact failure `grant_profile_access` was written to fix
  for the LeapPad2.

`runtime/setup-sysroot.sh` is fine on sounds (it uses `DEV_SOUNDS`) but has
LeapPad2 package IDs baked in at lines 758-759:

```sh
mkdir -p LF/Bulk/ProgramFiles/{KeyboardWidget,CameraWidget,PhotoEditor,SneakPeekWidget}
mkdir -p LF/Bulk/Downloads/PAD2-0x00210008-200000 LF/Bulk/Downloads/PADS-0x1F1E0002-300000
```

The GS's equivalents are `GAM2-0x00210008-200000` (Lucy Compatibility Data —
in its manifest, 911 bytes on the CDN) and `GAM2-0x00210008-300000` (Patch
List). Note `0x00210008` is the same product ID on both devices — only the
four-letter prefix changes — so this is a prefix substitution, not a lookup.

The widget directory names are LeapPad2's too; the GS's widget set from its
manifest is Camera, Video, Gallery, Microphone and Photo Editor.

### 4c. MEASURED — the D-pad rotation constant is a LeapPad2 fact

`tadpole_view.c` derives the D-pad mapping from the display rotation plus
`#define DPAD_GAME_TURN 3`. The comment is explicit that this is measured for
the LeapPad2: a portrait device playing landscape Leapster titles, so the
game's axes are a quarter turn from the D-pad's *regardless* of display
rotation, and the constant was arrived at by two rounds of measurement with a
human at the keyboard.

The GS is a **landscape handheld with a physically aligned D-pad**. That
premise does not hold, and the correct constant is very likely 0. There is a
`TADPOLE_DPAD_SHIFT` override, but no profile field — so this is a per-device
value with nowhere to live yet.

### 4d. MEASURED, but benign — the boot table entry is right

`tadpole_boot.c:64` says the LeapsterGS row is "a starting point … not a claim
that it works". Both paths it names are correct:

```
/var/screens/Lucy-Boot-logo.png                 exists, 320x240
/LF/Base/LucyAssets/Video/StartupVideo.ogg      exists, 112,519 bytes
```

and the selection loop takes the first row where either file exists, so a GS
sysroot cannot match the Valencia row (it has neither `Valencia-Boot-logoCW.png`
nor `LpadAssets`). The comment can be upgraded from speculation to fact —
though it still has not been *played*, only located.

### 4e. SPECULATION — where I would expect trouble

Labelled as guesses. None of this was tested.

**The 320x240 panel through the viewer's letterboxing.** The LeapPad2 path is
well travelled at 480x272 and the Ultra at 1024x600, but a *smaller* panel is
new, and the recorded `SDL_RenderReadPixels` output-coordinate trap lives in
exactly that code. Expect at least one scaling bug.

**Brio's multi-buffer framebuffer arithmetic.** `tadpole_shim.c`'s `NBUF 8`
comment derives its virtual-display height from an observed allocation —
`0x7F800 is exactly one 480x272x4 screen`. At 320x240x4 one screen is 0x4B000.
The code advertises `g_h * NBUF` and should scale, but the reasoning behind the
constant was validated at one size only.

**`libImageIO.so` is the one Brio library that differs**, by 2 KB. It is the
image decoder the Flash shell uses for its PNG assets. Probably a 320x240
build difference; possibly a decoder change that the shim's `open`/`mmap`
interception meets differently. Cheap to check, worth checking early.

**The three NAND variants may not be interchangeable.** I extracted the Micron
image only. The manifest says they differ in erase/write geometry, and the
volume name encodes it (`C2G-E1M-W4K`), which strongly implies the *contents*
are the same rootfs — but I did not download the Samsung or Hynix images and
diff them, so that is an inference from a filename.

**The Surgeon / recovery path is untouched.** 6.2 MB, present, never opened.

**The cartridge slot.** The GS takes Leapster Explorer cartridges, and
`libCartridgeMPI.so` is byte-identical to the LeapPad2's — but the LeapPad2's
own cartridge handling is a known soft spot (`CartManager` is commented out in
the shared `/usr/bin/app`), so "identical" here means "identically unfinished".

---

## 5. What I did not do

Stated plainly, because a reader should not have to infer it:

* **Nothing was booted.** No sysroot was built, no `AppManager` was run, no
  screenshot exists.
* **No profile was written.** §3 is a draft in a document, not a file in
  `runtime/devices/`.
* **No code was changed.** The breakages in §4 are located, not fixed.
* **Only the Micron firmware was extracted** — one of three.
* **`GetPlatformName()` was not disassembled**, so the precedence among the
  five paths it references is unknown.
* **`DEV_SYSTEM_REV`, `DEV_LCD_TYPE`, `DEV_LCD_MFG` and `DEV_MTD` have no
  evidence behind them** and are LeapPad2 values carried over. The LeapPad2's
  came off live hardware; these did not.

---

## 6. Verdict

**Feasible, and cheaper than any device added so far.**

The reason is not that the Leapster GS is simple. It is that Tadpole's hardest
won knowledge — the Brio MPI surface, the `libGLESv1_CM` shim, the
`AppManager` launch path, the ALSA codec shim, the `/usr/bin/app` semantics —
was all won against binaries the GS ships unchanged. The Ultra and the
LeapPad3 each required learning a second shell. This requires learning a second
*screen size*.

The work, roughly:

1. `runtime/devices/leapstergs.conf` — §3, mostly already written.
2. `tools/packagelists/EnglishLeapsterGSExplorer.xml` — a copy.
3. The `LpadAssets` hardcodes in two installers and the two LeapPad2 package
   IDs in `setup-sysroot.sh` — mechanical, four sites (§4b).
4. `FB_W`/`FB_H` in the shim's software rasteriser — the only real engineering
   (§4a), and possibly avoidable if the HLE path carries the load.
5. `DPAD_GAME_TURN` needs to become per-device (§4c).
6. Then boot it and find out what §4e got wrong.

**The single biggest unknown** is item 4: whether the software GL rasteriser
can be made geometry-agnostic cleanly, or whether 480x272 is baked into more of
its arithmetic than the two static arrays and the row stride. That file was
written against one panel size and has never been asked about another. If the
HLE path turns out to carry every title, it may not matter at all — but that is
precisely what has not been measured, and the recorded lesson that
`compat-sweep` runs `--no-viewer` and therefore only ever judged the software
path means the project's existing evidence cannot answer it either.

---

## Appendix — every command, in order

```sh
# 1. the manifests
unzip -o ~/Downloads/XMLs.zip -d xmls

# 2. does the firmware exist
curl -sSI https://digitalcontent.leapfrog.com/packages/GAMFW/GAM2-0x00210004-000000.lfp

# 3. the whole list (scratch script in §1; 236 HEADs, 0.15s apart)
python3 probe_gs.py xmls/EnglishLeapsterGSExplorer.xml

# 4. fetch and open
curl -sS -o GAM2-0x00210004-000000.lfp \
  https://digitalcontent.leapfrog.com/packages/GAMFW/GAM2-0x00210004-000000.lfp
unzip -l GAM2-0x00210004-000000.lfp
unzip -o -q GAM2-0x00210004-000000.lfp -d fwx
cat fwx/Firmware-Base/meta.inf
file fwx/Firmware-Base/4,2270472,kernel.bin

python3 -c "import sys; sys.path.insert(0,'<repo>/tools'); import pkgtool; \
  pkgtool.cmd_ubi('fwx/Firmware-Base/5,53477376,C2G-E1M-W4K-erootfs.ubi','gsrfs')"

# 5. the comparison (GS = gsrfs/*/ubi_rfs, LP2 = rootfs/stock-4.6.0.784/ubi_rfs)
diff $LP2/usr/bin/app        $GS/usr/bin/app          # identical
diff $LP2/etc/profile        $GS/etc/profile          # identical
diff $LP2/etc/init.d/rcS     $GS/etc/init.d/rcS       # three trivial hunks
diff $LP2/usr/bin/make_dev_nodes.sh $GS/usr/bin/make_dev_nodes.sh
diff $LP2/etc/init.d/mounts  $GS/etc/init.d/mounts

for d in lib usr/lib LF/Base/lib LF/Base/Brio/lib LF/Base/Flash/lib; do
  for f in $(ls $GS/$d); do
    md5sum $GS/$d/$f $LP2/$d/$f 2>/dev/null; done; done   # then compare pairwise

cmp -l $GS/usr/lib/libdbus-1.so.3.5.7 $LP2/usr/lib/libdbus-1.so.3.5.7   # 4 bytes

# 6. the device's own answers
cat  $GS/LF/Base/LUCY/meta.inf              # -> DEV_UIPKG
grep -n socaudio $GS/LF/Base/MfgTest/MfgTest_ReleaseNotes.txt   # -> DEV_CODEC
for f in $GS/var/screens/*.png; do file -b $f; done            # -> DEV_LCD, no CW
ls -la $GS/var/sounds/                      # -> the dangling links DEV_SOUNDS fixes
strings $GS/LF/Base/Brio/lib/libUtility.so | grep -E "sys/devices|Platform"
strings $GS/LF/Base/lib/libLightningBase.so | grep -E "main.swf|Assets_"
ls $GS/usr/share/dbus-1/system-services/    # Avahi only
find $GS -iname "*wifi*" -o -iname "*wireless*" -o -iname "*wpa*"   # nothing
ls -d $GS/LF/Base/Qt                        # does not exist

# 7. a content package end to end
curl -sS -o GAMS-0x00210001-000000.lf2 \
  https://digitalcontent.leapfrog.com/packages/GAMFW/GAMS-0x00210001-000000.lf2
# Type="Application", AppSo="MyStuffApp.so"; NEEDED lists the usual Brio +
# Lightning set and libdl.so.0 — the shim's injection vector, unchanged.
```
