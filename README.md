# Tadpole

A LeapPad2 emulator for Linux.

Tadpole runs the stock LeapFrog system software — AppManager, the Brio
framework, Flash Lite, and native Leapster titles — on a desktop, by emulating
the NXP3200 "VALENCIA" hardware they expect to find underneath.

**Tadpole contains no LeapFrog code.** You supply the system files and the
games — the ones from your own device. See
[About the software you run on it](#about-the-software-you-run-on-it).

---

## What works

| | |
|---|---|
| System menu, sign-in, home screen | yes |
| Flash titles | yes |
| Native Brio titles (2D) | yes |
| Audio | yes, paced to real time |
| Touch and buttons | yes |
| Host-GPU rendering (HLE) | yes — about 5x the software renderer |
| 3D (race tracks) | yes — Clam Prix races render and play |
| Skinned player character | yes — the rider draws with the kart |
| FMV / video layer | yes — Sneak Peeks trailers and the system videos play |
| Boot logo and startup animation | yes — off by default, see Fast Boot |

Frame rate on an AMD FirePro W4100: ~57 fps with GPU replay, against 11.5 fps
software. Both are capped at the panel's real 60 Hz.

## How many titles actually run

Every installed title, launched in turn and screenshotted:

```sh
./tools/compat-sweep.sh          # launch each one, capture it, record a verdict
./tools/compat-report.py         # -> build/compat/<date>/index.html
COMPAT_EMU="$(command -v qemu-arm)" ./tools/compat-sweep.sh     # the other engine
./tools/compat-compare.py --a <qemu-run> --b <glasspole-run>    # both, side by side
```

The most recent run, 110 titles, both engines swept sequentially on the same
firmware the same hour:

| | qemu-arm | Glasspole |
|---|---|---|
| Launches to a real screen | 82 | 85 |
| Crashes before drawing | 16 | 17 |
| Runs, draws nothing | 12 | 8 |
| — of which Flash | 14/15 | 14/15 |
| — of which native Brio | 68/95 | 71/95 |

**The two engines are effectively one-to-one on this catalogue.** Glasspole is
the from-scratch ARM JIT (see `glasspole/`); qemu-arm is the reference. That
result is why Glasspole is now the engine Tadpole runs by default, on Linux as
well as Windows: when the numbers are the same, the engine worth putting in
front of people is the one whose bugs are ours to fix. qemu-arm stays as the
fallback when no Glasspole is built, and as the reference the sweep diffs
against — `TADPOLE_QEMU="$(command -v qemu-arm)" ./tadpole.sh` for a single run
on it. Three
titles either way is inside the run-to-run spread — the guest's own dynamic
loader has an unlocked `dlopen`, so a title that loads two libraries at once
can fault on one run and reach its menu on the next, and each engine dodges
that race on different runs. Treat the two columns as the same number.

**The failures are not one bug each.** They cluster: a handful of distinct
fault sites account for nearly all of them, and the report puts that above the
table for exactly this reason. Chasing the clusters is what took Glasspole from
56 to 85 in a day — one of those clusters was a single `chdir` that made every
relative open resolve under the sysroot twice, and it alone was wearing three
disguises across 21 titles.

**And every one of those launches had no player signed in.** The sweep starts
titles directly, with no home screen, because that is what makes a hundred
launches practical; a title that wants profile data may well behave differently
when it has some. A crash here means "crashed this way" until it is checked
both ways.

The report is a single self-contained page — every thumbnail embedded, so it
can be sent on its own. It is written under `build/`, which is not in the
repository, so run the sweep to get one.

---

## Quick start

```sh
chmod +x Tadpole-x86_64.AppImage
./Tadpole-x86_64.AppImage
```

One file, no install step, no dependencies to hunt down — the ARM engine and
the firmware toolchain ride along inside it. The setup wizard opens on the
first run and asks for the two things Tadpole cannot ship: your device's system
files, and your cartridge backups.

To build that file yourself:

```sh
./tools/fetch-deps.sh          # stage qemu and the firmware tools (~70 MB)
./tools/online-update.sh       # system files, straight from LeapFrog
cd tadpole && make && cd ..    # the shim and the viewer
(cd glasspole && ./fetch-deps.sh && cmake -S . -B build -GNinja && ninja -C build)
./tools/build-appimage.sh      # -> build/Tadpole-x86_64.AppImage, ~22 MB
```

**The Glasspole line is not optional if you want the image people download.**
It is the engine Tadpole runs on by default, and `build-appimage.sh` can only
bundle one that exists — skip it and the image you built falls back to the
bundled qemu-arm, which is a different program from the one being released.

**The firmware step is not optional, and it comes before `make`.** The ARM shim
links against the LeapPad's own uClibc, which is LeapFrog's and cannot be
shipped here — so a fresh clone has no `runtime/libs/libc.so.0` and `make` stops
with an explanation of how to get one. `make viewer` on its own needs none of
it, if all you want is the front end.

`tools/test-build.sh` runs exactly this sequence in a clean Ubuntu, Arch and
Fedora container, from `git archive HEAD` rather than your working tree — which
is the only way to find out whether the instructions above are true.

---

## Requirements

**If you have the AppImage, there are none.** It carries Glasspole — the ARM
engine it runs on by default — a static `qemu-arm` behind it, and a private
Python with `ubi_reader`, so there is nothing to install and nothing to look up
for your distribution. Download it, `chmod +x`, run it.

Everything below is for building from source.

Run this first — it checks everything at once, says which pieces Tadpole already
carries, and prints the exact command for anything left:

```sh
./tools/check-deps.sh
```

### Let Tadpole fetch its own dependencies

```sh
./tools/fetch-deps.sh
```

This downloads a static `qemu-arm` and a relocatable Python with `ubi_reader`
into `build/deps/`, installing **nothing** system-wide. Everything afterwards —
`./tadpole.sh`, the firmware installer, the AppImage build — finds them there.
It is the shortest path from a clone to a running emulator.

Roughly 70 MB, pinned by SHA-256, re-fetchable at any time
(`./tools/fetch-deps.sh --clean`). What it fetched is recorded in
`build/deps/manifest.txt`.

### Or install them yourself

**To run:**

| | Arch | Debian / Ubuntu | Fedora | Why |
|---|---|---|---|---|
| an ARM engine | `qemu-user` | `qemu-user` | `qemu-user` | runs the guest's 32-bit ARM code — *or bundled, and a built `glasspole/` satisfies it too* |
| SDL2 | `sdl2-compat` | `libsdl2-dev` | `SDL2-devel` | window, input, audio |
| OpenGL | `mesa` | `libgl1-mesa-dev` | `mesa-libGL-devel` | host-GPU rendering (HLE) |
| zlib | `zlib` | `zlib1g-dev` | `zlib-ng-compat-devel` | the viewer decodes its own icon |

**To build:**

| | Arch | Debian / Ubuntu | Fedora | Why |
|---|---|---|---|---|
| clang | `clang` | `clang` | `clang` | cross-compiles the guest shim to ARM |
| lld | `lld` | `lld` | `lld` | the ARM linker |
| make | `make` | `make` | `make` | |
| python3 | `python` | `python3` | `python3` | build and analysis tooling |
| pkg-config | `base-devel` | `pkg-config` | `pkgconf-pkg-config` | finds SDL2's flags |
| curl | `curl` | `curl` | `curl` | `fetch-deps.sh` downloads with it |
| zstd, xz | `zstd xz` | `zstd xz-utils` | `zstd xz` | unpack the `.deb`s `fetch-deps.sh` pulls (Ubuntu uses zstd, Debian xz) |

**To install firmware** (once) — **all of this is bundled by `fetch-deps.sh`,**
which is why the table is here for reference rather than as a shopping list:

| | Arch | Debian / Ubuntu | Fedora | Why |
|---|---|---|---|---|
| ubi_reader | AUR / pip | pip | pip | reads the UBIFS root filesystem |
| lzallright | AUR / pip | pip | pip | ubi_reader's LZO backend |
| cryptography | `python-cryptography` | `python3-cryptography` | `python3-cryptography` | ubi_reader imports it unconditionally |
| zstandard | `python-zstandard` | `python3-zstandard` | `python3-zstandard` | ubi_reader imports it unconditionally |
| unzip, bzip2 | `unzip bzip2` | `unzip bzip2` | `unzip bzip2` | optional — `tools/pkgtool.py` reads both formats with Python's stdlib |

**ubi_reader is not packaged by any of the three.** There is no
`python3-ubi-reader` on Ubuntu and no `python-ubi-reader` in the Arch repos —
both are pip or AUR only, and `lzallright` is the same. That is precisely why
`fetch-deps.sh` exists: it stages a private Python with all four inside
`build/deps/`, installing nothing system-wide, and every tool here prefers that
one. Use it and this whole table is somebody else's problem.

**Its three dependencies are not optional.** ubi_reader imports them at module
scope, so a missing one fails the whole extraction — and reports only one at a
time. `check-deps.sh` asks the one question that matters (*can the Python that
would actually be used import `ubireader.ubifs.misc`?*), and
`install-firmware.sh` runs that check before doing any work.

### Arch

```sh
sudo pacman -S qemu-user sdl2-compat mesa zlib clang lld make python \
               base-devel curl unzip bzip2 zstd xz
sudo pacman -S libogg libtheora libvorbis      # optional: startup animation
```

### Debian / Ubuntu

```sh
sudo apt install qemu-user libsdl2-dev libgl1-mesa-dev zlib1g-dev \
                 clang lld make python3 pkg-config curl unzip bzip2 zstd xz-utils
sudo apt install libogg-dev libtheora-dev libvorbis-dev   # optional: startup animation
```

### Fedora

```sh
sudo dnf install qemu-user SDL2-devel mesa-libGL-devel zlib-ng-compat-devel \
                 clang lld make python3 pkgconf-pkg-config curl unzip bzip2 zstd xz
sudo dnf install libogg-devel libtheora-devel libvorbis-devel  # optional: startup animation
```

Then `./tools/fetch-deps.sh` for the rest. If you would rather not use it, add
your distribution's `python3-cryptography` and `python3-zstandard`, then
`pip install --user ubi_reader lzallright`.

### One note on OpenGL

Host-GPU rendering needs a **compatibility profile**: GLES 1.x has no shaders,
so Tadpole maps it onto desktop OpenGL's fixed-function pipeline. Mesa provides
this. Check with:

```sh
glxinfo -B | grep -i compatibility
```

---

## Getting the system files

**Do this before `make`.** Tadpole ships nothing from LeapFrog, and the ARM shim
has to link against the LeapPad's own uClibc — so there is no build without a
firmware first. (`make` says so, at length, if you try.)

### The easy way: no device, no LFConnect

```sh
./tools/online-update.sh
```

LFConnect fetches these packages from a public LeapFrog server, and so can this.
It downloads the firmware and the content packages, extracts the root
filesystem, and builds the sysroot — everything below happens automatically.
**Help → Online System Update** in the application does the same thing.

Two things it deliberately does not get: `Firmware-BulkEmpty` (15 MB of zeros —
`/LF/Bulk` is filled by the content packages, not the firmware), and `.lf3`
packages, which are encrypted and need a key Tadpole does not ship.

### Or from an LFConnect download cache

If you have run LFConnect on a PC, it leaves a `LFC_Downloads` folder with a
`cache/` directory full of hash-named `.lf2` and `.lfp` files:

```sh
./tools/install-firmware.sh /path/to/LFC_Downloads
```

It finds the `Firmware-Base` package, extracts the root filesystem, installs the
content packages, and builds the sysroot. Or use **Help → Setup Wizard** in the
application and press Browse.

---

## Build

```sh
cd tadpole && make
```

That builds the guest shim libraries (ARM) and the viewer (host). `make check`
reports any missing toolchain.

`make viewer` builds the front end alone and needs no firmware and no cross
toolchain — useful if you want the window and the setup wizard first, and will
fetch the system files through it.

To check the instructions on this page still work on a machine that is not
yours:

```sh
./tools/test-build.sh              # Ubuntu 24.04, Arch and Fedora, in podman
./tools/test-build.sh ubuntu       # just one
```

It builds from `git archive HEAD`, so nothing in your working tree — no
extracted rootfs, no `runtime/libs`, no months-old object files — can make a
broken build look fine.

The pieces, if you want to know what it is doing:

* `.lf2` is a **bzip2 tar**, `.lfp` is a **ZIP** — despite the extensions
* each holds a package directory with a `meta.inf` manifest
* `Type=DiskImage, Name=Firmware-Base` carries `kernel.bin` and a
  `*-erootfs.ubi` root filesystem
* that `.ubi` is an LZO-compressed UBIFS volume, hence `ubi_reader` and
  `lzallright` — installing ubi_reader alone fails with
  `ModuleNotFoundError: No module named 'lzallright'`

---

## Getting games

Games are `.tar` backups of your own cartridges, made with **LFManager**.

**File → Game Library**, point it at the folder holding them, and you get the
list you would expect: each title's own icon and name, read out of the backup,
with the ones you have already installed marked. Tick what you want and press
Install. The wizard's Games page opens the same window.

The first read of a folder takes a few seconds per gigabyte — the icon lives
inside a 20-120 MB archive — and is cached against each file's size and
modification time, so opening the library again is instant. The cache lives in
`~/.cache/tadpole/games`.

From the shell:

```sh
./tools/scan-games.sh /path/to/backups     # build the icon-and-name index
./tools/install-game.sh games/YourGame.tar # install one
./tools/install-game.sh --from-list list   # or a file of paths, one per line
```

Backups come in three shapes and the installer handles all of them: a flat
archive with `meta.inf` at the top, one wrapped in a directory, and one that
bundles a shared library package alongside the game. The scanner copes with the
same variety in icons — RGB and RGBA PNG, and the Flash-era titles whose
manifest names a `.swf` and ships the artwork beside it as `PopUpIcon.png`.

### Raw cartridge dumps (`.bin`)

The other way people back a cartridge up is on the device itself, which needs
no PC software at all:

```sh
dd if=/dev/mtdblock6 of=/LF/Bulk/cart.bin     # on the device, over telnet
```

then pull the file off by FTP. That is a complete, faithful backup — and it is
a raw FAT filesystem image, not a package, so Tadpole could not install one.

**File → Convert Cartridge Dump...** turns it into a `.tar` and writes it into
your games folder, where the Game Library picks it up like any other backup.

```sh
./tools/cart2tar.py cart.bin            # or several, and -o to choose where
./tools/fatread.py cart.bin             # just list what is on it
```

The FAT reading is hand-written (`tools/fatread.py`, no dependencies) because
every off-the-shelf way to do it is missing on one platform or the other:
`mount -o loop` needs root and does not exist on Windows, and `mtools` is not
installed by default on any of the three Linux distributions above. Long
filenames are read properly — LeapFrog's own files are not 8.3, and a reader
that ignores VFAT produces a package of mangled stubs that installs and then
fails to load.

Nothing is rearranged on the way through. A cartridge holds the title *and* a
`lib/` package of shared libraries, both with their own `meta.inf`, and the
installer already knows what to do with an archive containing several — so the
faithful copy is also the one that installs correctly.

---

## Running

```sh
./tadpole.sh
```

This opens the front end. **Nothing boots until you ask it to** — use
**File → Run System Menu**. On a fresh install with no firmware, the setup
wizard opens instead of a wall of errors.

```sh
./tadpole.sh --boot     # open it and start the system menu immediately
./tadpole.sh --list     # list installed titles
./tadpole.sh --app NAME # launch one title straight in, no home screen
./tadpole.sh --shell    # an ARM shell inside the guest
```

`--app` takes a name or a Package ID and matches on either, so
`--app 'Clam Prix'` is enough. It skips the menu, the sign-in and the tile —
useful when you are testing one title over and over, and the reason a sweep of
a hundred of them is practical at all. Titles that expect a signed-in player
may behave differently this way than they do from the home screen.

### Controls

| | |
|---|---|
| Arrow keys | D-pad |
| X / Z | A / B |
| Q / W | L / R |
| Home | Menu |
| Esc | Back |
| Mouse | stylus |
| Ctrl+R | rotate |
| Ctrl+Q | quit |

The D-pad rotates with the display. A Leapster title is landscape on a portrait
device, so its axes sit a quarter turn from the hardware's; Tadpole corrects for
that automatically. If the directions still feel wrong in your orientation, set
`TADPOLE_DPAD_SHIFT=0..3`.

### The window turns with the app

The home screen is portrait and a game is not — on the same 480x272 panel. The
LeapPad UI draws a quarter turn from how the device is held, which is why the
stock boot art is named `...logoCW.png`; titles draw landscape into the same
buffer. So Tadpole turns the window to match: **portrait for the system menu,
sign-in and home screen, landscape the moment a title starts, and back again
when you leave it.**

It follows the guest rather than guessing: the shim reports which screen is up
— the UI's own `/LF/Base/LPAD/main.swf`, or a package's entry point as named by
its `meta.inf` — and the viewer decides the rotation from that.

A few LeapPad titles draw portrait (My Books and Notepad are the two in this
library); those are listed in the viewer and turn the right way. For anything
else drawn the other way up, **Ctrl+R** still works, and your choice stands
until the guest moves to another screen. Untick **Options → Graphics → Turn
with the app** to go back to a fixed orientation.

### Settings

Everything in **Options** is saved to `~/.config/tadpole/ui.cfg` and applied to
the next boot. Graphics, Audio and Controller are what they sound like; the two
worth describing are:

**Debug level** (Options → Debug Settings) — one dial instead of a row of
switches:

| | |
|---|---|
| 0 — silent | the guest's output goes nowhere |
| 1 — normal | AppManager's serial log, exactly what the device prints (~430 lines to reach the home screen) |
| 2 — verbose | adds the shim's file and audio tracing, and every GL stub and error (~2400 lines) |
| 3 — trace | adds every guest syscall. Enormous and slow, and the only thing that answers "did it even try to open that file" |

With **Write a log file** on, the whole lot also goes to
`~/.local/state/tadpole/tadpole.log`, with the previous boot kept as
`tadpole.log.1`. That matters when Tadpole is launched from a desktop icon and
there is no terminal to print to.

**System Settings** holds "Boot the system menu at startup", the remembered
games folder, and:

**Fast Boot** — ticked, which is why you have never seen a LeapPad2 boot in
Tadpole. Turn it off and **File → Run System Menu** does what the device does
when you switch it on: the LeapFrog logo, then the "LeapPad2 Explorer"
animation with its chime, then the home screen. It costs no time — the
animation plays over a guest that is already booting and stops the moment
AppManager raises `/tmp/ui_ready`, exactly where VideoDaemon stops it on
hardware — so on a fast boot you see less of it than on a slow one.

It applies to **Run System Menu only**. Launching a title goes straight to the
title, on the device and here.

Both are your own files, read out of the firmware you installed:
`/var/screens/Valencia-Boot-logoCW.png` and
`/LF/Base/LpadAssets/Video/StartupVideo.ogg`. Tadpole ships neither. The
animation needs libogg, libtheora and libvorbis at build time; without them the
sequence is the logo alone (see Requirements).

Suggested by Kat/ushka in the LFHacks community.

### Useful environment variables

Anything set here wins over the saved settings.

| | |
|---|---|
| `TADPOLE_GL=0` | use the stock GPU stack (titles will assert — for debugging) |
| `TADPOLE_GL_SOFTWARE=1` | the deprecated software rasteriser — see below |
| `TADPOLE_HLE_STRICT=1` | stop the guest outright when replay dies, instead of showing the dialog |
| `TADPOLE_HZ=n` | frame cap (default 60; `0` uncaps) |
| `TADPOLE_DIR=path` | where the shared framebuffers and FIFOs live |
| `TADPOLE_DEBUG=1` | verbose shim logging (what debug level 2 sets) |
| `TADPOLE_STRACE=1` | every guest syscall (what debug level 3 sets) |
| `TADPOLE_QEMU=path` | a specific engine, instead of the default Glasspole — `"$(command -v qemu-arm)"` to run on qemu |
| `TADPOLE_DEPS=dir` | where the bundled engine and Python live (the AppImage sets this) |
| `TADPOLE_THEME=green` | the original green chrome, instead of blue |

### The software rasteriser is deprecated

Host-GPU replay is the only supported way to render. The software rasteriser is
still in the binary, and it is still the right tool for exactly one job —
telling whether a rendering fault is in the shared GL core or only in the replay
— but it is years behind, several times slower, and it cannot express things the
titles rely on: it samples one texture unit and it ignores the blend factors.
Two bugs found this month were invisible on it for that reason, having been
scored "ok" by every compatibility sweep, which runs headless and therefore in
software.

So it is no longer reachable by accident:

* **Options → Graphics → "Host GPU replay"** is ticked, greyed and always on.
  A saved setting that turned it off is ignored.
* **If replay dies mid-session**, the guest no longer drops quietly to software.
  It raises a dialog — *GPU render engine CRASHED. Please restart Tadpole.* —
  because the frames after that point would be wrong rather than merely slow.
* **`--no-viewer` refuses** unless you ask for software by name; there is no
  host GPU without a viewer.

To use it deliberately:

```sh
TADPOLE_GL_SOFTWARE=1 ./tadpole.sh --app "Ben 10"
TADPOLE_GL_SOFTWARE=1 ./tadpole.sh --boot --no-viewer      # headless
```

On Windows, set it in the shell before launching:

```bat
set TADPOLE_GL_SOFTWARE=1
"%LOCALAPPDATA%\Programs\Glasspole\Glasspole.exe"
```

or in PowerShell:

```powershell
$env:TADPOLE_GL_SOFTWARE = 1
& "$env:LOCALAPPDATA\Programs\Glasspole\Glasspole.exe"
```

The viewer passes its own environment down to the guest, so setting it before
launch is all that is needed on either platform.

---

## About the software you run on it

Tadpole is an emulator. It contains no LeapFrog code, no firmware, no game data
and no copyrighted assets — it reproduces the *hardware* a LeapPad2 provides, and
everything it runs at runtime comes from files you supply.

Those files are the ones already on your own device:

* **System files** come from a firmware download. LFConnect — LeapFrog's own PC
  software — leaves them in its download cache when it updates a device, so the
  cache on your machine is where `install-firmware.sh` expects to find them.
  Look for a `LFC_Downloads` folder with a `cache` directory inside it.
* **Games** come from your own cartridges, backed up with LFManager.

This documentation describes how to work with files you already have. It does
not point anywhere else for them, and please keep it that way — no direct links
to vendor servers or archives, in issues or pull requests.

Copyright in the system software and the games belongs to LeapFrog and its
partners, and is unaffected by anything here. What you do with your copies is
your responsibility and subject to the law where you live.

`games/` is in `.gitignore` so backups do not end up committed by accident.

## Licence

Tadpole is free software, released under the GNU General Public License.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

THE SOFTWARE IS PROVIDED "AS IS". See `LICENSE` for the full text.

## Project layout

```
tadpole.sh              start the front end
tadpole/shim/           guest-side libraries (ARM) — the emulation itself
tadpole/viewer/         the application: window, UI, audio, host-GPU replay
tools/                  install and diagnostic scripts
tools/test-build.sh     build from scratch in a clean container, 3 distributions
tools/compat-sweep.sh   launch every installed title and record what it did
rootfs/                 firmware you installed          (not distributed)
runtime/sysroot/        the guest's filesystem view
docs/HANDOVER.md        engineering notes — how it works and why
```

`docs/HANDOVER.md` is the real documentation: every non-obvious decision, every
bug that took more than a few minutes, and the measurements behind them.

---

## Troubleshooting

**"missing rootfs" / the wizard keeps appearing** — the firmware is not
installed. Run `./tools/install-firmware.sh` with your `LFC_Downloads` folder.

**`ModuleNotFoundError` during a firmware install** — ubi_reader is missing one
of its dependencies. Run `./tools/check-deps.sh`, which lists all of them at
once rather than one per attempt.

**A title asserts at startup with `vr5_platform_fbdev.cpp`** — GL is disabled.
It is on by default; check you have not set `TADPOLE_GL=0`.

**`HLE FELL BACK - software` in the status bar** — host-GPU replay gave up. The
log says why. It is not fatal; rendering continues in software.

**Sound is fast, garbled, or cutting out** — should not happen any more, but the
cap is adjustable in Options → Audio Settings.

**Everything is slow** — check the status bar says `HLE nn fps`. If it says
`idle` or has fallen back, you are on the software renderer, which is about 5x
slower.
