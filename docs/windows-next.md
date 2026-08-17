# Tadpole on Windows — brief 2

_Written 2026-08-09 on the Linux box, after the GL probe came back. Read
`docs/windows-handoff.md` first if you have not; it holds the context and the
decisions. This one assumes it._

## What you established, and what it decided

The probe on the OptiPlex 3020 reported:

```
GL_VERSION  4.3.0 - Build 20.19.15.4835   (Intel HD Graphics 4600)
compatibility profile context: obtained as requested
fixed function + FBO entry points: all resolve, and draw correctly
frame-shaped load (28 draws, ~10x real pixels): 1.394 ms -> 718 fps
```

That is the good ending, and it settles three things:

* The replayer's fixed-function compatibility-GL design **ports unchanged**.
* **No Mesa/llvmpipe bundling on day one.** It stays a later fallback for
  genuinely driverless machines, not a launch requirement.
* A 2013 iGPU on a 2017 terminal driver has roughly 10x headroom against a
  60 fps panel. The GPU is not going to be the thing that stops this.

Keep the caveat attached to the number: 28 synthetic draws are a proxy for a
frame, not a real command stream. It is a first answer, not a benchmark.

## A. Get this into git before anything else

Your tree is a source drop with no `.git`, so the ported `hle_probe.c` exists on
exactly one disk and nowhere else. Fix that first, and do not start task C until
it is done.

1. Copy your modified `hle_probe.c` (and anything else you touched) somewhere
   safe outside the tree.
2. `git clone https://github.com/BubbletopTag/tadpole.git` into a fresh
   directory and work there from now on.
3. Re-apply your changes onto the clone. Read the file you are replacing before
   overwriting it — the clone may have moved on.
4. Commit straight to `main`. **The commit message is a changelog** — it gets
   published verbatim as a release body, so write it for whoever reads the
   release, and never put a session link in it.
5. Push.

## B. Write down the probe results

They cost a session to obtain and currently live only in a chat log. Add
`docs/windows-gl-probe.md` recording:

* Windows build, CPU, GPU, and **the exact graphics driver version and date**
* the full probe output
* the entry-point resolution results, split into fixed-function and FBO
* the timings, with the note that the load was synthetic
* the `IddSampleDriver` second adapter you spotted — and a line on what to
  check if SDL ever opens a window on a display that is not there

## C. Task 3 — the GL function loader, and only that

`tadpole/viewer/tadpole_hle.c` and `tadpole_view.c` do
`#define GL_GLEXT_PROTOTYPES 1` and call GL entry points directly. That works
against Mesa, which exports everything; on Windows `opengl32.dll` exports **only
OpenGL 1.1**, so every FBO call, `glActiveTexture`, `glBufferData` and
`glBlitFramebuffer` must be resolved at runtime.

* Build one resolved function-pointer table, in the pattern your probe port
  already demonstrates.
* Enumerate the entry points from the source rather than trusting any count in
  these docs.
* **It must still build and run on Linux.** This is an improvement there too,
  not a Windows fork. No `#ifdef _WIN32` anywhere a portable call exists.
* Push when it compiles. The Linux side gets verified on the Linux box — you
  cannot check it, and a portable change that nobody compiles on Linux stops
  being portable within about a day.

### Explicitly not in scope

Do **not** touch the viewer's POSIX surface — `sys/mman.h`, FIFOs, `sys/wait.h`,
`signal.h`. You correctly flagged that it interacts with the one-process
decision, and it does: on Windows the emulator and viewer become threads in a
single `.exe`, at which point the framebuffer, the GL ring, the audio ring and
input stop being shared files and become ordinary memory. Rewriting that code
against the current two-process shape would be work thrown away. It comes after,
and it comes with a design.

Do not start on the ARM emulator core. That is `glasspole/` on the Linux box,
where `qemu-arm` sits beside it as an oracle to diff against.

## How to report

The bridge between the two machines delivers **inbound only** — messages reach
you, your replies do not come back. Everything travels through the user by hand.

So: keep chat reports short, and put anything that must survive in the repo as a
committed file. A finding that exists only in a reply is a finding that gets
lost in transit.
