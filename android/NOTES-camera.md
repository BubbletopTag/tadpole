# Camera — what the guest actually asks for

Tadpole has never had camera support. This is what an afternoon of measuring
found, so that whoever implements it starts from facts rather than from the
shape of the problem.

**Short version: it is a V4L2 device on `/dev/video0`, the shim already has the
exact mechanism it needs, and the hard part is not the interception — it is
deciding what pixel format to hand back.**

Everything below was measured with `TADPOLE_STRACE=1` against
`./tadpole.sh --app CameraWidget`, on the desktop, in August 2026.

---

## The chain, from the app down

`CameraWidget/App.so` → `libCameraMPI.so` → one of two backend modules, chosen
at run time:

| | |
|---|---|
| `libCameraVIP.so` | the built-in camera, through the `lf2000_vip` kernel driver |
| `libCameraUSB.so` | a USB (UVC) camera |

**Which one is picked, and this is the part that is not obvious.** Brio stats
`/sys/devices/platform/vip.0/driver` — not the device, the **driver symlink**,
which the kernel only creates once a driver has bound. `etc/init.d/camera`
agrees:

```sh
if [ -e /sys/devices/platform/vip.0 ]; then
        modprobe lf2000_vip
```

Our sysroot has neither path, so Brio fell through to the USB module every
time, and the USB module said:

```
CameraModule::CameraListener::Notify: USB camera missing 0
```

which reads like "there is no camera" and actually means "I am the wrong
backend, and there is no USB camera either".

### The USB path is a dead end, and here is how far it goes

Worth writing down so nobody repeats it. `libCameraUSB.so` scans
`/sys/class/usb_device/`, and for each entry reads
`<entry>/device/idVendor` and `<entry>/device/idProduct`. Creating a
plausible entry gets those files opened and read — the trace shows
`read(18,...) = 5` for `"046d\n"` — and then it still reports the camera
missing, because it compares the IDs against constants compiled into the
module. Finding them means disassembling: the strings are at file offsets
`0x14918`/`0x14956`/`0x149d6` in `libCameraUSB.so`, and the code is PIC, so
there are no absolute references to grep for.

Do not bother. The VIP path is the real one and it is far simpler.

### The VIP path, which is the whole answer

Create the driver node in the sysroot:

```sh
mkdir -p runtime/sysroot/sys/devices/platform/vip.0/driver
```

and the very next thing the guest does is:

```
open("/dev/video0", O_RDWR|O_NONBLOCK) = -1 errno=2 (No such file or directory)
```

No USB enumeration, no vendor IDs. It opens the V4L2 node directly, and the
only reason it fails is that nothing answers.

---

## Why this is a small job and not a large one

`tadpole_shim.c` already does exactly this shape of work for two other
devices, and says so in its own header:

> The pixel data path deliberately involves no interception at all: `open()`
> hands back a descriptor onto a plain host file, so the guest's `mmap()` is a
> real shared mapping of that file. The native viewer mmaps the same file and
> both sides see the same pages. **Only the control path (ioctl) is emulated.**

V4L2 mmap streaming is that arrangement exactly, pointing the other way:

| framebuffer (exists) | camera (to write) |
|---|---|
| `fb_index("/dev/fb0")` → host file | `video_index("/dev/video0")` → host file |
| guest mmaps it, writes pixels | guest mmaps it, **reads** pixels |
| viewer reads the arena and composites | viewer/Android **writes** frames into it |
| fb ioctls emulated (`FBIOGET_VSCREENINFO`…) | V4L2 ioctls emulated (below) |

The ioctls a streaming capture client uses are a short list, and the guest
enumerates before it configures — `v4l2_fmtdesc`, `v4l2_frmsizeenum` and
`v4l2_frmivalenum` all appear in the module's symbol table, so it calls
`VIDIOC_ENUM_FMT`, `ENUM_FRAMESIZES` and `ENUM_FRAMEINTERVALS`. Then
`QUERYCAP`, `S_FMT`/`G_FMT`, `REQBUFS`, `QUERYBUF`, `QBUF`, `DQBUF`,
`STREAMON`, `STREAMOFF`.

Because we answer the enumeration, **we choose what the camera can do.** Offer
exactly one format at one size at one frame rate and there is no negotiation to
get wrong.

### The one genuine question: which format

`libCameraVIP.so` links libjpeg (`jpeg_read_header`, `jpeg_read_scanlines`, and
a `JPEG_METHOD` argument to `CCameraModule::RenderFrame`), so it expects
**MJPEG** at least some of the time. Whether it will accept an uncompressed
format if that is all we advertise is the first thing to measure, and it is
cheap to measure: answer `ENUM_FMT` with only `YUYV`, and see whether it
proceeds or refuses.

That matters because it decides the work on the Android side:

- **If YUYV or similar is accepted** — Camera2 NDK gives `YUV_420_888`, so the
  host converts per frame. 480x272-ish at a few fps is nothing.
- **If it insists on MJPEG** — Camera2 can produce JPEG directly, and the
  frames could pass through untouched, which is *less* work, not more.

`CCameraModule` also mmaps `/dev/mem` and `/dev/fb2` for the preview path, and
fb2 is the YUV video layer the viewer **already** emulates and composites (it
is how in-title FMV works). So the preview may largely fall out for free.

---

## What it would take

**On the desktop first, not on Android.** A Linux host has V4L2 already, so the
host side is `open("/dev/video0")` on the real webcam and a copy into the shared
buffer — a few hundred lines including the ioctl emulation, with a real camera
to compare against. Get it right there, where `TADPOLE_STRACE` and a webcam are
both available, and Android becomes a backend swap rather than a bring-up.

**Then Android.** Camera2 NDK (`libcamera2ndk.so`, API 24, so within the API 26
floor) captures into an `AImageReader` and the frames go into the same shared
buffer. Needs `android.permission.CAMERA` in the manifest and a runtime grant —
one more prompt beside the storage one that is already there.

**Rough shape of the work**, in the order that keeps it measurable:

1. `video_index()` in the shim, beside `fb_index()` and `ev_index()` — an
   `open("/dev/video0")` that returns a descriptor onto a host file.
2. Emulate the enumeration ioctls, advertising exactly one mode. Measure what
   the guest does with it.
3. A frame buffer in the arena plus `QBUF`/`DQBUF` bookkeeping.
4. Host side: V4L2 capture on Linux.
5. Android side: Camera2 NDK into the same buffer.
6. `runtime/setup-sysroot.sh` should create `sys/devices/platform/vip.0/driver`,
   because without it the guest never asks in the first place.

Steps 1–4 are testable on this desktop today. Nothing in them is Android-specific.

## What is not known yet

- Whether MJPEG is required or merely supported.
- What resolution the guest requests. The LeapPad2's own camera is low
  resolution and the panel is 480x272, so this is unlikely to be demanding.
- Whether `CameraWidget` needs anything beyond capture — it saves photos, so
  there is a write path into the guest's filesystem, which already works.
- Whether the abort seen after "USB camera missing" is caused by the missing
  camera or is a separate fault. It was not investigated; with the VIP path
  reaching `/dev/video0` it may not survive to matter.
