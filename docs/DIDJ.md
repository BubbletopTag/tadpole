# The Didj

Whether Tadpole can run a Didj natively, and how far it gets today. Written
against the real firmware, pulled from LeapFrog's CDN and extracted.

**Short answer: further than expected.** The Didj's own `AppManager` starts
under `qemu-arm`, brings up Brio, and dies on ONE assertion — a cartridge read.
That is a class of problem Tadpole already solves for other devices.

## Not to be confused with "Didj support"

Two different things share the word, and conflating them has already caused one
wrong comment in the wizard:

- **The Didj** — the 2008 LF1000 handheld. Its own firmware, run as itself.
  That is what this document is about.
- **"Didj support"** — a compatibility layer **built by a member of the LFHacks
  community, not by LeapFrog**, which lets Didj GAMES run on LeapPad2 firmware.
  That is the wizard step, and it is an add-on to the LeapPad2.

## Getting the firmware

    ./tools/fetch-firmware.py --device didj --probe
    13 available, 0 not on the CDN, 56,943,042 bytes total

    ./tools/fetch-firmware.py --device didj --get all -o /tmp/didjfw

All thirteen packages are live. No code changes were needed to reach them:
Didj packages sit flat under `packages/DIDJ/` as `.lfp` rather than under
`packages/<middle>/` as `.lf2`, and `fetch-firmware.py` already tries `lfp`
when a device directory is set.

`tools/packagelists/Didj.xml` is **not** a LeapFrog artefact — LeapFrog never
published an LFConnect XML for the Didj as it did for the LeapPad and Leapster
lines. It is transcribed from a community-supplied inventory, and the install
destinations live in comments beside each entry because that mapping cannot be
derived from the CDN.

## The rootfs is JFFS2, and that is the first real difference

Every LeapPad firmware ships a tarball. `DIDJ-0x000E0003-000001.lfp` is a ZIP
containing:

    firmware-LF_LF1000/meta.inf
    firmware-LF_LF1000/erootfs.jffs2     7,077,888
    firmware-LF_LF1000/kernel.bin        1,310,720

`meta.inf` says `Device="Didj"`, `Version="1.35.2.4222"`, 22 April 2009 — which
is what `tad_detect_device()` matches on.

Nothing in this repo reads JFFS2 and the host had no mtd-utils, so extraction
used `jefferson`:

    python3 -m venv /tmp/jffsvenv && /tmp/jffsvenv/bin/pip install jefferson
    /tmp/jffsvenv/bin/jefferson -d /tmp/didjroot firmware-LF_LF1000/erootfs.jffs2

**Installed into a throwaway venv on purpose.** Adding a project dependency is
a decision for `tools/fetch-deps.sh`, not a side effect of an investigation.
Turning this into a supported install path is outstanding work.

## It is a Brio device

The boot chain is ordinary: `rcS` sources `/etc/profile`, runs `/etc/rc.d/S*`,
and `/etc/init.d/lightning` starts `/usr/bin/launch_main` — a shell script whose
`DEFAULT_MAIN_APP` is **AppManager**, the same shell the LeapPad2 and Leapster
GS run. The Brio package carries the same MPI set (`libModuleMPI`,
`libVideoMPI`, `libAudioMPI`, `libEventMPI`, `libPowerMPI`, `libUSBDeviceMPI`),
and `AppManager` also links `libopengles_lite.so`, so there is a GLES stack
here too.

Binaries are ARM EABI4 against uClibc 0.9.29, so `qemu-arm` executes them.

## The tree root is /Didj, not /LF

This is the structural difference that will cost the work. Every other device
Tadpole runs lays its system out under `/LF/Base`; the Didj uses `/Didj/Base`,
with cartridges at `/Cart/Base`. Its entire `/etc/profile` is two lines:

    PATH=/Cart/Base/Brio/bin:/Didj/Base/Brio/bin/:/Didj/Base/bin/:$PATH:/usr/local/bin
    LD_LIBRARY_PATH=/Didj/Base/Brio/lib/:/Didj/Base/lib/:$LD_LIBRARY_PATH:/usr/local/lib

Anything in the emulator that hardcodes `/LF` will have to learn this.

## Reproducing how far it gets

Assemble a sysroot — the JFFS2 rootfs, then each package at its destination
from the map in `tools/packagelists/Didj.xml`:

    R=/tmp/didjsys; cp -a /tmp/didjroot $R
    for p in 0x000E0004-000001 0x000E0005-000003 0x000E0006-000005 \
             0x000E0007-000006 0x000E0008-000007 0x000E0009-000008 \
             0x000E000A-000001 0x000E000B-000004 0x000E000C-000002; do
        unzip -qo /tmp/didjfw/DIDJ-$p.lfp -d $R/Didj/Base/
    done
    unzip -qo /tmp/didjfw/DIDJ-0x000E0010-000002.lfp -d $R/Didj/Data/Avatars/
    unzip -qo /tmp/didjfw/DIDJ-0x000F0001-000000.lfp -d $R/Didj/ProgramFiles/

Then:

    cd $R && qemu-arm -L . \
      -E LD_LIBRARY_PATH=/Didj/Base/Brio/lib:/Didj/Base/lib:/lib:/usr/lib \
      ./Didj/Base/bin/AppManager

    !ASSERT: [3] CButtonModule::LightningButtonTask: cart read failed
    terminate called without an active exception

**Order that `LD_LIBRARY_PATH` carefully, and do not trust an error that names
a host path.** qemu's `-L` only redirects paths that ALREADY EXIST in the
sysroot, so any library missing from the Didj tree silently resolves to the
DEVELOPER'S copy. Listing `/usr/lib` before `/lib` produced

    '/usr/lib/libstdc++.so.6' is not an ELF executable for ARM

which reads as a broken firmware and is nothing of the sort — `libstdc++` is in
`/lib` on this device, and the message is the host's x86 copy being found.

## What is left

1. **The cartridge.** `CButtonModule::LightningButtonTask` reads a cart and
   asserts when it cannot. Tadpole already fakes a cartridge for the LeapPad2
   (see `cartridge.sh` and the `cnotify` states in `tadpole.sh`), so this is
   the nearest thing to a solved problem on the list.
2. **A supported JFFS2 install path**, so `install-firmware` can lay a Didj
   image into a sysroot the way it does a tarball.
3. **`/Didj` versus `/LF`** everywhere the emulator assumes the latter.
4. **Fields not yet read** out of the image, deliberately absent from
   `runtime/devices/didj.conf` rather than guessed: `DEV_UIPKG`, `DEV_SPLASH`,
   `DEV_SOUNDS`, `DEV_CODEC`, and the `DEV_*_DEV` node names.

What is NOT a worry: the panel is 320x240 (all eight boot screens agree), there
is no wifi, no Qt, and no touchscreen — so none of the three blockers that cost
the LeapPad3 port apply here.
