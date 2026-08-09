# Pet Pad on Windows: where it stops, precisely

_2026-08-09, the session that chased it. Everything below is measured on the
OptiPlex against commit 1e31fa8 (chdir) plus the Windows stopgap; the shim was
cross-compiled here with `make shim shimz asound gl`, which works under MSYS2
unchanged._

## The one-line summary

saplayer boots deep and healthy, settles into its normal event loop — and the
movie never starts: in steady state the trace contains **zero ioctls**, so
FBIO_WAITFORVSYNC and the framebuffer flip path are never entered. The engine
idles; the timeline does not advance; the write-back framebuffer is black at
vsync=2. Nothing crashes and nothing errors. Whatever kicks a directly-launched
saplayer into playing on Linux does not arrive on Windows.

## How far it gets (identical every run)

Display module maps all three layers, CreateHandle/RegisterLayer at 480x272,
Flash player created (LFCurry.ttf found, locale en-us, IsLFPlatform true),
audio codec socaudiolfp100 found, hwparams set, CallbackThread started,
accelerometer plugin rate/mode set, third listener added — then silence.
On Linux the same command continues within seconds into ActionScript's own
trace lines (SoundManager::HandleLoad et al). Those never appear here, in
runs up to 12 minutes.

## The steady-state signature (from a usable --trace, see gp_log fix)

~1,400 syscalls/s across five guest threads:

* main: gettimeofday in bursts, `_newselect(nfds=1, read set) = 0` (a
  timeout-paced wait on one descriptor), `sysinfo` — the classic Flash Lite
  scheduler loop, cycling fast and freely
* one thread in `mq_timedreceive = ETIMEDOUT` every 100 ms, forever — a
  message queue nothing ever feeds
* two threads polling AF_UNIX sockets at 1 s intervals, always empty
* audio thread: paced writes attempting the FIFO, retrying (see below)
* **no ioctl anywhere** once setup ends — the render path is not running

## Ruled out on this box, so nobody re-chases

* **JIT speed.** md5sum of 700 KB: 0.2 s; `strings` over the same: 0.3 s.
  Branchy and bulk code both run at full speed.
* **The clocks.** gp_mono_ns measured 1.008 s across a 1.000 s sleep;
  gp_wall_ns equally sane; QPF 10 MHz. (An earlier "the guest is 100x slow"
  reading was an artifact — see the tracing tax below.)
* **Lock serialization.** Every blocking syscall (nanosleep, futex, mq, poll,
  select) unlocks the machine lock first; verified in source at this commit.
* **sysinfo lying about memory** (128 MB/96 MB, shared code, same on Linux).
* **The shared-layer bugs fixed today** (chdir, set_tid_address, getdents
  truncation, rename): all pulled, rebuilt, retested — no change to this stop.

## Found along the way

* **busybox sh cannot fork:** getppid (64) and wait4 (114) are ENOSYS. Every
  `while [ ... ]` loop in ash tries to fork per iteration and limps. Reported,
  not patched — syscall.cpp is the Linux side's surgery table.
* **msvcrt's unbuffered stderr made --trace useless:** character-at-a-time
  writes throttled a traced guest to 23 syscalls/s, which convincingly
  impersonated an emulator bug. gp_log on Windows now buffers and flushes per
  line; traced throughput rose 60x. The POSIX backend never had the problem.
* **gp_mkfifo's ENOSYS presents exactly as hoped:** logged, non-fatal, and the
  audio path retries per paced write — spammy but harmless, and the pacing
  keeps guest time honest regardless.
* **The stopgap works as specified:** six private mappings, write-back on exit
  AND on console-close (a Flash title never exits by itself, and a hard kill
  would skip atexit). fbshot.py composites a valid 480x272 frame from the
  written-back arena — black, correctly, since nothing drew. One wart: the
  write-back writes page-rounded lengths, so state.bin grows from 200 bytes to
  4096 and fbshot warns before proceeding.

## The question for the Linux side

Same command, same sysroot, same shim sources: what does the working Linux
run's trace show at this point that mine does not — and what carries the
"start playing" signal to a directly-launched saplayer? The two candidates
visible from here are the event FIFOs (they exist on Linux even with no
reader; on Windows the open fails outright) and whatever sits behind the
AF_UNIX socket that two threads poll at 1 Hz. A five-minute diff of traces
at the divergence point should name the carrier; this box can then either
provide that carrier honestly or wait for the one-process design, whichever
the answer implies.
