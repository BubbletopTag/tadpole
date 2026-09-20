# LeapPad Ultimate — porting notes

Working notes for the `leappad-emu-3` branch, in the spirit of `LEAPPAD3.md`
and `LEAPTV.md`: what was tried, what it did, and the measurement behind it.

Firmware under test: **9.1.0.252**, built 2021-09-29, `Device="LeapPadUltimate"`,
codename **SANTIAGO**. NXP4330 (LF3000), 1024x600, capacitive touch. The last
firmware LeapFrog built for any LeapPad, five years after the LeapPad3's.

**It boots.** Sign-in, Guest, the two-page home screen with the bundled apps,
touch, audio. Measured on this machine in one evening; no title has been
launched yet.

---

## Where the firmware comes from

The CDN, under the ordinary content layout, exactly as the LeapPad3's is —
no per-device firmware directory:

```
packages/0x00430001/PHR3-0x00430001-000000.lfp   156,894,035  Firmware Base
packages/0x00430003/PHR3-0x00430003-000000.lfp     7,110,410  Surgeon
packages/0x00430001/PHR3-0x00430001-000002.lfp         4,598  Bulk Empty
```

The IDs are from `tools/packagelists/EnglishLeapPadUltimate.xml`, LFConnect's
own list for the device (164 packages, `mandatoryVersion="9.1.0.85"`). 162 of
them are downloadable, 1.7 GB; 23 arrive as `.lf3` and open with the usual key
— the bundled apps (Photo Fun Ultra, Pet Chat, Pet Pad Party, the six demos,
Letter Factory Adventures, the five utilities' parents). The whole thing is one
Online System Update:

```sh
./tadpole.sh --device leappadultimate   # or pick it in the wizard, then
./tools/online-update.sh                # download, extract, install, register
./tadpole.sh --device leappadultimate --boot
```

The base package is the LeapPad3's shape: `firmware/emmc/2ext4/3/RFS`, a plain
GNU tar of the root (312 MB), and `firmware/emmc/1fat/2/FAT32`, a tar holding
the uImage. Neither installer could read that shape — the LeapPad3 and the
LeapTV were both laid out by hand — so both now can: `install-firmware.sh`
and `install-firmware.py` recognise an `RFS` under `emmc/` as the root, unpack
it to `emmc_rfs` (device nodes skipped, modes kept), and keep the uImage under
`fat/`. The Python side asks tarfile for `filter="fully_trusted"` where it
exists, because 3.14's default `data` filter refuses the absolute symlinks the
root is full of.

`shots/` — `ultimate-signin.png`, `ultimate-home.png`.

## The LeapPad3 at the Ultra's size

Everything the LeapPad3 port established carries over unchanged, and this is
measured rather than assumed:

| | LeapPad3 (6.2.0.654) | Ultimate (9.1.0.252) |
|---|---|---|
| kernel | 3.4.39, nxp4330-cabo | 3.4.39, nxp4330-santiago |
| loader | ld-uClibc 0.9.33.1-git | byte-identical |
| libc | uClibc 0.9.33.1-git | same version, different build |
| shell | AppServer / Qt 4.8.4 QWS, `-qws` | same, different build |
| GL | Mali, GLES 1 in every Qt module | Mali, a different build of the same 1,479,456-byte blob; still GLES 1 only |
| Brio | | identical but for libUtility and libWirelessMPI |
| `/etc/profile` | | identical but for one PATH entry |
| `/usr/bin/app` | `fbctrl set pan 0 544` | `fbctrl set pan 0 1200` |
| panel | 480x272 | **1024x600** |

So the profile is the LeapPad3's with the Ultra's panel, and the two geometry
bugs the Ultra hit at 1024x600 did not reappear: MainPicker reports
`isPortrait: false w: 1024 h: 600` and draws edge to edge.

Where the facts in `leappadultimate.conf` come from:

* **SANTIAGO** — the uImage's own name (`Angstrom/3.4.39+git/santiago`), the
  kernel's "SANTIAGO GPIO mapping" banner, `/etc/init.d/touchscreen-santiago`,
  `/var/screens/santiago_boot_battery_low_1024_600.png`, and a branch in
  `usr/bin/recovery-dftp` beside RIO, CABO, XANADU, BOGOTA and GLASGOW.
* **1024x600** — every full-screen asset in `/var/screens`, two of them by
  name, the kernel's mode string, and rcS's battery-low card.
* **No first app** — `/usr/bin/app` reads `$RIO_FIRST_APP`, nothing sets it,
  and `BaseAppPaths.json` is the same "empty one will work for Cabo, Xanadu,
  Bogota" placeholder. `AppServer -qws` alone brings up SignIn.
* **The UI package** — `LF/Base/Qt/meta.inf` says `PHR3-0x00270008-100000`.
* **Touch** — `touchscreen-santiago` probes i2c bus 2 for a Chipone (K&D) or
  an Ilitek (GP) controller and never reads `lcd_mfg`, so that sysfs value is
  decoration here.

## Two things between the tar and the home screen

**1. The QWS display lock.** Under Glasspole the shell died one second in:

```
QLock::QLock: Cannot create semaphore ... FATAL: Cannot get display lock
```

AppServer's display lock is a SysV semaphore, and Glasspole has no `semget`
(ARM 299) or `semctl` (300). The LeapTV hit the same wall; the profile names
qemu as its engine the same way. `TADPOLE_QEMU` still overrides.

**2. The home screen asks the databases, not the disk.** As on the Ultra and
the LeapPad3: `tools/register-packages.py` after the content pass, or the
picker shows the four packages the firmware knew about and nothing else.
`Packages::GetInstalledPackages: Returning 24 packages` once it has run.

## What is not done

* **No title has run.** BrioWrapper has not been launched here; the LeapPad3's
  wireless-MPI crash and the LeapTV's are the things to expect first.
* **Tile labels.** Several tiles carry their package *type* ("Demo", "Game",
  "Video") where the device shows the title. Not investigated; likely the
  DisplayName the register tool writes.
* **The camera.** Two of them on this device, like the LeapPad3; `Camera` in
  the dock is untested. The LeapTV's `tadpole_v4l2.c` is the piece to reuse.
* **Wireless.** The fake ConnMan reports online, package-manager's web-service
  calls fail with "Host not found", and nothing shims LeapFrog's services.
* **UNVERIFIED** values in the profile are exactly the LeapPad3's:
  `platform_family`, `system_rev`, `lcd_type`, the codec.
