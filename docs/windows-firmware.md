# Firmware on Windows: fetch, install, and the first real guest

_Recorded 2026-08-09 on the OptiPlex 3020, the session it first worked. The
question brief 5 asked was whether the no-device firmware path runs on
Windows. It does, end to end — and at the end of it a real LeapFrog binary
ran under glasspole.exe on Windows for the first time:_

```
$ glasspole.exe --sysroot runtime/sysroot /bin/busybox echo hello
[glasspole] elf: runtime/sysroot/bin/busybox — ... interp /lib/ld-uClibc.so.0
[glasspole] elf: runtime/sysroot/lib/ld-uClibc.so.0 — entry 60000e60 ...
hello
[glasspole] guest exited with status 0
```

The guest's own dynamic linker, from the real 4.6.0.784 firmware, resolving
the real uClibc. `busybox sh -c` and `busybox uname -a` also pass.

## The exact commands

MSYS2 MINGW64 shell, from the repo root:

```
pacman -S mingw-w64-x86_64-python mingw-w64-x86_64-python-pip unzip

# ubi_reader's compression backends need real CPython — see below.
# Install python.org CPython (3.12 used here), then:
/c/Users/admin/cpython312/python.exe -m pip install ubi_reader lzallright

python3 tools/fetch-firmware.py --probe            # 98 available, 0 missing
python3 tools/fetch-firmware.py --get all -o firmware   # 331 MB

export TADPOLE_PYTHON=/c/Users/admin/cpython312/python.exe
./tools/install-firmware.sh firmware

./glasspole/build/glasspole.exe --sysroot runtime/sysroot /bin/busybox echo hello
```

`fetch-firmware.py` needed **zero changes** — urllib against the CDN behaves
identically on Windows.

## What had to be done the Windows way

Four things, each now in the tree, none of them a fork:

1. **`ubi_reader` needs python.org CPython, not MSYS2's Python.** Its LZO
   backend `lzallright` ships Windows wheels only for standard CPython; under
   MinGW Python it tries a Rust source build and fails. (MSYS2's `python-lzo`
   package serves only the OLD ubi_reader line, whose scripts are not
   importable modules — no version works with both.) The `TADPOLE_PYTHON`
   hook in `lib-deps.sh` already existed for exactly this, so nothing in the
   scripts changed: point it at CPython and the whole chain — `lzallright`,
   `zstandard`, `cryptography` — comes down as wheels.
2. **MSYS2's unzip stops `*` at `/`.** `unzip -p pkg '*meta.inf'` matches
   nothing there (it matches fine on Linux), so package identification found
   no Firmware-Base and the install refused. The pattern is now
   `'meta.inf' '*/meta.inf'`, which both unzips read the same way.
3. **Symlinks inside the UBIFS image do not survive Windows extraction —
   silently.** `os.symlink` needs a privilege ordinary sessions lack, and
   ubi_reader swallows each failure, so the rootfs came out missing its whole
   nervous system: `/lib`'s SONAME chain, the ELF interpreter
   `/lib/ld-uClibc.so.0`, and ~60 busybox applet names. Nothing dynamic could
   load and nothing said why. `pkgtool.py ubi` now diverts symlink creation
   to a ledger and materialises each entry afterwards as a HARD link (same
   volume, no privilege, native code reads it as a plain file), copy as
   fallback, multiple passes for link-to-link chains. On Linux the real
   symlink succeeds and the ledger stays empty.
4. **The sysroot's "links" are copies on MSYS.** MSYS's default `ln -s`
   writes WSL-style reparse points (tag 0xA000001D) that native Win32 code
   cannot follow — glasspole got ENOENT straight through them — and NTFS
   symlinks need the same missing privilege. `setup-sysroot.sh` now spells
   "link" as `lns()`: symlink on Linux, remove-and-copy on MSYS. Costs
   ~100 MB of disk; keeps rootfs/ pristine and the sysroot natively readable.

## Known limits, all expected

* `busybox ls` fails: syscall 196 (`lstat64`) is not in the census-derived
  set — Cars 2 never called it — so the syscall layer reports ENOSYS by
  design. One `case` in syscall.cpp when the Linux side wants it; not added
  from here because that file is mid-surgery there.
* The 8 `.lf3` digital purchases skip without a key, with the installer's
  usual honest message.
* `linuxrc` stays dangling (its target resolution is an initramfs question
  nothing here asks).
* AppManager is NOT expected to run on Windows yet: the FIFO and
  shared-mapping gaps are real and are what the one-process merge exists to
  fix. Nothing in this session touched that.

## What this changes

The OptiPlex is no longer a build-and-screenshot box. It obtains its own
firmware from the CDN, extracts it, builds a sysroot, and runs real guest
binaries under its own emulator. What stands between it and AppManager is
the one-process design — the same thing it was before, but now with nothing
else in front of it.
