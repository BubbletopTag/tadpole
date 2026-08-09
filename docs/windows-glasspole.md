# Tadpole on Windows — brief 3: the Windows half of Glasspole

_Written 2026-08-09 on the Linux box. Read `glasspole/README.md` first — it
explains what this thing is and why. `docs/windows-handoff.md` has the
background if you have not seen it._

## Where the viewer work landed

Task C is **complete**, not partial. My brief said to do the GL loader in both
`tadpole_hle.c` and `tadpole_view.c`; in fact `tadpole_view.c` calls exactly one
GL function, `glViewport`, which is core OpenGL 1.0 and exported by
`opengl32.dll` directly. Nothing to do there. Your scoping was right.

All three of your commits are verified on Linux: the viewer and probe build
clean, and the ported probe runs here against a completely different driver
(AMD FirePro W4100, radeonsi) and passes — compatibility profile obtained, all
entry points resolved, 1786 Mpx/s. A runtime-resolved loader is an improvement
on Linux too, exactly as intended.

## What Glasspole now does on Linux

`/bin/busybox` from the stock rootfs runs to completion. The guest's own
`ld-uClibc.so.0` resolves and relocates `libc.so.0`, sets up TLS, protects its
RELRO and hands off — using nothing but the syscalls in `glasspole/src/`. Four
of five busybox applets match `qemu-arm` byte for byte.

So `src/host.h` is no longer a proposal. It has been exercised by a real
program, and it is stable enough to implement against.

## Task D — write `glasspole/src/host_win32.c`

About twenty-five functions. `src/host_posix.c` is the reference
implementation; read it and `src/host.h` together before starting. The header
explains the contract, and it was written to Win32's capabilities from the
beginning, so nothing above it should need to change. **If you find yourself
wanting to change something above `host.h`, stop and report it** — that is a
design defect worth knowing about, not something to work around.

### Setup

```
pacman -S mingw-w64-x86_64-clang mingw-w64-x86_64-lld   # for the ARM test program
cd glasspole && ./fetch-deps.sh
cmake -S . -B build -G Ninja && ninja -C build
```

`CMakeLists.txt` already selects `host_win32.c` when `WIN32`. dynarmic builds on
Windows — its x64 backend and its SEH-based fastmem handler are upstream and
supported, so expect that part to just work.

### Constraints, and why each one

* **64-bit only.** dynarmic has no x86-32 backend. Do not attempt one.
* **Windows 7 SP1 API floor.** Specifically: **no `WaitOnAddress`/
  `WakeByAddressSingle`** (Windows 8+) — implement `gp_wait_on`/`gp_wake` as an
  address-keyed table of `CONDITION_VARIABLE` + `SRWLOCK`, which is Vista and
  costs about sixty lines. And **no `VirtualAlloc2`/`MapViewOfFile3`**
  (Windows 10 1803+) — they are not needed, because guest memory is one
  reservation we sub-allocate ourselves.
* **`gp_reserve`/`gp_commit`/`gp_decommit`/`gp_release`** map onto
  `VirtualAlloc` with `MEM_RESERVE`, `MEM_COMMIT`, `MEM_DECOMMIT` and
  `VirtualFree` with `MEM_RELEASE`. Note `MEM_RELEASE` requires the base address
  of the original reservation, which is why `gp_release` takes the size.
* **The reservation is 4 GiB** — the guest's entire 32-bit address space, so a
  guest address is `base + vaddr` with no lookup. That is fine in a 64-bit
  process; it is address space, not committed memory.
* **Paths are UTF-8 on the way in.** Widen to UTF-16 with
  `MultiByteToWideChar(CP_UTF8, ...)` and call the `W` entry points. Never the
  `A` ones — the guest's paths are not in the host's ANSI codepage.
* **`GetLastError()` → negative Linux errno lives ONLY in this file.** That
  mapping is the entire reason the interface exists. `ERROR_FILE_NOT_FOUND` and
  `ERROR_PATH_NOT_FOUND` both become `GP_ENOENT`, and so on.
* **`gp_pread` — use `OVERLAPPED`, not save-and-restore.** The comment in
  `host.h` says the backend "saves and restores", which is worse advice than it
  should be: `ReadFile` with an `OVERLAPPED` carrying the offset reads from that
  offset without touching the handle's file pointer, on a synchronous handle.
  Fix that comment while you are there.
* **Link the runtime statically** (`-static-libgcc -static-libstdc++`, and
  static winpthread) so the executable has no MSYS or MinGW DLL dependencies.
  Someone should be able to copy one file.

### What you can and cannot test there

You **can** build and run the ARM test programs. Note these are `.elf`, not
`.bin` — the flat-binary loader is gone, replaced by the real ELF loader:

```
./build/glasspole.exe --sysroot . build/hello.elf
./build/glasspole.exe --sysroot . build/thread.elf
```

`hello.elf` prints `hello from an ARM guest` and exits 0. That exercises
reserve, commit, file open/read/stat, console write and the whole JIT path.

`thread.elf` prints `child thread ran` then `parent saw the child exit`. That
one is the real test of your backend: it exercises `gp_thread_create`,
`gp_wait_on` and `gp_wake` — the three functions where Windows differs most
from Linux, and where the Windows 7 floor is decided. If your condition-variable
implementation of `gp_wait_on` is wrong, this hangs; if it is right, it prints
both lines and exits 0.

A relative program path is a HOST path; an absolute one goes through the
sysroot. That is why `--sysroot .` works above.

You **cannot** run busybox: the rootfs is not in git and never will be, since it
is LeapFrog's firmware. So "hello.bin runs" is the bar for this task. If you
want a harder test, the ELF loader path can be exercised by pointing
`--sysroot` at any directory containing an ARM binary — it will fail, and it
should fail with a clear message rather than a crash.

### Report

Commit and push as you go; findings that only exist in a reply do not survive
the trip between machines. Worth recording in the commit or in `docs/`:

* anything in `host.h` that turned out to be awkward or wrong for Win32 — that
  is the most valuable output of this task, more than the code
* whether the 4 GiB reservation succeeds, and what `VirtualAlloc` does if it
  does not
* whether anything forced you above the Windows 7 floor
