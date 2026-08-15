# Can the guest's ARM32 code run on Android?

This is the question that decides whether Tadpole on Android is a project or a
wish. Everything else — the viewer, SDL, the build — turned out to be easy, and
this is where the difficulty actually lives.

Measured where it could be measured; cited where it could not. Nothing here is
inferred from what ought to be true.

---

## 1. The 32-bit userspace is going away, and it is already gone on new phones

Not a trend to plan around — a line that has already been crossed.

| when | what | so |
|---|---|---|
| Oct 2022 | **Pixel 7** ships 64-bit-only. Google's own announcement calls it "the first 64-bit-only Android phone" | no 32-bit ABI, no `/system/bin/linker`, `SUPPORTED_32_BIT_ABIS` empty |
| 2022–23 | **Cortex-A715 / Cortex-X3** drop AArch32 at EL0 in silicon | not a ROM choice any more |
| Android 14 | devices with Armv9 CPUs (Snapdragon 8 Gen 1 and later) **must** be 64-bit-only to ship with it | the choice is taken away from the OEM |
| Oct 2023 | **Snapdragon 8 Gen 3** removes AArch32 entirely — Qualcomm's first | Xiaomi shipped a binary translator, "Tango", because the hardware cannot |

The measured version of that, on the API 35 emulator, which is what a current
phone looks like:

```
probe: /system/bin/linker    ABSENT — 64-bit-only ROM, nothing 32-bit can execute here at all
probe: /system/bin/linker64  present
ro.product.cpu.abilist32     (empty)
ro.zygote                    zygote64
```

No 32-bit linker on disk, no 32-bit ABI advertised, and a zygote that cannot
fork a 32-bit child. On that device **nothing** ARM32 executes, by any means,
at any privilege an app can reach.

## 2. But the target hardware is exactly where 32-bit still lives

The user's stated target is minSdk 26 and cheap old tablets, and that inverts
the picture. The devices this is *for* — a Hoozo-class MediaTek tablet, a
Snapdragon 400/600-series phone, anything Android 8 to 11 — are overwhelmingly
32-bit-capable and most ship a 32-bit userspace, because their stock ROM had to.

The dev device sits between the two. **OnePlus Nord N30 5G, Snapdragon 695
(SM6375), 2×Cortex-A78 + 6×Cortex-A55.** Both cores implement AArch32 at EL0, so
the silicon can. Whether LineageOS 23 (Android 16 QPR0) ships a 32-bit userspace
on it is a separate question with a local answer.

### Measured, 2026-08-15, on the device itself

It does, and the CPU obliges. `CPH2513`, platform `holi`, kernel 5.4.302:

```
ro.product.cpu.abilist    arm64-v8a,armeabi-v7a,armeabi
ro.product.cpu.abilist32  armeabi-v7a,armeabi
ro.zygote                 zygote64_32
/system/bin/linker  ->  /apex/com.android.runtime/bin/linker      (both present)
/system/lib/libc.so                                               (32-bit, present)
```

`zygote64_32` is the one that settles the ROM question — the platform starts a
32-bit zygote as well as a 64-bit one, so 32-bit userspace is not vestigial
here, it is live.

**And the properties were not taken on trust.** A real ARM32 binary, built with
the NDK at API 26 and run over adb:

```
$ adb shell /data/local/tmp/arm32dyn
ARM32 EXECUTED. sizeof(void*)=4 machine=armv8l kernel=5.4.302-qgki-gc704f110e1f9
```

`armv8l` is what `uname` reports to a process running in AArch32 on ARMv8
silicon, and a four-byte pointer is not something a 64-bit process can report.
**This device executes AArch32 at EL0.**

A GOTCHA WORTH THE THREE LINES. The same program built `-static` does not run:

```
error: "...": executable's TLS segment is underaligned:
       alignment is 8 (skew 0), needs to be at least 32 for ARM Bionic
```

That is an NDK static-linking rule, not a statement about ARM32 — bionic wants
a 32-byte-aligned TLS segment and the static startup files do not provide one.
Build ARM32 test binaries dynamically or the measurement looks like a failure
it is not.

### What it does and does not change

It does not change the verdict below. The blockers on the armeabi-v7a path were
never that the CPU would refuse — they are that dynarmic has no 32-bit host
backend, that `cpu.cpp` wants a 4 GiB reservation a 32-bit process has not got,
and that **exec from app-writable files is denied** no matter what the CPU can
do. Firmware arrives at runtime and can never live in the APK.

What it does change is that this device can host the *other* experiment if it is
ever worth running: an armeabi-v7a process is itself in AArch32, so the guest's
code could in principle be mapped `PROT_EXEC` and branched to with no
translation at all — not exec'd, so the SELinux rule above does not apply. That
is a new execution engine to write, and dynarmic on arm64 already works, so it
is an optimisation and not a route. Noted because on a **32-bit-only** tablet it
stops being an optimisation and becomes the only path: arm64-v8a will not
install there, and without it there is no JIT.

The property and the file are not the same question, which is why the probe
checks both. A ROM can advertise an ABI whose loader is absent.

**The Android emulator cannot answer this.** Tried, and it is a hard no:

```
FATAL | QEMU2 emulator does not support arm64 CPU architecture
```

The SDK emulator dropped ARM guests on x86 hosts. There is no armeabi-v7a system
image past **API 25** either — `system-images;android-26;*` exists only for
arm64-v8a. So ARM testing on this machine means real hardware, full stop, and
that is a workflow fact rather than a detail.

---

## 3. The three options, and the one that is actually available

### Option A — run the guest ARM32 natively, in an `armeabi-v7a` app process

Superficially the big prize: the app process is ARM32, the guest is ARM32, no
instruction emulation at all.

It does not survive contact.

**A1. It cannot use Glasspole, and Glasspole is the engine.** Glasspole is glue
around **dynarmic**, and dynarmic's own documentation says there are *no plans
to support any 32-bit host architecture*. There is no dynarmic backend that runs
on armeabi-v7a and there is not going to be.

**A2. The address-space assumption is structural.** `glasspole/src/cpu.cpp`:

```c
constexpr uint64_t GUEST_SPACE = 0x100000000ull;  /* the whole 32-bit range */
```

The guest's entire 4 GiB is reserved up front so a guest address is `base +
vaddr` with no lookup — which is also what dynarmic's fastmem wants. A 32-bit
host process has under 4 GiB of address space *in total*. This is not a constant
to tune; it is the reason the memory model has no page table.

**A3. Native execution still needs a loader, and the syscalls go to the wrong
place.** The guest binaries are uClibc ELFs naming `/lib/ld-uClibc.so.0` as their
interpreter. bionic's linker will not load them, so you write your own loader —
and then every guest syscall goes straight to the Android kernel, bypassing
`glasspole/src/syscall.cpp`, which is where the sysroot path rewriting, the
synthesised `/proc`, the device nodes and the shim's whole world live. You would
be reimplementing that with seccomp-BPF and SIGSYS. That is not less work than a
JIT; it is different work with worse tools.

**A4. And you may not exec what the user supplied.** Measured:

```
probe: exec from app files   DENIED (execve refused — SELinux/noexec)
probe: exec from APK lib     OK — a packaged binary CAN be executed
```

An app cannot execute a file it wrote. That is `untrusted_app` losing exec on
`app_data_file` in Android 10, and it applies regardless of target API. The
guest binaries come out of the user's own device firmware at runtime — they can
never be in the APK. So even the loader-free version of this idea is dead.

(A2 and A4 have a shared silver lining that matters later: a JIT never execs
anything and never maps a user file executable. It *reads* the ELF and generates
its own code. Both restrictions miss it completely.)

**Verdict: not viable.** Not "hard" — the engine cannot be built for the ABI.

### Option B — Glasspole on arm64-v8a

**This turns out to be nearly free, and it is the finding that changes the
shape of the whole port.**

Glasspole is not a from-scratch instruction emitter. `cpu.cpp` opens by saying
so — "dynarmic does the work; what lives here is the memory view it reads
through, the SVC hook, and thread creation". And dynarmic **already has an
A32-on-AArch64 backend**, written for yuzu's Android port, present in the exact
commit `glasspole/fetch-deps.sh` pins:

```
deps/dynarmic/src/dynarmic/backend/arm64/
    a32_address_space.cpp  a32_core.h  a32_interface.cpp
    emit_arm64_a32.cpp     emit_arm64_a32_memory.cpp  ...
deps/dynarmic/externals/oaknut/     # the AArch64 assembler it emits through
```

It is not merely present — **it builds, here, today**:

```
$ ./android/glasspole/build-glasspole.sh dynarmic
-- Target architecture: arm64
[126/126] Linking CXX static library deps/dynarmic/src/dynarmic/libdynarmic.a

$ llvm-readelf -h emit_arm64_a32.cpp.o
  Class:    ELF64
  Machine:  AArch64
arm64 backend objects: 16
```

89 MB of AArch64 static library, containing `emit_arm64_a32.cpp.o` — the
A32-to-AArch64 code emitter — compiled by the NDK at API 26. The only thing that
had to be supplied was Boost's headers, which an NDK sysroot has none of and
which are header-only for what dynarmic uses.

So "port Glasspole's JIT to emit AArch64" — which the brief listed as one of the
three options and which sounds like months — **is not work that needs doing.**
It was done upstream, for the same reason (an Android port), and Tadpole is
already pinned to a dynarmic that contains it.

What is left is real, ordinary, and now enumerated rather than guessed at.
Building the full `glasspole` target stops with exactly three errors, all in
Glasspole's own sources and none in dynarmic:

```
src/signal.cpp:46: error: expected unqualified-id
    constexpr uint32_t SA_SIGINFO = 0x00000004;
    note: expanded from macro 'SA_SIGINFO'   [NDK asm-generic/signal-defs.h]
    ... and SA_RESTORER, SA_NODEFER, SA_RESETHAND

src/cpu.cpp:81: error: no member named 'atomic_ref' in namespace 'std'
    std::atomic_ref<T> ref(*reinterpret_cast<T *>(m->Ptr(addr)));
```

Four constants that are macros in bionic's uapi headers and are not in glibc's,
and one C++20 feature the NDK's libc++ has not got — which is the guest's
ldrex/strex compare-exchange, so it needs `__atomic_compare_exchange_n` rather
than deleting. Both are one-line changes. They are written down instead of made
because `glasspole/src/` is shared with `main`; see
`android/glasspole/build-glasspole.sh` for the exact diagnostics.

Beyond those:

- `host_posix.c` (408 lines) against bionic. It is already written to a
  deliberately narrow interface — `host.h` says the interface is "shaped to what
  WIN32 can do", which is a much bigger crossing than Linux-to-Android.
- The 4 GiB reservation. `MAP_NORESERVE` on a 64-bit host is fine.
- `signal.cpp`, and dynarmic's fastmem, which uses a SIGSEGV handler. Android
  allows that, but it fights with the platform's own handler for tombstones.
- No exec anywhere: the guest ELF is read, not run. A4 does not apply.
- And the JIT needs executable memory, which was measured and works both ways:

```
probe: anon RWX mmap         OK — W^X is NOT enforced on anonymous memory here
probe: anon RW->RX mprotect  OK (Glasspole's code cache can live here)
probe: memfd_create          OK; RW map OK, RX map OK
```

**On W^X and minSdk 26, since it was asked specifically:** it buys nothing,
because there is nothing to buy. The Android 10 restriction that gets quoted is
about *executing files* from app-writable storage — which is the A4 result above
— not about anonymous executable memory. `mmap(PROT_EXEC|PROT_WRITE)` anonymous
and `mprotect(RW→RX)` both work, and they have to: ART's own JIT does exactly
this on every device. If a hardened ROM ever refuses, the memfd dual mapping is
the fallback and it was measured working too. This is not a reason to lower
minSdk and it is not a risk to the port.

### Option C — build `qemu-arm` for Android

Possible, and the fallback if B stalls. qemu-user loads the guest ELF itself, so
A4 misses it as well; it would ship in the APK's lib directory as
`libqemu-arm.so`, which is measured to be executable. But it is a second engine
to maintain, it is the one Tadpole moved *away* from, and it is slower. Worth
keeping as the reference for differential testing — which is exactly its role on
the desktop — and not as the thing users run.

---

## Verdict

**Build `arm64-v8a` and make Glasspole the engine.** Not armeabi-v7a.

That is the opposite of the ABI brief this work was given, and the reason is
specific rather than a preference: the ARM32-native idea's appeal was that it
removed the emulation work, and it turns out the emulation work was already
removed — by dynarmic having an arm64 backend Tadpole is already pinned to.
Meanwhile armeabi-v7a cannot host dynarmic at all, so choosing it does not
simplify the port, it deletes the engine.

The 32-bit ABI keeps one job. `armeabi-v7a` builds cleanly today — SDL2, the
viewer and the shim all produce v7a binaries in this tree — and on an old
32-bit-only tablet it is the only ABI that will install. But what would run
there is the **software rasteriser front end with no ARM engine behind it**,
which is what the screenshot in the previous commit shows: the viewer, running,
saying "No ARM engine installed". That is worth shipping as the ABI's honest
limit, not as a second engine.

**The realistic path, shortest honest version:** arm64-v8a APK, SDL2 (done),
viewer (done), Glasspole built for arm64 with dynarmic's existing backend
(configures; build in progress), `host_posix.c` adjusted for bionic, the shim
arena moved off `/tmp` (see NOTES-shim.md), and the HLE's second window folded
into the single Android surface. None of those is research. The research
finished when `-- Target architecture: arm64` printed.
