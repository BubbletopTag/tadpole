# Tadpole

A LeapPad2 emulator for Linux.

Tadpole runs the stock LeapFrog system software — AppManager, the Brio
framework, Flash Lite, and native Leapster titles — on a desktop, by emulating
the NXP3200 "VALENCIA" hardware they expect to find underneath.

**Tadpole contains no LeapFrog code.** You supply the firmware and the games,
from hardware you own. See [Legal](#legal).

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

**Needed to run:**

| Package | Why |
|---|---|
| `qemu-user` (`qemu-arm`) | runs the guest's 32-bit ARM code |
| `SDL2` | window, input, audio |
| `zlib` | the viewer decodes its own icon |
| OpenGL with a **compatibility profile** | for host-GPU rendering (HLE) |

**Needed to build:**

| Package | Why |
|---|---|
| `clang` + `lld` | cross-compiles the guest shim to ARM |
| `make`, a C compiler | the viewer |
| `python3` | build and analysis tooling |

**Needed once, to install firmware:**

| Package | Why |
|---|---|
| `ubi_reader` | the LeapPad root filesystem is a UBIFS volume |
| `unzip`, `bzip2`, `tar` | firmware packages are ZIP and bzip2 tar |

Arch:

```sh
sudo pacman -S qemu-user sdl2 clang lld python unzip
yay -S python-ubi-reader
```

Debian / Ubuntu:

```sh
sudo apt install qemu-user libsdl2-dev clang lld python3 unzip python3-ubi-reader
```

The compatibility profile matters: GLES 1.x has no shaders, so HLE maps it onto
desktop OpenGL's fixed-function pipeline. Mesa provides this. Check with
`glxinfo -B | grep -i "compatibility"`.

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
LFConnect — LeapFrog's own PC software — leaves in its cache when it updates a
device you own.

Look for a `LFC_Downloads` folder containing `cache/` full of hash-named
`.lf2` and `.lfp` files. Then:

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
* that `.ubi` is a UBIFS volume, hence `ubi_reader`

---

## Getting games

Games are `.tar` backups of cartridges you own, made with **LFManager**. Install
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

## Legal

**Dump your own hardware. Do not download firmware or games.**

Tadpole is a clean-room emulator: it contains no LeapFrog code, no firmware, no
game data, and no copyrighted assets. Everything it needs at runtime comes from
files you provide.

* **Firmware** — obtain from a LeapPad2 you own, via LeapFrog's own LFConnect
  software. Distributing it is copyright infringement.
* **Games** — back up cartridges you own with LFManager. A backup of a cartridge
  you own is one thing; sharing it is another.
* Do not commit either into this repository. `games/` is in `.gitignore` for
  exactly this reason.

Emulators are lawful. The software they run is still owned by somebody.

---

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

**A title asserts at startup with `vr5_platform_fbdev.cpp`** — GL is disabled.
It is on by default; check you have not set `TADPOLE_GL=0`.

**`HLE FELL BACK - software` in the status bar** — host-GPU replay gave up. The
log says why. It is not fatal; rendering continues in software.

**Sound is fast, garbled, or cutting out** — should not happen any more, but the
cap is adjustable in Options → Audio Settings.

**Everything is slow** — check the status bar says `HLE nn fps`. If it says
`idle` or has fallen back, you are on the software renderer, which is about 5x
slower.
