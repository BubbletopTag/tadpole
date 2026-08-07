# Device profiles

One `.conf` per tablet Tadpole knows how to be. Adding a device is this file
and nothing in C: both readers glob this directory.

| | |
|---|---|
| `runtime/device.sh` | the shell side — resolves which device, sources its profile |
| `tadpole/viewer/tadpole_ui.c` | the wizard's "Which tablet?" page |
| `tools/fetch-firmware.py` | `--device <id>`, for the CDN directory and package list |

## Which device is normally not a choice

It is a property of the firmware that is installed. Every LeapFrog rootfs
carries `Firmware/meta.inf` with a `Device="..."` line, and `device.sh` matches
that against each profile's `DEV_META_DEVICE`:

```
Device="LeapPad2Explorer"  ->  leappad2.conf
Device="LeapPadUltra"      ->  leappadultra.conf
```

The wizard's picker and `ui.cfg`'s `device` line are **overrides**, and no
`device` line is written unless one is chosen. That default is deliberate: a
stale setting would boot a LeapPad2 rootfs at the Ultra's 1024x600 and give you
a working emulator showing a corrupt screen, which is a miserable thing to
debug. The picker exists because the choice has to be made *before* there is
any firmware to detect — it decides which firmware gets downloaded.

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
