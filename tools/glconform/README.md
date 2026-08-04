# glconform — differential GLES1 conformance probe

One ARM binary. Run it on the real LeapPad2 and under Tadpole, diff the two
logs. A divergence is a candidate bug instead of a guess.

```sh
./tools/glconform/build.sh                        # -> runtime/glconform
./tools/glconform/run-emu.sh                      # -> tools/glconform/emu.log
./tools/glconform/run-hw.py 192.168.0.111         # -> tools/glconform/hw.log
./tools/glconform/diff-conform.py tools/glconform/hw.log tools/glconform/emu.log
```

`diff-conform.py` exits non-zero when anything diverges, so it works as a
regression gate as well as a report.

## Why one binary and not two

It links the device's own library **filenames** — `libopengles_lite.so`,
`libEGL.so` — which exist both on the device and in `runtime/shimlibs-gl`. Those
firmware libraries carry no SONAME, so `DT_NEEDED` records exactly those names,
and `LD_LIBRARY_PATH` alone decides which implementation answers. Linking
against our shims instead would bake in their SONAMEs (`libEGL.so.1`,
`libGLESv1_CM.so.1`), and neither file exists on the device.

See `build.sh` for why the NEEDED list has nine entries rather than two — short
version: the stock GL stack leaves 118 symbols undefined and declares no
dependencies of its own to satisfy them.

## Reading a log

```
META    tool=glconform version=2
META    gl_vendor="NEXEL" gl_renderer="VR5" gl_version="OpenGL ES CM 1.0"
EGLINIT OK|FAIL detail="..."
RESULT  <test> OK|FAIL|SKIP err=0x<hex> detail="..."
```

**Read `selfcheck.error_tracking` first.** It calls `glEnable` with a
deliberately invalid enum and expects `GL_INVALID_ENUM`. A side that fails it
has no working error state, so no other `err=` code in that log means anything.
`diff-conform.py` says so at the top rather than leaving it to be noticed.

Do **not** run this under `TADPOLE_GL_DEBUG=2`. Several tests deliberately raise
errors and deliberately call unimplemented entry points; level 2 turns the first
of those into an abort. `run-emu.sh` forces the level down and says so.

## What it found on its first hardware run

The device is the authority on all of these — none were guesses:

| | device | Tadpole was | now |
|---|---|---|---|
| `MAX_MODELVIEW_STACK_DEPTH` | 32 | 16 | 32 |
| `MAX_PROJECTION_STACK_DEPTH` | 32 | 16 | 32 |
| `MAX_TEXTURE_STACK_DEPTH` | 16 | 16 | 16 |
| `MAX_TEXTURE_SIZE` | 4096 | 1024 | 4096 |
| `MAX_TEXTURE_UNITS` | 2 | 4 | 2 |
| `MAX_CLIP_PLANES` | 1 | unanswered (0) | 1 |
| `SUBPIXEL_BITS` | 4 | unanswered (0) | 4 |
| `STENCIL_BITS` | 0 | unanswered (0) | 0 |
| `MAX_PALETTE_MATRICES_OES` | 32 | 32 | 32 |
| `MAX_VERTEX_UNITS_OES` | 4 | 4 | 4 |

The stack depth is the one that mattered. It is not a wrong answer to a query,
it is a **smaller stack**: a title nesting past 16 had its 17th `glPushMatrix`
silently dropped here while it succeeded on the device, and every matrix after
that point diverged — including the ones
`glLoadPaletteFromModelViewMatrixOES` snapshots for skinning.

It also confirmed two things that were open assumptions:

* **`eglCreateWindowSurface`'s native window.** `tadpole_egl.c` says the real
  driver wants a Brio handle "of undocumented layout" whose first two words are
  the panel size. Passing exactly that — `{480, 272, 0, ...}` — brings EGL up on
  real hardware. The NULL that fbdev platforms often accept was never tested;
  this is.
* **`glScissor(0,0,-1,-1)` raises nothing on the device**, where the spec says
  `GL_INVALID_VALUE`. It lands in `BOTH_FAIL`, which is the bucket for "the
  device is not conformant either" — matching the device is what makes titles
  work, so this one stays as it is.

## The buckets, and what to do with each

| bucket | meaning | action |
|---|---|---|
| `DIVERGE` | hardware and we disagree | the worklist |
| `VALUE_DIFF` | both pass, different numbers | check — this is where the stack depth hid |
| `BOTH_FAIL` | neither implements it | usually leave alone; match the device |
| `BOTH_OK` | agree | nothing |
| `MISSING` | one side never ran the test | a run stopped early; never a pass |

`VALUE_DIFF` exists because a plain pass/fail diff would have shown the stack
depth as two green ticks.

## Current state

24 tests agree. The six that diverge are all the same root cause — the entry
point is still a no-op stub in `tadpole_gles_stubs.c`:

* `light.ambient_roundtrip`, `light.position_roundtrip` — `glLightfv`/`glGetLightfv`
* `material.diffuse_roundtrip` — `glMaterialfv`/`glGetMaterialfv`
* `texenv.decal_roundtrip` — `glTexEnvi`/`glGetTexEnviv`
* `texparam.wrap_roundtrip` — `glGetTexParameteriv`
* `clipplane.roundtrip` — `glClipPlanef`/`glGetClipPlanef`

`matrix.identity_readback` already moved to `BOTH_OK` when `glGetFloatv`,
`glGetFixedv` and `glGetBooleanv` were implemented — which is what the loop
looks like.

Implementing a cluster and re-running is the loop. The tests do not need
changing as entry points land — they start passing.

## Adding a test

Each test is an independent `static void t_foo(void)`: drain the error state, do
the thing, print one `RESULT` line, bump `g_ok`/`g_fail`. Add the call to
`glconform_main()`. No framework changes — this is meant to grow to dozens more.

Two rules the existing tests follow:

* **No float ever reaches `printf`.** The guest ABI is softfp and a float
  through varargs is promoted to double in core register pairs; getting that
  subtly wrong corrupts the log instead of failing loudly. Print thousandths as
  `int` — `milli()`.
* **Never report the output of a rejected `glGet`.** A rejected query leaves its
  destination untouched, so the value is this harness's own poison on one side
  and anything at all on the other. Printing it manufactures a divergence out of
  two implementations that agree exactly.
