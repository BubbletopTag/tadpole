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
| Skinned player character | **not yet** — the kart draws, its rider does not |
| FMV / video layer | **not yet** |

Frame rate on an AMD FirePro W4100: ~57 fps with GPU replay, against 11.5 fps
software. Both are capped at the panel's real 60 Hz.

---

## Quick start

```sh
chmod +x Tadpole-x86_64.AppImage
./Tadpole-x86_64.AppImage
```

One file, no install step, no dependencies to hunt down — a static `qemu-arm`
and the firmware toolchain ride along inside it. The setup wizard opens on the
first run and asks for the two things Tadpole cannot ship: your device's system
files, and your cartridge backups.

To build that file yourself:

```sh
./tools/fetch-deps.sh          # stage qemu and the firmware tools (~70 MB)
cd tadpole && make && cd ..    # the shim and the viewer
./tools/build-appimage.sh      # -> build/Tadpole-x86_64.AppImage, ~22 MB
```

---

## Requirements

**If you have the AppImage, there are none.** It carries a static `qemu-arm` and
a private Python with `ubi_reader`, so there is nothing to install and nothing
to look up for your distribution. Download it, `chmod +x`, run it.

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

| | Arch | Debian / Ubuntu | Why |
|---|---|---|---|
| qemu-arm | `qemu-user` | `qemu-user` | runs the guest's 32-bit ARM code — *or bundled* |
| SDL2 | `sdl2` | `libsdl2-dev` | window, input, audio |
| OpenGL | `mesa` | `libgl1-mesa-dev` | host-GPU rendering (HLE) |
| zlib | `zlib` | `zlib1g-dev` | the viewer decodes its own icon |

**To build:**

| | Arch | Debian / Ubuntu | Why |
|---|---|---|---|
| clang | `clang` | `clang` | cross-compiles the guest shim to ARM |
| lld | `lld` | `lld` | the ARM linker |
| make | `make` | `make` | |
| python3 | `python` | `python3` | build and analysis tooling |

**To install firmware** (once) — all of this is bundled by `fetch-deps.sh`:

| | Arch | Debian / Ubuntu | Why |
|---|---|---|---|
| ubi_reader | `python-ubi-reader` (AUR) | `python3-ubi-reader` | reads the UBIFS root filesystem |
| lzallright | `python-lzallright` (AUR) | pip | ubi_reader's LZO backend |
| cryptography | `python-cryptography` | `python3-cryptography` | ubi_reader imports it unconditionally |
| zstandard | `python-zstandard` | `python3-zstandard` | ubi_reader imports it unconditionally |
| unzip, bzip2 | `unzip bzip2` | `unzip bzip2` | optional — `tools/pkgtool.py` reads both formats with Python's stdlib |

**ubi_reader's three dependencies are not optional.** It imports them at module
scope, so a missing one fails the whole extraction — and reports only one at a
time. `check-deps.sh` asks the one question that matters (*can the Python that
would actually be used import `ubireader.ubifs.misc`?*), and
`install-firmware.sh` runs that check before doing any work.

### Arch

```sh
sudo pacman -S qemu-user sdl2 mesa zlib clang lld make python unzip bzip2 \
               python-cryptography python-zstandard
yay -S python-ubi-reader python-lzallright
```

### Debian / Ubuntu

```sh
sudo apt install qemu-user libsdl2-dev libgl1-mesa-dev zlib1g-dev \
                 clang lld make python3 unzip bzip2 \
                 python3-cryptography python3-zstandard
# ubi_reader and its LZO backend are usually not packaged:
pip install --user ubi_reader lzallright
```

### One note on OpenGL

Host-GPU rendering needs a **compatibility profile**: GLES 1.x has no shaders,
so Tadpole maps it onto desktop OpenGL's fixed-function pipeline. Mesa provides
this. Check with:

```sh
glxinfo -B | grep -i compatibility
```

---

## Build

```sh
cd tadpole && make
```

That builds the guest shim libraries (ARM) and the viewer (host). `make check`
reports any missing toolchain.

---

## Getting the system files

Tadpole ships nothing from LeapFrog. You need a firmware download, which
LFConnect — LeapFrog's own PC software — leaves in its download cache when it
updates a device.

On the machine where you have run LFConnect, look for a `LFC_Downloads` folder
with a `cache/` directory full of hash-named `.lf2` and `.lfp` files. Then:

```sh
./tools/install-firmware.sh /path/to/LFC_Downloads
```

It finds the `Firmware-Base` package, extracts the root filesystem, installs the
content packages, and builds the sysroot. Or use **Help → Setup Wizard** in the
application and press Browse.

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
./tadpole.sh --shell    # an ARM shell inside the guest
```

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

**System Settings** holds "Boot the system menu at startup" and the remembered
games folder.

### Useful environment variables

Anything set here wins over the saved settings.

| | |
|---|---|
| `TADPOLE_GL=0` | use the stock GPU stack (titles will assert — for debugging) |
| `TADPOLE_GL_HLE=1` | host-GPU rendering |
| `TADPOLE_HLE_STRICT=1` | crash rather than silently fall back to software |
| `TADPOLE_HZ=n` | frame cap (default 60; `0` uncaps) |
| `TADPOLE_DIR=path` | where the shared framebuffers and FIFOs live |
| `TADPOLE_DEBUG=1` | verbose shim logging (what debug level 2 sets) |
| `TADPOLE_STRACE=1` | every guest syscall (what debug level 3 sets) |
| `TADPOLE_QEMU=path` | a specific qemu-arm, instead of the bundled or installed one |
| `TADPOLE_DEPS=dir` | where the bundled qemu and Python live (the AppImage sets this) |

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
