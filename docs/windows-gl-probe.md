# Windows GL capability probe — OptiPlex 3020 results

_Recorded 2026-08-09, from the first native Windows session. This is the probe
run that `docs/windows-next.md` refers to; the machine is the project's Windows
test box precisely because its graphics driver line is dead._

## The machine

| | |
|---|---|
| Model | Dell OptiPlex 3020 |
| OS | Windows 10 Enterprise LTSC 2021, build 10.0.19044 |
| CPU | Intel Core i5-4590 @ 3.30 GHz (Haswell) |
| GPU | Intel HD Graphics 4600 (integrated) |
| Driver | **20.19.15.4835, dated 2017-10-15** — the terminal 15.40-series release; Intel never shipped another for Haswell |
| Toolchain | MSYS2 MINGW64: gcc 16.1.0, SDL2 2.32.10, cmake 4.4.2, ninja 1.13.2 |

A second display adapter is present: **`IddSampleDriver Device`** (driver
17.57.32.886), a virtual indirect-display driver, presumably left over from
remote-desktop tooling. It exposes a display that has no physical monitor. If
SDL ever opens a window that cannot be seen, or `SDL_GetNumVideoDisplays()`
reports more displays than monitors, check
`SDL_GetDisplayBounds()`/`SDL_GetDisplayName()` per index and pin the window to
the display backed by the real GPU before suspecting anything else.

## Probe output, verbatim

`tadpole/viewer/hle_probe.c` (the runtime-loader port, built with mingw-w64
gcc, linked `-lmingw32 -lSDL2main -lSDL2 -lopengl32`, no `-mwindows`):

```
  GL_VERSION  4.3.0 - Build 20.19.15.4835
  GL_VENDOR   Intel
  GL_RENDERER Intel(R) HD Graphics 4600
  GL_SHADING_LANGUAGE_VERSION 4.30 - Build 20.19.15.4835
  compatibility profile context: obtained as requested
  GL_MAX_TEXTURE_SIZE 16384   GL_MAX_SAMPLES 8
  fixed function: glVertexPointer=yes glMatrixMode=yes glTexEnvi=yes glAlphaFunc=yes
  FBO entry points: glGenFramebuffers=yes glFramebufferTexture2D=yes glBlitFramebuffer=yes glCheckFramebufferStatus=yes
  MSAA renderbuffer counts that complete: 2 4 8
  FBO 480x272 with 16-bit depth: complete
  glReadPixels 480x272 BGRA: 0.749 ms/frame  (software raster is 78 ms)
  centre pixel BGRA   0,255,  0,255  (expect a texture colour)
  corner pixel BGRA  20, 31, 13,255  (expect the clear colour)
  clear+draw+readback: 0.908 ms/frame -> 1102 fps ceiling
  FRAME-SHAPED load: 28 draws, 913920 px (~10x a real frame), + readback
    1.394 ms/frame -> 718 fps if the guest were free
    throughput 656 Mpx/s   (software rasteriser: 1.07 Mpx/s)
    a real 93000 px frame would cost about 0.142 ms host-side
PASS host-GPU replay is viable
```

The draw was verified by pixel value, not just by absence of GL errors: the
centre came back as the test texture's green and the corner as the clear
colour, so the fixed-function path genuinely rasterised.

## Entry-point resolution

Everything resolved through `SDL_GL_GetProcAddress` against the Intel ICD.

Fixed function (exported by `opengl32.dll` itself, GL 1.1 surface):

* `glVertexPointer` — yes
* `glMatrixMode` — yes
* `glTexEnvi` — yes
* `glAlphaFunc` — yes

Framebuffer objects (past 1.1, resolved via `wglGetProcAddress` under the
hood — these are the ones the GDI 1.1 fallback would NOT have):

* `glGenFramebuffers` — yes
* `glBindFramebuffer` — yes
* `glFramebufferTexture2D` — yes
* `glCheckFramebufferStatus` — yes
* `glBlitFramebuffer` — yes
* `glGenRenderbuffers` / `glBindRenderbuffer` / `glRenderbufferStorage` /
  `glRenderbufferStorageMultisample` / `glFramebufferRenderbuffer` /
  `glDeleteRenderbuffers` — yes, all

## Timings

All at the panel's 480x272, RGBA8 offscreen FBO, `GL_BGRA` readback:

| Measurement | Result |
|---|---|
| `glReadPixels` alone | 0.749 ms/frame |
| clear + 1 draw + readback | 0.908 ms/frame (1102 fps ceiling) |
| frame-shaped load: 28 draws, ~914k px, + readback | 1.394 ms/frame (718 fps) |
| implied cost of a real ~93k px frame | ~0.14 ms host-side |

**Caveat, attached to the number on purpose:** the 28-draw load is synthetic —
it matches the Clam Prix menu's measured draw-call count and overshoots its
painted pixels roughly 10x, so it is a conservative proxy, not a replay of a
real command stream. First answer, not a benchmark.

## What it settled

* GL 4.3 **compatibility profile granted as requested** — the fixed-function
  replayer design ports to this driver unchanged.
* **No Mesa/llvmpipe bundling at launch.** The feared GDI OpenGL-1.1 fallback
  (no FBOs) did not appear on the worst plausible real driver. llvmpipe stays a
  later fallback for genuinely driverless machines.
* ~10x GPU headroom against a 60 fps panel on a 2013 iGPU with a 2017 driver.
  The GPU is not the constraint on Windows; the ARM side will be.
