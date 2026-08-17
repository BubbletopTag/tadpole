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

## It runs in the Tadpole window, not just headless

`./tadpole.sh --boot`. Two more bugs had to go first, and both were only ever
going to show up on a device that is not 480x272.

### 9. The viewer sized itself before it knew the panel

`w`/`h` are read once, before the frame loop, from a `state.bin` that on a
cold start does not exist yet — so they fell back to 480x272 and the textures
were built that size. When the guest mapped and turned out to be 1024x600,
every scanline was streamed into a 480-wide texture: colours right, each line
further left than the one above, which reads as violent horizontal tearing
rather than as a size mismatch. It also overran `pixels`, allocated for the
smaller panel.

Invisible on a LeapPad2, where the fallback happens to be the right answer —
which is why it survived until there was a second device.

The viewer now rebuilds textures, buffers, logical size and window size on the
first frame where `g_state` disagrees, and says so:

```
tadpole-view: 480x272, scale 2x, dir /tmp/tadpole
tadpole-view: panel is 1024x600
```

*The first attempt at this sat beside the `try_map()` retry, inside a branch
that only runs while UNMAPPED, so it never fired on the frame that mattered.
It is at the top of the frame loop now, where there is no branch to get wrong.*

### 10. `guest()` preferred the developer's binaries

`tadpole.sh`'s `guest()` kept an absolute path as given whenever it existed and
only fell back to the rootfs when it did not — which silently runs the HOST's
binary for every name the two systems share. Starting the guest's D-Bus daemon
produced:

```
qemu-arm: /usr/bin/dbus-daemon: Invalid ELF image for this architecture
```

An absolute path in this project means a path inside the guest. It looks there
first now.

## D-Bus is not optional

`rcS` runs `dbus-1 start` and `dbus-session start`, and the Ultra's shell is
built on it — `libQtDBusE` is in every module's `NEEDED`, and `RioPkgManager`
is a D-Bus proxy. `tadpole.sh` now starts a session bus for a Qt device and
passes `DBUS_SESSION_BUS_ADDRESS` to the shell.

**VideoDaemon is not started for a Qt device.** On this firmware it links none
of the three names we impersonate, so the shim only reaches it transitively,
and from there its own `dlsym(RTLD_NEXT, "open")` comes back round to itself.
The shim's guard catches that and stops rather than recursing away the stack.
AppServer copes — it logs `DaemonControl socket connect failed` and carries on.

## Packages: registered, but the picker still will not show them

**The Ultra does not scan `ProgramFiles`.** Traced under strace, a booted
MainPicker opens exactly two files to find out what is installed —

```
/LF/Bulk/SharedPackageInfo.db      what is installed, and where
/LF/Bulk/LocalPackageInfo.db       icon, state, dates + ProfileAccess
```

— and never touches `ProgramFiles` at all. So a perfectly installed package
that is not in the databases does not exist as far as the home screen is
concerned. On hardware they arrive already populated on the Bulk partition
(they are in `erootfs.md5`) and `lfpkg` maintains them; we install by
untarring, so nothing does.

**The schema is recovered, not guessed.** It is in `usr/lib/liblfp.so.1.0.0`
as literal strings — the two `CREATE TABLE Packages` (they differ between the
two databases), `CREATE TABLE ProfileAccess`, and every INSERT and SELECT.
Finding it meant asking which library links `libQtSqlE` at all, because
`strings | grep -i "create table"` on the obvious candidates returns nothing.

`tools/register-packages.py` writes all 26 installed packages into both
databases. **This is necessary and not yet sufficient**: with the rows present
the picker still comes up empty, so something in them is being filtered.

Known so far:

* `PreviewIcon` must be RELATIVE to `InstallDir`. An absolute path there makes
  the reader build `InstallDir + "/LF/Bulk/..."`, and MainPicker segfaults on
  the resulting null image — which is what it did first time.
* `Hidden=0`, `DeviceHidden=0`, `State=1` (and `State=0`, tried separately), `DeviceAccess` from `meta.inf`, and
  `ProfileAccess` rows for every slot in `meta.inf`'s `ProfileAccess=` (the
  Ultra's apps say `-1,0,1,2,3`) are all set, and are not enough.

### The missing service: `/usr/bin/package-manager`

MainPicker does not read the databases itself. It asks
**`com.leapfrog.PackageManager`** over D-Bus, through `libQtRioPkgManager`,
which is only a PROXY. Exactly two files in the image contain that name: the
proxy, and `/usr/bin/package-manager`. Nothing started the daemon, so the query
had nothing to reach — the picker came up correct, themed and empty however
carefully the databases were filled in.

`tadpole.sh` starts it now for a Qt device.

**It needed a fifth shim variant to work at all.** package-manager links none
of `libdl`/`libz`/`libEGL`, so it ran unshimmed — and an unshimmed guest cannot
CREATE files, because qemu-user's `-L` only redirects paths that already exist.
SQLite creates a journal beside its database, so every write failed:

```
Failed to vacuum local database, most likely corrupt:
QSqlError(14, "Unable to fetch row", "unable to open database file")
```

`runtime/shimlibs-pkg/libWebServices.so.1` is the vector, chosen because
package-manager links it and **neither AppServer nor MainPicker does** — so it
can share a `LD_LIBRARY_PATH` with `shimlibs-egl` without ever putting two
shims in one process. Unlike libEGL, the stock libWebServices HAS a SONAME, so
the copy's had to be patched too (`libWebServices.so.1` -> `libWebServices.so.9`,
same length): a copy still calling itself by the original name made our library
depend on itself, and package-manager died on
`can't resolve symbol '_ZN8LeapFrog11WebServices15DownloadManagerC1Ev'`.

With that in place the daemon registers on the bus and **builds both databases
itself**, which settled a question the strings could not:

```
SharedPackageInfo.db   Packages, ProfileAccess
LocalPackageInfo.db    Packages, CartTracker
```

`ProfileAccess` is in the SHARED database. `register-packages.py` had it in
Local — a very reasonable reading of two adjacent `CREATE TABLE` strings, and
wrong. Rows in the wrong file are not an error; they are simply never read,
which looks exactly like filtering.

### Icons: ask the device to do it

`shots/ultra-home-icons.png`. Fourteen tiles with their real artwork — Pet Pad
Party, Pet Chat, Art Studio, Photo Fun, Bookshelf, App Center, LeapSearch,
Calculator, Notepad, Clock, Calendar, Music, Voice Memo, Welcome.

**The daemon does not scan at startup, but it will if asked.** Introspecting
the service over D-Bus lists a method called `RebuildPackageDatabase`. Calling
it walks `ProgramFiles`, parses every `meta.inf` with the code that wrote them,
and fills both databases correctly — 26 hand-written rows became **126** real
ones, including all the widget and download packages hand-registration had
skipped.

```sh
dbus-send --session --print-reply --dest=com.leapfrog.PackageManager / \
          com.leapfrog.PackageManager.RebuildPackageDatabase
```

`tadpole.sh` now does this automatically, **only when `Packages` is empty** — a
rebuild discards per-package local state (install dates, the NEW! flags, last
played), so it is not a thing to do on every boot.

That also retires the interesting part of `tools/register-packages.py`. Hand
registration got the schema right and the contents subtly wrong; the device's
own code has neither problem. The tool is kept for `--list`, and for the record
of what the schema is and how it was recovered.

**The lesson worth keeping**: two days of this were spent reconstructing what
the guest already knew how to do. The question that cracked it was not "what
does this table need" but "what can I ask the device to do for me" — and the
answer was one D-Bus introspection away the whole time.

### Where it had stood

Before the rebuild call above, everything looked right and nothing showed:
daemon running and shimmed, 26 hand-written rows in both databases, 130
`ProfileAccess` rows, schema written by the device's own code,
`CurrProfile=0x00000000` matching slot 0, no crashes — and an empty picker.
Two boots were spent on `State=1` and `State=0`; neither mattered.

The rows were not wrong so much as incomplete: the device's own rebuild
produces 126, not 26. Hand registration covered `ProgramFiles` directories
only, and the picker wants rather more than that.

Dead ends recorded so they are not retried:

1. **`State`** is `LFP::PackageState`, a C++ enum — so its values are NOT in
   the strings and `1` is a guess. Disassembling `Package::SetState` or
   finding a real `LocalPackageInfo.db` would settle it.
2. **The DeviceAsset may need its own row.** The apps are marked `hidden` in
   `EnglishLeapPadUltra.xml` and their DA is not; on the LeapPad2 the DA
   carries the home-screen icon. `install-content.sh` merges the DA INTO the
   app directory, so there is currently no `-DA0000` row at all.
3. `Redownload` is compared against the STRING `'true'`
   (`SELECT PackageID FROM Packages WHERE Redownload = 'true'`) while the
   column is NUMERIC — worth matching the device's spelling exactly.

## Still to do

* **icons** — see above; the packages are registered and the picker still
  filters them out
* **touch: most of the way, not there.** See "Touch" below.
* **the firmware installer cannot be driven from the GUI** — reported against
  the wizard; not yet investigated
* **`make-profile.sh` does not write the RIO profile shape** (see 8 above)
* **VideoDaemon has no injection vector** — it links neither libdl, libz nor
  libEGL on this firmware, so nothing intercepts it yet
* ~~audio: `libasound.so.2` lives in `shimlibs`, which is off the path for a Qt
  device now, so the Ultra has no audio shim~~ — **fixed on `leappad-emu-3`.**
  `make shimegl` now copies it into `shimlibs-egl` beside `libGLESv1_CM.so`, on
  the same argument that makes that copy safe: `tadpole_asound.c` is built
  alone and defines no `open`/`ioctl`/`mmap`, so it cannot become the second
  interceptor. Note the Qt devices' `libAudio.so` also imports two symbols the
  LeapPad2's does not (`snd_pcm_hw_params_get_{buffer,period}_time`) and the
  shim had to grow them, or Brio's `dlopen` fails outright. Measured on a
  LeapPad3; the Ultra shares the path and the shell but has not been booted
  since, so treat it as untested there.
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


---

## Touch — the plumbing works, Qt still does not act on it

Not finished. What is established, so none of it needs redoing:

**`LinuxInput` is not an option, despite appearances.** `libQtGuiE`'s strings
list `linuxinput`, `intellimouse`, `mouseman` and `qvfbmouse`, which reads like
four built-in handlers. With `QWS_MOUSE_PROTO=LinuxInput:/dev/input/event2`
correctly in the environment — verified in the running qemu's argv, not
assumed — Qt opened **no** event node at all. The only mouse driver plugin in
the image is `libqtslibmousedriver.so`, so that list is a name table for
handlers this build does not carry. It has to be tslib.

**tslib was never broken; it was pointed at a node that did not exist.**
`/etc/profile` sets `TSLIB_TSDEVICE=/dev/input/touchscreen0`, which udev makes
on hardware and nothing made here. `module_raw input` opened nothing, the chain
came up with a null `ops->read`, and the first touch jumped through it — the
crash that made `tadpole.sh` disable tslib by default in the first place. The
shim now maps that name to event2 (`ev_index`), and with the device's own four
`TSLIB_*` variables in `DEV_ENV`, tslib reads `/etc/ts.conf` and loads all five
modules — `input`, `pthres`, `variance`, `dejitter`, `linear`.

**The events reach the guest.** With `--debug`:

```
[tadpole] ev2 GUEST-GOT KEY code=330 val=1
[tadpole] ev2 GUEST-GOT ABS code=0 val=690
[tadpole] ev2 GUEST-GOT ABS code=1 val=290
[tadpole] ev2 GUEST-GOT ABS code=24 val=60
[tadpole] ev2 GUEST-GOT SYN code=0 val=0
```

Correct coordinates, correct pressure, read by exactly one process — so this is
not two readers stealing from one FIFO, which was the obvious suspicion.

**One sample is not a touch.** `tools/tap.py` sent a single position and held
it. Real hardware reports continuously while a finger is down, and this
device's `ts.conf` chain is made of filters with memory: `variance delta=30`
and `dejitter delta=100` both need a run of samples before they emit anything.
A single sample is swallowed inside tslib with no error. `tap.py` now streams
at 50 Hz for the duration of the hold. **The viewer almost certainly needs the
same treatment** — it sends on motion, so a press-and-hold without movement is
one sample, exactly the case that vanishes.

**Where it stops:** tslib has the samples and the picker does not move. Nothing
in the log complains. The next step is to see what `ts_read` actually returns —
there is no `ts_test` or `ts_print` in this image, so that means either
building one against the guest's `libts` or logging inside the shim's read path
for fd 8. After that, Qt's `QWSTslibMouseHandler` applies its own
`QWSPointerCalibrationData`, which is the next place a correct sample can be
turned into the wrong screen position — or into none.

Also unstarted: **installing packages from the Tadpole GUI for the Ultra**. The
Game Library and the wizard's install path are still LeapPad2-shaped, and the
Ultra needs the extra step this branch found — after any install, the package
manager has to be told, or the new title will not appear on the home screen.
