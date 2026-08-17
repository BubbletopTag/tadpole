# Tadpole on Windows — session brief

_Written 2026-08-09 on the Linux box, for a Claude Code session running natively
on Windows. Read this first; it carries the context so you don't re-derive it._

## What Tadpole is

A LeapPad2 emulator. The console's software is 32-bit **ARM** Linux: a stock
uClibc rootfs (`rootfs/stock-4.6.0.784/ubi_rfs`) running LeapFrog's `AppManager`
and per-title `App.so` binaries built on their "Brio" engine.

On Linux it works by running those ARM binaries under **`qemu-arm`** (user-mode)
with an `LD_PRELOAD`ed ARM shim (`tadpole/shim/`) that fakes the hardware —
framebuffers, evdev input, ALSA, EGL/GLES. A separate native viewer
(`tadpole/viewer/`, SDL2 + OpenGL) draws the result. 75 of 110 installed titles
reach a real screen. See `docs/STATUS.md` for where the work is, and
`docs/HANDOVER.md` (append-only engineering log) for why anything is the way it
is.

## Why Windows is hard, and what we decided

`qemu-arm`'s user-mode does two jobs: translate ARM instructions, and translate
Linux syscalls. The second half only exists on Linux and never will exist on
Windows — QEMU has no linux-user support for Windows hosts by design.

So the plan is to replace `qemu-arm` with our own program doing both jobs:

* **ARM → x86 translation: `dynarmic`.** The JIT from the Switch/3DS emulators.
  0BSD licensed (GPL-compatible), A32 frontend covers the guest exactly, and its
  Windows support is real — `backend/x64/exception_handler_windows.cpp`
  implements the fastmem page-fault path via SEH, `SupportsFastmem()` returns
  true. Verified building on the Linux box: `libdynarmic.a`, A32-only, 15 MB.
* **Linux syscalls → Win32: we write it.** Measured, not guessed: a full run of
  Cars 2 under `qemu-arm -strace` made 445,000 syscalls of only **51 distinct
  kinds**. 381,000 of those were `gettimeofday`. Every one of the 168 `ioctl`s
  was `TCGETS` (libc's isatty probe) — **zero device ioctls**, because the shim
  already absorbs all hardware access inside the guest. All 4 `clone`s were
  plain pthread creation (`CLONE_VM|CLONE_THREAD|CLONE_SETTLS`), no `fork`.
  The census is a finite checklist, not a research project.

### Design decisions already taken

* **64-bit only.** dynarmic has x86-64 and ARM64 backends, no x86-32. (The ARM64
  backend means Windows-on-ARM could run natively later.)
* **API floor: Windows 7 SP1 x64 where it is free; advertise Windows 10+.**
  dynarmic itself only calls `VirtualAlloc`, `VirtualProtect` and
  `RtlAddFunctionTable`. Our own code should avoid `WaitOnAddress` (Win8+) in
  favour of an address-keyed `CONDITION_VARIABLE`/`SRWLOCK` table, and avoid
  `VirtualAlloc2`/`MapViewOfFile3` (Win10 1803+) entirely.
* **One process.** On Linux the emulator and viewer are separate processes
  sharing files in `/tmp/tadpole`. On Windows they become threads in one `.exe`,
  so the framebuffer, GL ring, audio ring and input queues are just memory. This
  is what lets us skip the Win10-only mapping APIs, and it is the single `.exe`
  we want anyway.
* **The emulator core gets built on Linux first**, not here — because there
  `qemu-arm` sits beside it as a known-correct oracle to diff against. This
  session is not building the emulator.

## What this session is for

The viewer, and only the viewer. It is independent of ARM emulation, it is where
old Intel graphics will surprise us, and it can be tested with captured data
alone. Do these in order and stop to report after task 2.

### 0. Environment

Install natively on Windows — **not** in WSL; WSL would defeat the purpose.

* MSYS2 with `mingw-w64-x86_64-{gcc,cmake,ninja,pkg-config,SDL2}`, plus git
* Report: exact Windows version and build, CPU, GPU model, **graphics driver
  version and date**

The test box is a Dell OptiPlex 3020 — Haswell-era with Intel HD 4400/4600
integrated graphics, whose Windows driver line stopped at 15.40.x. That is
precisely why it is a good test box, and precisely why task 1 matters.

### 1. The OpenGL capability probe — the important one

`tadpole/viewer/hle_probe.c` already does this on Linux. Port it, or write the
equivalent, and report **exactly** what this machine gives us:

* `GL_VERSION`, `GL_VENDOR`, `GL_RENDERER`, `GL_SHADING_LANGUAGE_VERSION`
* whether a **compatibility profile** context is obtainable via
  `SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY)`
* whether the fixed-function entry points resolve through
  `SDL_GL_GetProcAddress`: `glVertexPointer`, `glMatrixMode`, `glTexEnvi`,
  `glAlphaFunc` (the four `hle_probe.c` already checks)
* whether the FBO entry points resolve: `glGenFramebuffers`,
  `glFramebufferTexture2D`, `glBlitFramebuffer`, `glCheckFramebufferStatus`
* `GL_MAX_TEXTURE_SIZE`, available MSAA sample counts

**Why this decides things.** The replayer uses fixed-function
compatibility-profile GL — `glFrustum`, `glEnableClientState`, `glAlphaFunc`,
`glColorPointer`, 78 entry points — because GLES1 maps nearly 1:1 onto desktop
GL 1.x. Every real vendor driver exposes that. The failure mode is a machine
with no vendor driver, where Windows falls back to its generic GDI OpenGL
**1.1**, which has no FBOs at all. If that is what this box reports, we need
Mesa's llvmpipe `opengl32.dll` shipped alongside the exe from day one. ANGLE is
not an option — it is GLES2/3 with no fixed function.

### 2. Report back and stop

The probe output determines the next move. Do not carry on into the port before
that is known.

### 3. Then: the GL function loader

This is the one substantial porting task in the viewer and it is mechanical.

`tadpole/viewer/tadpole_hle.c` and `tadpole_view.c` do
`#define GL_GLEXT_PROTOTYPES 1` and call GL entry points directly. That works
against Mesa, which exports everything. On Windows, `opengl32.dll` exports
**only OpenGL 1.1** — every FBO call, `glActiveTexture`, `glBufferData`,
`glBlitFramebuffer` and friends must be resolved at runtime through
`SDL_GL_GetProcAddress`.

* Build a resolved function-pointer table, in the style `hle_probe.c` already
  uses for its four probes.
* **Keep it portable.** The same code must still build and run on Linux — this
  change is an improvement there too, not a Windows fork. Avoid `#ifdef _WIN32`
  anywhere a portable call exists.
* 78 GL entry points are in use across `tadpole/viewer/*.c`; enumerate them from
  the source rather than trusting this number.

The rest of the viewer's POSIX surface (`sys/mman.h`, `fcntl.h`, `sys/wait.h`,
`signal.h`, FIFOs) also needs addressing, but it is smaller and it interacts
with the one-process decision above — raise it before rewriting it.

## Constraints

* **Tadpole is GPL.** Do **not** copy code from `lindbergh-loader/linuxloader`;
  it is CC BY-NC-SA 4.0 and incompatible in both directions. It was researched
  as a possible model and rejected — it runs x86 Linux binaries on x86 Windows
  by binding ELF symbols straight to Win32 DLLs, which cannot work for an ARM
  guest. Reading it for ideas is fine; taking its code is not.
* **Commit straight to `main`** — this project does not use feature branches.
* **Commit messages are changelogs.** They get published verbatim as release
  bodies, so write them for the person reading the release, and never put a
  session link in one.
* Don't start on the ARM emulator core here.

## Ground truth worth not re-deriving

* Guest binaries are ARMv7-A, Thumb-2, VFPv2, `Tag_ABI_align_needed: 8-byte`,
  interpreter `ld-uClibc.so.0`. (`readelf -A` on
  `rootfs/stock-4.6.0.784/ubi_rfs/LF/Base/bin/AppManager`.)
* The GL boundary is **already portable** and needs no redesign. Per
  `tadpole/shim/tadpole_glcmd.h`: the guest's GL shim libs serialise calls into
  a ring of `u16 op; u16 pad; u32 len; payload` packets with fixed-width fields,
  no pointers in any payload, arrays travelling as (buffer name, offset). The
  header states it must stay "free of any assumption about which architecture is
  reading it". It is a byte stream, not an ABI — identical bytes on Windows.
* Host-GPU replay measured ~57 fps against 11.5 for the software rasteriser;
  the software path is a fallback, not a plan.
* The panel is 480x272.
