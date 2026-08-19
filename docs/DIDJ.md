# The Didj

Whether Tadpole can run a Didj natively, and how far it gets today. Written
against the real firmware, pulled from LeapFrog's CDN and extracted.

**Short answer: further than expected.** Online System Update installs a Didj
the same way it installs any other device, and the Didj's own `AppManager` then
starts under `qemu-arm`, brings up Brio, and dies on ONE assertion — a cartridge
read. That is a class of problem Tadpole already solves for other devices.

    ./tools/online-update.sh            # with the Didj chosen in the wizard
    TADPOLE_DEVICE=didj ./tools/online-update.sh

Downloads thirteen packages, extracts the JFFS2 root filesystem to
`rootfs/stock-1.35.2.4222/jffs2_rfs`, and assembles `runtime/sysroot` with the
`/Didj` tree the packages fill. Re-running it is safe and rebuilds in place.

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

Extraction was a throwaway `jefferson` venv while this was an investigation.
`tools/pkgtool.py` now reads JFFS2 itself, with nothing to install:

    ./tools/pkgtool.py jffs2 erootfs.jffs2 /tmp/didjroot

**Written rather than depended on**, which is the opposite of the call made for
UBI. `ubi_reader` is a substantial piece of software that already exists and is
shipped as-is; JFFS2 here is two structs and one compressor. Measured before
writing any of it: the image is 4637 nodes, and every compressed inode in it is
`JFFS2_COMPR_ZLIB` — no LZO, no rtime, no rubin — so `zlib`, a memcpy and a run
of zeros cover it completely. Anything else is refused loudly rather than
filled with plausible bytes.

**Checked against `jefferson`, not against itself.** Same image, both readers:
178 files byte-identical by MD5, the same 325 symlinks with the same targets,
the same 62 directories, no diff at all. And every node's checksums verify —
all 4637 header CRCs, all 673 dirent CRCs, all 3911 inode node and data CRCs —
which is a stronger statement about the struct layout than any extraction that
merely looked plausible, since a CRC only agrees if every preceding field is
where the reader thinks it is. `tools/tests/jffs2_read_test.py` covers the
awkward cases against fixtures it builds itself, so it runs with no firmware
present.

One trap worth writing down: JFFS2's `crc32` is **not** zlib's. It calls
mtd's table loop with an initial value of 0 and applies neither the initial nor
the final inversion of the standard CRC-32, so `zlib.crc32(buf, 0xFFFFFFFF) ^
0xFFFFFFFF` is what agrees with it. Comparing against plain `zlib.crc32` says
every node in a perfectly good image is corrupt.

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

The manual recipe this section used to give — copy the rootfs, unzip nine
packages into `Didj/Base`, one into `Didj/Data/Avatars`, one into
`Didj/ProgramFiles` — is what `install-firmware.py` now does. The destination
map lives in `DIDJ_DESTS` there and in the comments beside each entry in
`tools/packagelists/Didj.xml`; the two say the same thing, and the XML says it
because the CDN cannot.

After an install:

    cd runtime/sysroot && qemu-arm -L "$PWD" \
      -E LD_LIBRARY_PATH=/Didj/Base/Brio/lib:/Didj/Base/lib:\
../shimlibs:../libs:/lib:/usr/lib \
      -E TADPOLE_DIR=/tmp/tadpole-didj -E TADPOLE_SYSROOT="$PWD" \
      -E TADPOLE_W=320 -E TADPOLE_H=240 -E TADPOLE_EVDEV=lf1000 \
      ./Didj/Base/bin/AppManager

(or `./tadpole.sh`, which passes all of that and warns that it will not draw.)
The shim directories on `LD_LIBRARY_PATH` are what supply the input devices and
`/dev/dsp`; without them Brio asserts on the first thing it touches.

    (no assert — it runs, and does not draw)

**Both the cartridge and the input asserts are gone.**

Brio finds its keyboard by opening `/dev/input/event0..` in turn and asking
`EVIOCGNAME`, and it wants three devices, not one. All three name/phys pairs
are read out of the device's own kernel — `kernel.bin` is a container with a
gzip'd Linux 2.6.20.1 inside, and each driver's strings sit adjacent in its
rodata:

| name | phys |
|---|---|
| `LF1000 Keyboard` | `lf1000/input0` |
| `Power Button` | `lf1000/power_button` |
| `LF1000 USB` | `lf1000/usb` |

**Serving only the first was worse than serving none.** Brio's
`ButtonPowerUSBTask` polls three descriptors; having filled only one it polled
two uninitialised ones, which held `1`, so it asked about stdout, was told
`POLLIN` every time, read it, got `EBADF`, and went round again — three hundred
thousand times in twelve seconds, with no error anywhere. A missing device does
not announce itself; it corrupts the poll set of whatever wanted it.

Audio is portaudio over OSS, which no other device here uses, so the shim
answers `/dev/dsp` directly: the format ioctls, and writes forwarded to
`$TADPOLE_DIR/audio.<pid>.dsp` where the viewer already looks. The guest
negotiates **32000 Hz, stereo, S16_LE, 2048-byte fragments** and produces
16 KB buffers on time.

 Brio
names every `/sys` path this device uses in `libUtility.so` and
`libDisplay.so`, and there are three; the installer writes them, with each
value read out of the firmware rather than guessed:

| file | value | where the value comes from |
|---|---|---|
| `lf1000-nand/cartridge` | `none` | `usr/bin/cartinfo` lists the whole set in its help text — production, development, manufacturing, base, none — and defaults to none. The slot is empty. |
| `lf1000-usbgadget/vbus` | `0` | `usr/bin/lftest_usb` asserts `vbus = 0` for "cable is unplugged". |
| `lf1000-power/status` | `1` | `etc/init.d/lightning` treats 3 and 4 as low battery, anything else as normal; 1 is EXTERNAL in the LeapPad2's own hardware capture. |

Without the first of those, `AppManager` does not start at all. **Both backends
now reach the same next assert**, which is a good sign in itself: qemu-arm and
glasspole disagree about nothing here.

**Do not trust an error that names a host path.** qemu's `-L` only redirects
paths that ALREADY EXIST in the sysroot, so any library missing from the Didj
tree silently resolves to the DEVELOPER'S copy and reports something that reads
as broken firmware and is nothing of the sort:

    '/usr/lib/libstdc++.so.6' is not an ELF executable for ARM
    '/lib/libz.so' is not an ELF executable for ARM

The first is an `LD_LIBRARY_PATH` ordering mistake — `libstdc++` is in `/lib`
on this device. The second is structural and the sysroot builder fixes it: the
Didj keeps `libz`, `libpng` and `liblzo2` in `/usr/lib` while something in the
Brio stack asks for `/lib/libz.so` by absolute path, so **both** directories are
built holding the union of the two. A faithful copy of the tree produces five
of those messages; the assembled sysroot produces none.

One line of host bleed-through is left, and it is cosmetic:

    cache '/etc/ld.so.cache' is corrupt

The Didj has no `/etc/ld.so.cache` — on hardware the loader's open fails and it
moves on — but under `-L` a missing file falls through to the host's, which is
glibc's. Writing a uClibc-format empty cache silences it; that is fabricating a
loader file to quiet a warning, so it is not done.

## What is left

1. **The display, and it is the only thing left between here and a picture.**
   `libDisplay.so` opens `/dev/mlc`, `/dev/layer0..2`, `/dev/dpc`, `/dev/ga3d`
   and `/dev/mem` — the LF1000's multi-layer controller. The shim fakes
   `/dev/fb0..2` for every other device, an ordinary fbdev with the
   `LF1000FB_*` extensions; none of that is reusable as-is. Until it exists the
   Didj runs and never paints.
2. **`/Didj` versus `/LF`** everywhere the emulator assumes the latter.
   `install-firmware.py` knows the difference; `tadpole.sh`, `run.sh` and the
   viewer do not yet, which is why there is no "play" for this device.
3. **`Pa_StartStream` never returns.** Audio itself works — see below — but
   Brio's main thread is left in a futex wait after portaudio creates its
   callback thread, so nothing after audio init runs. The callback thread is
   healthy and producing 16 KB buffers on time, which is the odd part. The
   likely answer is the one `tadpole_asound.c` reached for ALSA: replace
   `libportaudio.so` outright rather than emulate a device well enough for a
   2008 copy of it. Brio imports only nine `Pa_*` symbols, so the surface is
   small.
4. **`pipe` is missing from glasspole** (ARM syscall 42), so any guest shell
   script with a pipeline or a `$(…)` fails there with "pipe call failed" —
   and the Didj boots through `usr/bin/launch_main`, which is a shell script.
   It needs `gp_pipe` in `host.h` and an implementation in BOTH backends;
   adding it to `host_posix.c` alone is the failure mode that file exists to
   prevent. qemu-arm has no such gap.
5. **`runtime/setup-sysroot.sh` cannot build this tree**, and says so rather
   than building nonsense over it — both write to `runtime/sysroot`, and the
   LeapPad layout it assembles would overwrite a working Didj. It still
   *switches* to the Didj; rebuilding means re-running the installer.
6. **Fields not yet read** out of the image, deliberately absent from
   `runtime/devices/didj.conf` rather than guessed: `DEV_UIPKG`, `DEV_SPLASH`,
   `DEV_SOUNDS`, `DEV_CODEC`, and the `DEV_*_DEV` node names. Nothing is
   invented under the sysroot's `/sys` or `/flags` either, for the same reason:
   the LeapPad's values were read off real hardware and nobody has read a
   Didj's.

What is NOT a worry: the panel is 320x240 (all eight boot screens agree), there
is no wifi, no Qt, and no touchscreen — so none of the three blockers that cost
the LeapPad3 port apply here.
