# Device profiles

One `.conf` per tablet Tadpole knows how to be. Adding a device is this file
and nothing in C: both readers glob this directory.

| | |
|---|---|
| `runtime/device.sh` | the shell side — resolves which device, sources its profile |
| `tadpole/viewer/tadpole_ui.c` | the wizard's "Which tablet?" page |
| `tools/fetch-firmware.py` | `--device <id>`, for the CDN directory and package list |

## Which device is never a guess

It is a property of the firmware that is installed. Every LeapFrog rootfs
carries `Firmware/meta.inf` with a `Device="..."` line, and `device.sh` matches
that against each profile's `DEV_META_DEVICE`:

```
Device="LeapPad2Explorer"  ->  leappad2.conf
Device="LeapPadUltra"      ->  leappadultra.conf
```

**Several can be installed at once**, so the saved `device` line in `ui.cfg`
now selects which of them is live rather than overriding what the firmware
says. The two can no longer disagree: `runtime/sysroot` holds exactly one
device's assembled tree, `runtime/sysroot/.tadpole-device` records whose, and
the profile is always loaded for that. Naming a device with no firmware
installed does nothing but print a line saying so.

That matters because the failure it replaces was invisible. A stale setting
used to boot a LeapPad2 rootfs at the Ultra's 1024x600 and give you a working
emulator showing a corrupt screen. Now the setting moves the whole tree —
rootfs, sysroot, libraries and profile together — or it moves nothing.

The picker still exists for the case it was written for: the choice has to be
made *before* there is any firmware to detect, because it decides which
firmware gets downloaded.

| | |
|---|---|
| `runtime/sysroot` | the live device's assembled tree |
| `runtime/libs` | ...and its flat directory of guest libraries |
| `runtime/installs/<id>/` | the same two, for a device that is installed but parked |
| `runtime/sysroot/.tadpole-device` | one line: the `DEV_ID` this tree was built for |

Switching is `tad_activate_device()` in `device.sh`: park the live pair, move
the wanted pair in. Renames rather than a symlink at `runtime/sysroot`, because
three things outside the shell hardcode that literal path — `tadpole_boot.c`
reads the boot logo out of it, `tadpole_view.c` builds glasspole's Windows
command line from it, and `tadpole/Makefile` links against
`../runtime/libs/libc.so.0` — and because MSYS symlinks are invisible to native
Windows code, which is the same fact that makes `setup-sysroot.sh`'s `lns()` a
copy there.

```
./runtime/setup-sysroot.sh          rebuild the live device
./runtime/setup-sysroot.sh didj     switch to another, building it if new
./tadpole.sh --devices              what is installed, and which is live
```

## Device pictures

The wizard draws a placeholder: a box in each device's own aspect ratio, which
already tells 480x272 apart from 1024x600. Real photographs go here as
`<DEV_ID>.png` — `leappad2.png`, `leappadultra.png` — sized for a 34x21 slot,
so something around 136x84 is plenty.

**The loader is not wired up yet.** Dropping the files in will not show them
until `tadpole_ui.c` learns to read them; its PNG decoder is currently written
around the single application logo. Until then the placeholder stands, which is
the honest thing for it to do.

## Fields

Values are shell assignments — this file is `.`-sourced. Anything derived from
the firmware rather than measured on hardware is marked `UNVERIFIED` in the
profile, and those comments are worth keeping accurate.

| field | what it is |
|---|---|
| `DEV_ID` | the filename stem; what `ui.cfg` and `--device` name |
| `DEV_META_DEVICE` | the `Device=` string autodetect matches |
| `DEV_PLATFORM`, `DEV_PLATFORM_FAMILY` | `/sys/devices/system/board/*`; `rcS` branches on both |
| `DEV_LCD` | `WxH`, the `lcd_size` format, and the shim's geometry |
| `DEV_*_DEV` | kernel platform device names, which differ by SoC generation |
| `DEV_SHELL`, `DEV_SHELL_ARGS`, `DEV_FIRST_APP` | what "boot the system menu" runs |
| `DEV_SOUNDS` | `/var/sounds`, transcribed from this device's branch of `rcS` |
| `DEV_ENV` | guest environment the device sets in `/etc/profile` |
| `DEV_FW_DIR`, `DEV_FW_PKG`, `DEV_PKGLIST` | where its firmware lives on the CDN |
| `DEV_MTD` | `/proc/mtd`, which `CMfgData::Init` parses |
| `DEV_HAS_QT`, `DEV_HAS_WIFI` | capability flags the scripts branch on |
