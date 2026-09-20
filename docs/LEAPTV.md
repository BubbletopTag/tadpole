# LeapTV — porting notes

Working notes for the `leappad-emu-3` branch, in the spirit of `LEAPPAD3.md`:
what was tried, what it did, and the measurement behind it.

Firmware under test: **7.0.1.2634**, built 2014-10-07, `Device="LeapTV"`,
codename **GLASGOW**. LF3000 (NXP4330), 1280x720 over HDMI, no touchscreen,
a Bluetooth controller whose glowing tip the camera tracks.

**A proof of concept.** The shell boots, draws on the host GPU at the panel's
size and takes clicks; the wand's *location* does not move yet, and no title
runs. Everything below was measured on this machine in one evening.

---

## Where the firmware comes from

The "DONUT" recovery image (archive.org/details/leaptv-donut) is the payload
the surgeon writes to the eMMC: three GNU tars — `RFS` (the root), `BULK`
(`/LF/Bulk`, titles included) and `FAT32` (the kernel) — plus the surgeon's
own kernel. The RFS is the same shape as the LeapPad3's tar, so it needed no
new extraction; it needed an installer that takes three bare tars instead of
an `.lfp`:

```sh
./tools/install-leaptv-donut.sh DONUT.zip     # or a directory holding the tars
./tadpole.sh --device leaptv --boot
```

It lays out `rootfs/leaptv-7.0.1.2634/{emmc_rfs,bulk,fat}` and runs
`setup-sysroot.sh leaptv`, which now mirrors a `bulk/` sibling into `/LF/Bulk`
the way it mirrors `LF/Base`: the databases and `Data/` copied once, everything
else shadowed so the package daemon's `meta.inf` rewrites land in the sysroot.

The CDN also answers 200 for `packages/0x00300001/THD1-0x00300001-000141.lf2`,
the newest marker in this image's `Firmware/`, so a retail firmware exists
online; no package list has been transcribed for it.

## What works

`shots/` — the wrist-strap page of GlasgowUI's child scene at 1280x720, 56 fps
under host-GPU replay, shader blur and all. Clicking in the viewer presses the
controller's buttons (see *The pointer*), and the shell navigates.

## Five things between the tar and a picture

**1. The shell is GLES 2.** Every system app — GlasgowUI, OutOfBox,
ParentSettings, VideoWidget, ErrorWidget — imports glCreateShader,
glUseProgram and glVertexAttribPointer, with fourteen small ESSL 1.00 shaders
under `LF/Base/GlasgowUI/Shared/Shaders`, and Pet Play World's `App.so` links
libGLESv2 too. The shim aliased libGLESv2.so to the GLES1 rasteriser.

`shim/tadpole_gles2.c` is the programmable half: shaders, programs, uniforms,
generic attributes, thirteen new opcodes appended to `tadpole_glcmd.h`, and a
host replay in `viewer/tadpole_hle.c` that compiles the ESSL as-is under
ARB_ES2_compatibility and falls back to a GLSL 1.20 translation. The one hard
problem is that there is no reply channel for glGetUniformLocation and
glGetAttribLocation, so the guest numbers them itself from the shader source
and tells the host the name behind each number. glPixelStorei stopped being a
stub, because the text arrives as GL_ALPHA textures whose rows are padded.

**2. One object for every GL name.** GlasgowUI links libGLESv2.so at the first
level and libEGL.so only transitively, through six of its own libraries. A
shim living only in libEGL.so arrives too late in its symbol scope and its own
`dlsym(RTLD_NEXT, "open")` came back round to itself — the guard in
`tadpole_shim.c` stopped it with exit 70. So the Qt-device shim is now a single
object carrying the GLES sources, and every GL name in `runtime/shimlibs-egl`
is a symlink to it; uClibc recognises an already-mapped file by inode, so a
guest naming two of them still gets one copy.

**3. RTLD_NEXT is not "the next definition".** In GlasgowUI the shim's
`dlsym(RTLD_NEXT, "dlopen")` returned NULL, every dlopen in the guest then
failed silently, and the loader's leftover "Unable to resolve symbol" was what
every Brio module load reported — an afternoon spent looking for a missing
symbol that did not exist. uClibc's do_dlsym walks the symbol-table chain from
the entry after the caller and searches each later module's scope, so whether
a libdl is reachable depends on the executable's link order. The shim now
finds the real dlopen by walking the loader's object list (`dl_iterate_phdr`)
and reading libdl's own dynamic symbol table; `tools/dlprobe.c` is the guest
program that proved it.

**4. The chained stock EGL was another device's.** `libEGLreal.so` is built
once, from whichever rootfs was live, and never refreshed on a switch; on this
checkout it was the Ultra's 47 KB vr5 library. `tools/make-egl-real.py` on the
LeapTV's Mali libEGL.so fixed the run; refreshing it on every switch, as
`real-libs.py` does for libdl, is still to do.

**5. Two 480x272 assumptions.** The core refused any GL layer window larger
than the LeapPad2's panel and defaulted to it, so the shell drew squeezed into
one corner; the bound is now the real panel from TADPOLE_W/H when the host is
replaying. And the viewer built its GPU target at the size state.bin said last
time — 480x272 on a first run — before learning the panel; `hle_host_resize()`
rebuilds it when the panel is known. A saved 4x render scale on a 720p panel
was 5120x2880 and a rebuild every frame (the viewer compared against the
granted scale, not the requested one); there is a 10 Mpx budget now.

Also: Glasspole has no SysV semaphores (ARM 299/300), and AppServer's QWS
display lock is one, so the profile names qemu as its engine, like the Didj's.

## The pointer

On a LeapTV the pointer is the controller's glowing tip as seen by the camera:
libVisionMPI's wand tracker turns the blob into a location, libControllerMPI
hangs it off the HWController object, and GlasgowUI navigates by hotspots.
Neither the Bluetooth stack nor the camera exists here.

GlasgowUI touches the controller through a dozen non-virtual methods, all
imported by name, and our shim sits ahead of libControllerMPI in its scope —
so `shim/tadpole_wand.c` defines those same mangled names and answers for one
controller, always connected, whose buttons are the viewer's mouse (published
to `$TADPOLE_DIR/pointer.bin` on every motion; `tools/wand.py` writes it from
a script). Everything the shell asks while `TADPOLE_WAND` is unset is
forwarded to the real library.

**The shell does not poll; it listens.** After GetAllControllers it never
calls GetButtonData or GetLocation on its own, so the stand-in posts Brio
events from the frame tick, with the real `HWControllerEventMessage` and a
`CEventMPI` of its own. The event types are computed at load into
libControllerMPI's `.bss`; on this build they read, in order from `+0x1a0a8`,
`0x10007002` .. `0x1000700b`, and the constructor's three pre-built messages
use `0x10007004`, `05`, `06`. Measured by posting them:

| type | posting it made the shell... |
|---|---|
| 0x10007004 | read the analog stick |
| 0x10007005, 06 | read the buttons and act on them (**the default**) |
| 0x10007009 (AddController) + 0x10007007 | ask the mode and re-read the controller list |
| 0x10007002, 03, 08, 0a | nothing visible |

**The location is a vision event, not a controller one.** The one place
GlasgowUI calls `HWController::GetLocation` is an event handler that first
checks the message type against five values in its own data —
`0x10005001`..`04` and `06`, the *vision* group, which the wand tracker posts
from camera frames — and the shell logs `kCameraRemovedEvent` and never starts
its VNVisionMPI. Posting those five with a controller message does nothing,
because nothing is listening. So the cursor needs the camera: either a fake
`/dev/video0` feeding a synthetic frame with a blob at the mouse (the faithful
route, since the real tracker and hotspots then run), or a stand-in for
libVisionMPI's C++ surface. That is the next piece of work, and it is not
small.

`TADPOLE_WAND_EVT`, `TADPOLE_WAND_CONNECT_EVT` (offsets into the table, or
literal types from 0x01000000 up), `TADPOLE_WAND_MODE` and
`TADPOLE_WAND_BUTTONS` are the knobs the measurements above were made with.

## Open

* **GlasgowUI crashes on the way out** when it hands off to a title: SIGSEGV
  in libc's `free()` at teardown, heap corruption, seen with and without the
  stand-in's vector. AppServer relaunches it, so navigation survives.
* **No title runs.** BrioWrapper crashes seconds into Pet Play World — the
  same shape as the LeapPad3's open "every Brio title dies in the wireless
  MPI", and not yet looked at here.
* **`tools/gen-gl-stubs.py` cannot read a Mali library.** It picks the first
  libGLESv1_CM.so under `rootfs/`, and the LeapTV's is Mali's combined
  EGL+GLES blob, so it reports 103 EGL entry points "missing" that
  `tadpole_egl.c` defines. The check that is right is the import diff: every
  gl*/egl* name the LeapTV's binaries import against what the shim exports,
  which was clean.
* The wrist-strap page shows on every boot. `keyWristStrapComplete` is one of
  the exit-state keys, and writing it to `/LF/Bulk/GlasgowUIexitState` did not
  skip the page.
* `libEGLreal.so` should be refreshed per device (see 4).
