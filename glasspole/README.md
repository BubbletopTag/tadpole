# Glasspole

Tadpole runs LeapPad2's ARM software under `qemu-arm`. This is a replacement for
`qemu-arm` that can exist on Windows, where `qemu-arm`'s user mode cannot.

## Why this exists

`qemu-arm -L sysroot ./AppManager` does two separable jobs:

1. translate ARM instructions into something the host CPU can run, and
2. translate the guest's Linux syscalls into host Linux syscalls.

Job 2 is written against a Linux host and always will be — QEMU has no
linux-user support for Windows and none is planned, because it would mean
implementing Linux on top of an undocumented, unstable NT syscall ABI. That
single fact is the entire reason there is no Windows Tadpole.

So: job 1 goes to **dynarmic** (the ARM JIT from the Switch and 3DS emulators —
0BSD, so GPL-compatible, and its Windows fastmem path is real rather than
stubbed). Job 2 we write, here.

## Why job 2 is small

Measured on 2026-08-09 rather than estimated. One full run of Cars 2 under
`qemu-arm -strace`: **445,553 syscalls, of only 51 distinct kinds.**

| | |
|---|---|
| 381,096 | `gettimeofday` — 85% of every call the guest ever makes |
| 46,807 | `read` |
| 5,300 | `clock_gettime` |
| 168 | `ioctl` — **every single one `TCGETS`**, libc probing for a terminal |
| 4 | `clone` — all four plain pthread creation, no `fork` anywhere |

There are **no device ioctls at all**, because Tadpole's shim already absorbs
every hardware access inside the guest: framebuffers, evdev, ALSA, EGL, GLES.
What actually reaches the kernel is boring — open a file, read it, check the
clock, start a thread. A general-purpose ARM Linux usermode emulator is a huge
project. One that has to satisfy *this* rootfs, with *that* shim already loaded,
is a finite checklist.

`docs/windows-handoff.md` has the full census and the reasoning behind the
decisions below.

## Rules this code is written to

**Nothing here calls a host syscall directly.** Everything goes through
`src/host.h`, an interface of about twenty-five functions designed to the
*Win32* capability set. Linux is one implementation of it, deliberately not
using the conveniences Windows lacks. A Linux backend that forwarded `open` to
`open` would prove nothing and would quietly bake Linux semantics into the
interface — that is the failure mode this design exists to avoid.

Consequences, all of which apply on Linux too:

* Guest memory is one reservation we manage with our own page table, and every
  guest mapping is aligned to **64 KB** — Windows' allocation granularity — so
  layout bugs surface here, where there is an oracle to catch them.
* Guest file descriptors are our own table of opaque handles. Never host fd
  numbers, so no inherit or dup semantics leak through.
* Threads go through `gp_thread_create`, never `clone`.
* Futexes go through `gp_wait_on`/`gp_wake`, which is `FUTEX_WAIT` on Linux and
  an address-keyed condition variable on Windows — **not** `WaitOnAddress`,
  which would cost us Windows 7.
* Host functions return **negative Linux errno values**. Translating
  `GetLastError()` is the Win32 backend's job, so no OS-specific error logic
  ever appears in the syscall layer.
* No `fork`, no unlink-while-open, no `/proc/self/*`. The guest's `/proc` reads
  are synthesized; on Windows there would be nothing to forward them to anyway.

**Built on Linux first, and not because Linux is the goal.** On Linux,
`qemu-arm` sits next to us as a known-correct implementation to diff against,
instruction for instruction and syscall for syscall. On Windows a divergence
looks like a black screen with the CPU, the loader and the syscall layer all
unproven at once. The Linux build has no end-user value — `qemu-arm` already
works there. It is a test rig, and it is worth building.

## Where it is

Milestone 0: a flat ARM binary executing under dynarmic, with `write` and
`exit_group` dispatched through the host interface.

```
./fetch-deps.sh          # clones and builds dynarmic (A32 only)
cmake -S . -B build -GNinja && ninja -C build
./build/glasspole tests/hello.bin
```

Next, in order: the ELF loader and `ld-uClibc.so.0`; the process image (stack,
`auxv`, TLS); threads and futexes; then the shim's shared mappings and a first
frame. Only after a title runs here does the Win32 backend get written.

## Layout

```
src/host.h          the host interface — the one file that defines the contract
src/host_posix.c    Linux backend
src/host_win32.c    Windows backend (not written yet)
src/cpu.cpp         dynarmic glue: memory callbacks and SVC dispatch
src/syscall.c       the 51
tests/hello.S       milestone 0's ARM program
```

## Licence

GPL, like the rest of Tadpole. dynarmic is 0BSD and compatible.

Do not copy code from `lindbergh-loader/linuxloader`. It was researched as a
model and rejected: it runs *x86* Linux binaries on *x86* Windows by binding ELF
symbols straight to Win32 DLLs, which cannot work for an ARM guest — and it is
CC BY-NC-SA 4.0, which is incompatible with GPL in both directions. Reading it
for ideas is fine. Taking its code is not.
