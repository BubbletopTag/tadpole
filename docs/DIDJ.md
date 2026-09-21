# The Didj

Whether Tadpole can run a Didj natively, and how far it gets today. Written
against the real firmware, pulled from LeapFrog's CDN and extracted.

**It boots.** `./tadpole.sh` brings up AppManager and reaches its first-run
country picker — real UI, real artwork, drawn by the guest through this
project's GLES shim onto an emulated LF1000 multi-layer controller.

    ./tools/online-update.sh     # with the Didj chosen in the wizard
    ./tadpole.sh

**And it draws on the host GPU.** Host-GPU replay (HLE) was pinned off for
this device for months because it "froze on the copyright screen"; it never
did — see "Host-GPU replay" below for the three bugs that looked like one
freeze. The home screen, the country picker and a title all render under
replay at the panel's 60 fps now, and the profile no longer asks for the
software rasteriser.

What does NOT work yet: it runs on qemu-arm rather than glasspole (a default
the profile sets, with the reason recorded there), the boot movie's YUV plane
is not composited, and `libpng` hangs inside `imager`. See "What is left".

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

    (no assert — it runs; AppManager still does not reach its first frame)

**IT DRAWS.** Not AppManager yet, but the device's own display tool does, which
means the LF1000 multi-layer controller is emulated well enough to put real
pixels on a real layer:

    cd runtime/sysroot && qemu-arm -L "$PWD" \
      -E LD_LIBRARY_PATH=../shimlibs-z:../shimlibs:/Didj/Base/Brio/lib:/Didj/Base/lib:../libs:/lib:/usr/lib \
      -E TADPOLE_DIR=/tmp/tadpole-didj -E TADPOLE_SYSROOT="$PWD" \
      -E TADPOLE_W=320 -E TADPOLE_H=240 -E TADPOLE_EVDEV=lf1000 \
      ./usr/bin/imager /dev/layer0 /test/testimg.rgb

`/test/testimg.rgb` is LeapFrog's own 320x240 test image, dated 22 April 2009
and shipped in the firmware. `tools/fbshot.py` will not composite it as-is —
imager writes 24-bit RGB where the arena's state says 32 — but the pixels are
there and correct.

**The `.png` path hangs**, in libpng, and the raw `.rgb` path does not. That is
worth chasing before trusting the display further; `display_screen` uses PNGs.

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

## Input and audio, driven end to end

Both work now, and each had a bug that looked like something else.

**Every keypress went to the Power Button.** The viewer wrote keys to evdev
slot 1 — `gpio-keys` on a LeapPad2 — and on the Didj slot 1 is the Power
Button; the `LF1000 Keyboard` is slot 0. Brio read every press on a node it
only watches for power events and did nothing, which from the window looks
like "does not respond to any input". The codes were never the problem: the
Didj's own `LinuxKeyToBrio()` (in `Brio/Module/libEvent.so`, read out of the
disassembly) is a jump table over codes 19..108 that maps R P A H L X B M and
the four arrows to `kButton*` bits — exactly the set the viewer already sends.
Volume (114, 115) and Esc (1) fall outside the table, so Brio ignores them on
this device.

**And the arrows went out a quarter turn round.** The viewer turns the D-pad
for every LF2000 device (`DPAD_GAME_TURN` in `tadpole_view.c`, measured on
the LeapPad2 and again on the LeapPad3); this device's libEvent maps its
codes straight, so at ROT 0 in the home menu LEFT moved the selection DOWN
and RIGHT moved it UP. `DEV_DPAD_TURN=0` in the profile says so, and travels
the same way as the node roles below: the shim publishes it in `state.bin`
(`dpad_turn`, after `gl_layer`) and the viewer reads it, defaulting to the
LF2000 turn for a file that lacks it.

The slot numbers are the guest's to say, so the shim now publishes each node's
purpose in `state.bin` (`ev_role[]`, appended after the screen tail) and the
viewer, `tools/key.py` and `tools/tap.py` look the keyboard up by purpose.
`tadpole/viewer/tadpole-view --print-nodes` prints what was resolved:

    /tmp/tadpole-didj: keys=ev0 touch=ev-1 power=ev1 (published by the shim)

Measured with `TADPOLE_DIR=/tmp/tadpole-didj tools/key.py right` at the
country picker: the ring moves from USA to UK, and the shim log shows the read
on the keyboard's fd:

    [tadpole] ev0 fd=6 GUEST-GOT KEY code=106 val=1

`state.bin` growing is the part with teeth. Every reader used to demand its
own `sizeof` exactly, and `tadpole_gles_core.c` answered a mismatch by
rendering to the full panel — the Leapster scaling bug, again. Readers now
take a longer file as a newer writer's appended field (only shorter is an
error), which is the rule `main`'s `tadpole_state.h` states.

**Audio ran at 375 times real time.** Not a sample-rate problem, though it
sounds like one: `audio.fmt` and the viewer both said 32000 Hz stereo 16-bit
throughout. Measured by draining the guest's `/dev/dsp` FIFO for ten seconds:

    bytes=480387072 over 10.00s -> 48038693 B/s   (128000 expected)

The shim paced `/dev/dsp` writes to real time only when NO viewer was reading,
trusting the FIFO to supply backpressure otherwise. But the viewer drains the
pipe far faster than real time by design, into a ring it trims back to its
latency cap, so the pipe never stayed full; Brio's mixer thread rendered the
whole soundtrack as fast as qemu could go, the ring threw most of it away, and
what reached the speaker was a sampling of the song at fast-forward speed.
`tadpole_asound.c` met exactly this on the ALSA path, and its model is now
the OSS path's too: bytes drain at the byte rate, the writer is held until
what is in flight fits one device buffer (four 4096-byte fragments, 128 ms),
and the clock resyncs rather than banking credit when the buffer drains. After:

    [tadpole] dsp pace[pid 355604]: 128000 B/s in (rate=32000 ch=2 -> 128000 B/s expected) cap=16384 slept=970 ms/s

`TADPOLE_AUDIO_PACE=0` switches it off (Options → Audio → "Hold guest to
realtime" is the same switch) and `TADPOLE_AUDIO_DEBUG=1` prints that line
once a second.

One more, found by the measurement itself: closing the FIFO's read end while
the guest wrote to it killed the guest silently — SIGPIPE, no line in any log.
A process that has opened `/dev/dsp` now ignores SIGPIPE, and a run of refused
writes drops the fd so the next write re-probes for a reader. The viewer holds
its end open so this never happened in ordinary use, but a viewer that died
would have taken the guest with it.

## Host-GPU replay

Replay "froze the Didj on its copyright screen": two minutes of identical
frames, 24 draws each, no GL error and no progress, where the software
rasteriser walked on to the country picker. The guest was never stopped. It
ran at 60 fps the whole time, drawing menus into a page nobody looked at, and
three separate faults each produced exactly that picture. All three were found
by instrumenting the ring (`TADPOLE_HLE_DEBUG=1`) and the MLC ioctls
(`--debug`) and diffing against the software run, not from the window.

**1. The frame was presented on the wrong plane.** Every LF2000 device puts
its 3D surface on fb1, and the rasteriser, the host replayer and the viewer's
compositor all assumed so. The Didj's libDisplay configures exactly one RGB
layer, read out of its MLC ioctl trace:

    layer0: format 4, hstride 1280, address 0, enable 1     (the RGB plane)
    layer1: asked for its address, never configured
    layer2: format 1, hstride 4096, address 0x20000000      (YUV, the boot movie)

Its OpenGL context renders into layer0's buffer. The software rasteriser
opens `/dev/fb1` and had been landing on the right bytes by accident — every
plane aliases arena offset 0 here — while the replayer wrote fb1's page,
which fb0 aliased, so the viewer diverted the frame to a shadow page it would
composite as fb1. The Didj never enables fb1. `DEV_GL_LAYER=0` in the profile
now says which plane GL is; the shim publishes it in `state.bin` (`gl_layer`,
appended after `ev_role`, stored plus one so that zero means "not
published" — a longer file from another branch's shim must not read as
fb0), the rasteriser opens that node, and the viewer
presents into that plane's page and composites it opaque rather than
alpha-keyed, since it IS the picture. Every other device keeps fb1.

**2. Every draw after the first resync had no vertex array.** The Didj's UI
binds five texture names (4, 7, 9, 20, 21) it never uploads, so the host asks
for a state resync on every boot. The resync begins with `TADGL_RESET`, which
drops the host's array table — every slot's enable and buffer reference — and
nothing rebuilt it. The per-draw path should have: it re-sends a VBO array's
reference at each draw, except that it tested the pointer before the binding,
and with a VBO bound the pointer is the byte offset, which is 0. So an
offset-0 VBO array was skipped as "unset", the host drew from the reference
it last held, and after the reset it held none:

    drawelem pkts 1200 skipped-nobuf 0 no-array 1180

`hle_send_array` now gates on the binding, as `glDrawElements` already said
to for its indices, and `hle_sync_arrays` re-sends every array's enable and
reference after the reset.

**3. Once the draws landed, the frame was white.** The resync re-uploaded
every texture as a fresh host object with GL's default parameters, and the
default minification filter wants mipmaps the upload does not supply. An
incomplete texture is sampled by desktop GL as if texturing were off, so
every quad came out in the vertex colour. The guest had the filters all along
(`struct gl_texture`); the resync now sends them after each image, with
`GL_GENERATE_MIPMAP` bound first so the driver honours it, and restores each
unit's binding afterwards.

The five never-uploaded textures still draw white on both paths. That is the
"missing textures" bug the old note called separate, and it still is.

## What is left

1. **It only runs on qemu-arm.** `runtime/devices/didj.conf` sets
   `DEV_ENGINE=qemu` for that reason. Under glasspole the Didj reaches its
   copyright screen and stops — no assert, no crash, and not one GL draw call,
   where the same binaries under qemu go on to the country picker. Two syscalls
   it asks for are still missing there (36 `sync`, 149 `_sysctl`) but qemu
   refuses both as well, so that is not it. `TADPOLE_QEMU` overrides the
   default for whoever goes looking.

2. **Host-GPU replay** — fixed; see "Host-GPU replay" above. Was: freezes
   it on the copyright screen. It never froze; the frame went to a plane the
   Didj does not enable, and two resync bugs behind that.

   Still open from it: the UI binds five texture names it never uploads, and
   those draws are white on both paths. Where the device gets those pixels
   from is unread.

3. **`imager`'s PNG path hangs** inside libpng where its raw `.rgb` path does
   not. `display_screen` uses PNGs, so the boot screens are not reachable
   through the device's own tooling yet.

   The MLC contracts that ARE settled, all read out of `usr/bin/imager` rather
   than guessed:

   | ioctl | meaning | answer |
   |---|---|---|
   | `_IO('m', 25)` | `get_address` | 0 — an offset into the arena, not the hardware's 0x82000000 |
   | `_IO('m', 29)` | `get_fbsize` | `w*h*4*NBUF` |
   | `_IOR('m', 14)` | the layer rectangle | four words, `{left, top, right, bottom}`, inclusive |
   | `_IO('m', 9)` | `set_address` | accepted; the video layer then maps at `0x20000000 + address`, which is why the arena reaches past the Didj's DRAM base |

   The rectangle took a disassembly: imager computes `width = buf[3]-buf[0]+1`
   and `height = buf[2]-buf[1]+1`, so it was never a packed width and height,
   which is what four failed guesses had assumed.
5. **`/Didj` versus `/LF`** everywhere the emulator assumes the latter.
   `install-firmware.py` knows the difference; `tadpole.sh`, `run.sh` and the
   viewer do not yet, which is why there is no "play" for this device.
6. **`Pa_StartStream`** — fixed; see shim/tadpole_portaudio.c. Was: Audio itself works — see below — but
   Brio's main thread is left in a futex wait after portaudio creates its
   callback thread, so nothing after audio init runs. The callback thread is
   healthy and producing 16 KB buffers on time, which is the odd part. The
   likely answer is the one `tadpole_asound.c` reached for ALSA: replace
   `libportaudio.so` outright rather than emulate a device well enough for a
   2008 copy of it. Brio imports only nine `Pa_*` symbols, so the surface is
   small.
7. **`pipe`** — implemented, in both host backends. Was: (ARM syscall 42), so any guest shell
   script with a pipeline or a `$(…)` fails there with "pipe call failed" —
   and the Didj boots through `usr/bin/launch_main`, which is a shell script.
   It needs `gp_pipe` in `host.h` and an implementation in BOTH backends;
   adding it to `host_posix.c` alone is the failure mode that file exists to
   prevent. qemu-arm has no such gap.
8. **`runtime/setup-sysroot.sh` cannot build this tree**, and says so rather
   than building nonsense over it — both write to `runtime/sysroot`, and the
   LeapPad layout it assembles would overwrite a working Didj. It still
   *switches* to the Didj; rebuilding means re-running the installer.
9. **Fields not yet read** out of the image, deliberately absent from
   `runtime/devices/didj.conf` rather than guessed: `DEV_UIPKG`, `DEV_SPLASH`,
   `DEV_SOUNDS`, `DEV_CODEC`, and the `DEV_*_DEV` node names. Nothing is
   invented under the sysroot's `/sys` or `/flags` either, for the same reason:
   the LeapPad's values were read off real hardware and nobody has read a
   Didj's.

What is NOT a worry: the panel is 320x240 (all eight boot screens agree), there
is no wifi, no Qt, and no touchscreen — so none of the three blockers that cost
the LeapPad3 port apply here.
