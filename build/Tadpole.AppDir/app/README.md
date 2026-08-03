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
| 3D (race tracks) | **not yet** — geometry renders black |
| FMV / video layer | **not yet** |

Frame rate on an AMD FirePro W4100: ~57 fps with GPU replay, against 11.5 fps
software. Both are capped at the panel's real 60 Hz.

---

## Requirements

Run this first — it checks everything at once and prints the exact command for
your distribution:

```sh
./tools/check-deps.sh
```

### The full list

**To run:**

| | Arch | Debian / Ubuntu | Why |
|---|---|---|---|
| qemu-arm | `qemu-user` | `qemu-user` | runs the guest's 32-bit ARM code |
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

**To install firmware** (once):

| | Arch | Debian / Ubuntu | Why |
|---|---|---|---|
| unzip | `unzip` | `unzip` | `.lfp` packages are ZIP |
| bzip2 | `bzip2` | `bzip2` | `.lf2` packages are bzip2 tar |
| ubi_reader | `python-ubi-reader` (AUR) | `python3-ubi-reader` | reads the UBIFS root filesystem |
| lzallright | `python-lzallright` (AUR) | pip | ubi_reader's LZO backend |
| cryptography | `python-cryptography` | `python3-cryptography` | ubi_reader imports it unconditionally |
| zstandard | `python-zstandard` | `python3-zstandard` | ubi_reader's zstd backend |

**ubi_reader's three dependencies are not optional.** It imports them lazily, so
a missing one fails minutes into an extraction rather than at startup — and only
one at a time. `check-deps.sh` tests all of them up front, and
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

Games are `.tar` backups of your own cartridges, made with **LFManager**. Install
them with:

```sh
./tools/install-game.sh games/YourGame.tar
```

or **File → Install Package** in the application.

Backups come in three shapes and the installer handles all of them: a flat
archive with `meta.inf` at the top, one wrapped in a directory, and one that
bundles a shared library package alongside the game.

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
| Z / X | A / B |
| A / S | L / R |
| Home | Menu |
| Esc | Back |
| Mouse | stylus |
| Ctrl+R | rotate |
| Ctrl+Q | quit |

The D-pad rotates with the display. A Leapster title is landscape on a portrait
device, so its axes sit a quarter turn from the hardware's; Tadpole corrects for
that automatically. If the directions still feel wrong in your orientation, set
`TADPOLE_DPAD_SHIFT=0..3`.

### Useful environment variables

| | |
|---|---|
| `TADPOLE_GL=0` | use the stock GPU stack (titles will assert — for debugging) |
| `TADPOLE_GL_HLE=1` | host-GPU rendering |
| `TADPOLE_HLE_STRICT=1` | crash rather than silently fall back to software |
| `TADPOLE_HZ=n` | frame cap (default 60; `0` uncaps) |
| `TADPOLE_DIR=path` | where the shared framebuffers and FIFOs live |
| `TADPOLE_DEBUG=1` | verbose shim logging |

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
