# Camera and microphone — what the guest actually asks for

**Status: working on the tablet.** The viewfinder is live on both cameras,
the shutter saves a photograph the LeapPad2's own Gallery lists, and the Video
recorder writes an AVI with MJPEG video and PCM audio from the tablet's
microphone.

    LF/Bulk/Data/Local/All/Photos/lf_photo_000009.jpg   800x600, + Preview/ Thumb/
    LF/Bulk/Data/Local/All/Videos/lf_video_000001.avi   8.04 s
      Stream #0:0  mjpeg (Baseline), yuvj420p, 320x240, 29.73 fps
      Stream #0:1  pcm_s16le, 16000 Hz, mono

The first half of this file was written before any of it worked and predicted
the shape of the job. It got the entry point right — it is a V4L2 device on
`/dev/video0` — and was wrong about almost everything after that, in ways worth
recording, because each wrong turn is a thing the next person would also
assume.

---

## The chain, from the app down

`CameraWidget/App.so` (and `VideoWidget/App.so`) → `libCameraMPI.so` → one of
two backend modules, chosen at run time by stat'ing
`/sys/devices/platform/vip.0/driver` — not the device, the **driver symlink**,
which the kernel only creates once a driver has bound:

| | |
|---|---|
| `libCameraVIP.so` | the built-in camera, through the `lf2000_vip` driver |
| `libCameraUSB.so` | a USB (UVC) camera |

Both sysroot builders now create that directory, and it is not optional. It is
also not optional for the reason the old version of this file gave.

### The USB path is a dead end, but not where it was thought to be

The old note said the USB module "still reports the camera missing" and stops.
It does not stop. It opens `/dev/video0` happily and gets all the way to
`CameraModule::InitCameraInt: completed OK` — and then does nothing for ever,
because `CCameraModule::StartVideoCapture`'s second line is

    if (this->mCameraPresent == 0) return 0;

and `mCameraPresent` is set only by the USB listener, which is looking for a
vendor ID under `/sys/class/usb_device`. So the USB path fails silently AFTER
the camera is fully initialised, which is a far better disguise than "no such
device". `CVIPCameraModule`'s constructor sets the same field from
`InitCameraInt`'s return value instead, so on the VIP path a camera that
answers its ioctls is a camera that is present.

---

## The format: ENUM_FMT must answer NOTHING

This was the open question — "answer `ENUM_FMT` with only YUYV and see whether
it proceeds". `ENUM_FMT` is the wrong lever, and offering **any** format
through it is actively harmful.

`CVIPCameraModule::EnumFormats` — the virtual the application actually calls —
does not use the V4L2 enumeration at all, unless `CCameraModule` has already
cached one. Its first act is to look at that cache, and only if it is EMPTY
does it build the list the LeapPad2 really offers, out of eight statics in the
module itself:

| always | if within `/flags/high-res` (`"%dx%d"`) |
|---|---|
| QSVGA 400x300, SVGA 800x600, QVGA 320x240, VGA 640x480 | WXGA 1280x800, SXGA 1280x960, HD16 1600x900, UXGA 1600x1200 |

and every one is `tCaptureMode.fmt` **3** — planar YUV420. That number is what
makes recording possible: `CameraTaskMain` turns `fmt` into a fourcc for
`AVI_set_video`, which sets an ffmpeg `pix_fmt` from it —

    '422P' -> 16    'YUYV' -> 1    'YU12' -> 15    anything else -> unset

— and the MJPEG encoder then refuses to open. The only two values a V4L2
enumeration can ever produce are 1 and 0, because
`CCameraModule::InitCameraInt` builds its cache with

    mode->fmt = (frmival.pixel_format == 'MJPG') ? 1 : 0;

so advertising MJPEG gives

    [tadpole] cam0: S_FMT 320x240 fourcc 47504a4d
    [mjpeg @ 0x560020]colorspace not supported in jpeg
    === tadpole: guest crashed ===  signal SIGFPE (8)

and advertising anything else gives fourcc 0, which fails the same way — plus a
garbage mode, because `VideoWidget::setInfo` searches the list for an exact
size match and leaves its copy uninitialised when it finds none:

    [tadpole] cam0: S_FMT 56308x45910 fourcc 00000000

**`/flags/high-res` does not exist on a real LeapPad2** and does here, holding
`640x480`. That is an emulator-side cap, for the arena reason below, not
anything the firmware does.

---

## The preview is not a stream, and the recording is not DQBUF-in-a-loop

Two separate surprises, and between them they are most of the work.

**The viewfinder never goes through DQBUF at all.**
`CVIPCameraModule::StartVideoCapture` points `VIDIOC_S_FBUF` at a display
surface, turns on `VIDIOC_OVERLAY`, and on real hardware the VIP block then
DMAs into the MLC's video plane while the application waits for a button.
Measured, with the viewfinder open:

    AppManager                    3% CPU
    state.bin vsync_count         13, 13, 13 — frozen
    cam0 n_qbuf=1 n_dqbuf=0       one buffer queued, not one taken

So there is no clock on the guest side to hang a preview off, and the shim
cannot draw it. It publishes WHERE the surface is instead — translating the
physical base out of `S_FBUF` into an offset in the arena — and
`tadpole/viewer/tadpole_cam.c` draws into it off the render loop it already
has. The base translates because `fill_fix()`'s own `smem_start` is what Brio
quotes back:

    [tadpole] cam0: S_FBUF base 82afc000 -> fb2 +3129344, 320x240 pitch 1920 YU12

**The recorder polls QUERYBUF for `V4L2_BUF_FLAG_DONE`.** `CameraTaskMain` runs
`TryLockMutex -> PollFrame(handle) -> ... -> TaskSleep`, and
`CCameraModule::PollFrame` walks the buffers issuing `QUERYBUF`, looking for
one whose flags carry DONE; only then does it call `GetFrame`, which dequeues.
A QUERYBUF that never says DONE produces a recording of the right length with
the right audio and not one video frame in it, and then a SIGFPE in `AVI_close`
dividing by a frame count of zero.

---

## Where the frames go, and the one place this is not like the hardware

`InitCameraBufferInt` does **not** mmap the video node. It already has the
whole of video memory mapped from its own open of `/dev/fb2`, and works the
buffer addresses out arithmetically — the two addresses in the guest's log are
the same one:

    CCameraModule: mmap 82800000: a9fe8000, len 003fc000
    InitCameraBufferInt: i=0, flags=00000000, mapping=0xa9fe8000

so `QUERYBUF`'s `m.offset` is never read, and the frame has to be written into
the arena at video memory offset 0. Buffer *i* is at `mapping + width * i` —
320 bytes apart for a 320-wide frame, which cannot be three frames, so REQBUFS
returns 1 however many are asked for.

The layout is the video plane's, the same one `blit_layer_yuv420()` in the
viewer already reads: with P the pitch, Y row *y* at `y*P`, Cb row *y* at
`y*P + P/2`, Cr row *y* at `(h/2 + y)*P + P/2`.

**P is 4096 for a recording and free otherwise.** `AVI_set_video` hardcodes
`frame->linesize[0..2] = 4096`, and `AVI_write_frame` lays the planes out as
`data[0]=buf, data[1]=buf+2048, data[2]=buf+2048+height*2048`. The still path
recovers the pitch as `frameinfo.size / frameinfo.height` and accepts anything.

And here is the one place the emulator differs from the device. On hardware
`/dev/fb2` is a separate video heap; here all three framebuffers share one
arena, because Brio allocates every surface from a single offset allocator and
pans whichever fb matches the pixel format. So video memory offset 0 is arena
offset 0, and Brio's allocator hands out its first surface at `0xFF000` — two
full 480x272x32 screens in, with everything on screen above that. `0xFF000`
bytes is therefore the entire budget for one captured frame (`TAD_CAM_HEADROOM`
in `tadpole/shim/tadpole_cam.h`). A 640x480 frame at pitch 4096 wants 1966080
and goes through both display pages; that is what turned the panel into
coloured stripes on the way out of the Camera app.

---

## The microphone

`libMicrophone.so` opens ALSA capture on `plughw:0,0` and reads with the mmap
interface — `avail_update`, `mmap_begin`, `mmap_commit` — so
`tadpole/shim/tadpole_asound.c`, which has been a complete replacement for
libasound since audio worked, now serves capture as well and hands back a real
memory area. Five symbols were missing, and one of them is fatal in a way that
reads as a camera fault:

    /LF/Base/bin/AppManager: can't resolve symbol 'snd_pcm_sw_params_malloc'

The guest loader refuses the whole module, so Brio has no microphone at all,
and the camera reports that as its own problem.

`$TADPOLE_DIR/mic` is the transport and also the switch: a FIFO with no reader
cannot be opened for writing, so "the viewer can open it" means exactly "the
guest is capturing". The viewer creates the node, because one made by the guest
belongs to root and carries the bare `app_data_file:s0` context that an app may
read but not write.

---

## The Android side

**Not the NDK.** `libcamera2ndk` would keep the frames in C with no Java
anywhere, and it enumerates nothing on this tablet:

    I/tadpole: camera: 0 device(s) on this tablet
    $ adb shell dumpsys media.camera | grep 'Camera HAL device'
    == Camera HAL device device@1.0/internal/0 (v1.0) static information: ==
    == Camera HAL device device@1.0/internal/1 (v1.0) static information: ==

Both cameras are HAL 1.0, which camera2 presents as hardware level LEGACY, and
`ACameraManager_getCameraIdList` filters LEGACY devices out on purpose — the
NDK API is defined against HAL3. So frames come through
`org.tadpole.view.CameraSource` and `android.hardware.Camera`, whose
`setPreviewCallbackWithBuffer` delivers NV21 into a buffer we own and re-queue.

**One camera at a time**, because these are HAL1 devices and
`Camera.open()` on the second while the first is held throws "Fail to connect
to camera service" — and the guest opens `/dev/video1` before it closes
`/dev/video0`. `CameraSource.start`/`stop` block until the open or the release
has actually happened, for the same reason.

`/dev/video0` is the rear camera and `/dev/video1` the front one, which is how
CameraWidget's switch button is wired. Rotation comes from
`Camera.CameraInfo.orientation` through the standard display formula at 90
degrees (the LeapPad2's camera UI is landscape); the front camera is mirrored.

---

## What is still open

* **Video recording above 320x240.** The recorder needs pitch 4096 and a
  400x300 frame at that pitch does not fit in the headroom, so it falls back to
  the minimum pitch and the video would come out striped. The widget records at
  320x240 by default; the other two quality settings are untested.
* **A separate arena for `/dev/fb2`** would remove the headroom limit entirely
  and is the real fix. It is not small: Brio's single offset allocator is what
  makes one arena work today.
* **`MicroPhoneWidget`** — the standalone microphone widget — has not been
  driven. The recorder's audio track is the evidence the capture path works.
