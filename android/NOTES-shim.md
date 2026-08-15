# Does the shim design survive Android?

Mostly, and by a wider margin than expected — but one piece of it does not
survive at all, and it is not the piece the brief predicted.

Everything below marked *measured* came out of `android/app/jni/tadpole_probe.c`
running on an API 35 image. Run it on any device with
`android/build-apk.sh run` and read `adb logcat -s tadpole | grep probe`.

---

## What the shim actually is

Two mechanisms, and they have very different fates.

1. **Replacing the guest's shared libraries.** `tadpole/shim/` builds ARM32
   `.so`s that stand in for the guest's libz, libasound, libEGL and
   libGLESv1_CM, and they are `LD_PRELOAD`ed into the guest —
   `tadpole_shim.c`'s own header says so, and `tadpole.sh` sets
   `LD_LIBRARY_PATH` for the rest.

2. **A shared arena plus named FIFOs.** The shim maps `$TADPOLE_DIR/fb0.bin`
   and `state.bin` `MAP_SHARED` and the viewer maps the same files; audio and
   input travel over `mkfifo`'d nodes in the same directory. Default
   `/tmp/tadpole`, overridable by `TADPOLE_DIR`.

---

## The arena and the FIFOs: they survive, and they need one change

This was expected to be the hard part. It is not.

```
probe: /tmp                  exists, writable=NO
probe: mkfifo in app dir     OK, round trip OK
probe: MAP_SHARED file       OK (the framebuffer arena's mechanism)
probe: memfd_create          OK; RW map OK, RX map OK
probe: fork()                OK
```

**`mkfifo` works.** In the app's own private directory, an app may create a
named pipe and both ends work. Nothing about the FIFO design has to be
redesigned — `mkfifo` is in bionic, SELinux permits it on `app_data_file`, and a
round trip through one succeeded. The Windows port had to replace FIFOs with
named pipes (`tp_fifo_fd` in `tadpole_view.c`); Android needs no such thing.

**`MAP_SHARED` on a file works**, so the framebuffer arena is unchanged too.

**`fork()` works**, which matters more than it looks: `gp_fork()` exists because
VideoDaemon daemonises, and the whole guest-supervision model in
`tadpole_view.c` is fork/exec/waitpid/kill. All of it is in bionic and all of it
linked.

**The one change is the path, and the viewer already has the hook.** `/tmp` on
this ROM is `drwxrwx--x shell shell` — it exists only because `adb shell`
created it, it is owned by the shell user, and an app cannot write to it. On
another ROM it may not exist at all. Neither case is usable, so `TADPOLE_DIR`
has to point at `getFilesDir()`.

That variable already exists, and it exists because **Windows needed it** — the
`LOCALAPPDATA` block in `tadpole_view.c`. So the port needed no viewer change,
only a way to set an environment variable before `main`, which is
`nativeSetenv` in `android/app/jni/tadpole_jni.c`. Proven end to end:

```
I/tadpole: setenv TADPOLE_DIR=/data/user/0/org.tadpole.view/files
I/tadpole: tadpole-view: 480x272, scale 2x, dir /data/user/0/org.tadpole.view/files
```

**Scoped storage is not a problem here** and it is worth saying why, because it
is the thing everyone raises. Scoped storage governs *shared* external storage —
`/sdcard`, the MediaStore. Nothing Tadpole does at runtime belongs there. The
arena, the FIFOs, the sysroot and the games all live in the app's private
directory, which scoped storage does not touch. Where it *will* be felt is the
setup wizard: importing the user's firmware and cartridge backups means a
document picker rather than a path, and `tools/online-update.sh` — a shell
script calling Python — has no Android equivalent at all.

---

## `LD_PRELOAD`: the answer depends entirely on which engine, and it is not close

### Under Glasspole — completely unaffected

This is the part worth being clear about, because it looks like the scariest
dependency and it is not a dependency on Android at all.

Under Glasspole the guest is **not an Android process**. `glasspole/src/elf.c`
loads the guest's ELFs into a 4 GiB region Glasspole reserved inside its own
address space, and `syscall.cpp` answers the guest's syscalls. The guest's
dynamic loader is the guest's own `ld-uClibc.so.0`, running as emulated ARM32
code, and `LD_PRELOAD` is a string in an emulated environment block that an
emulated loader reads. Android's linker never sees any of it. bionic is not
involved. SELinux is not involved, because nothing is `exec`ed and no file is
ever mapped executable.

The shim's ARM32 `.so`s stay ARM32 `.so`s, built with the same clang, loaded the
same way, doing the same job. **Nothing in `tadpole/shim/` needs to change.**

That is the same reason the SELinux exec rule cannot bite:

```
probe: exec from app files   DENIED (execve refused — SELinux/noexec)
probe: exec from APK lib     OK — a packaged binary CAN be executed
```

An app may not execute a file it wrote — which is fatal for any plan to run the
user's firmware binaries directly, since firmware arrives at runtime and can
never be inside the APK. A JIT never execs anything. It reads the ELF as data
and emits its own code into anonymous memory, which is measured to work.

### Under a native-ARM32 design — it does not survive

If instead the guest ran natively in an `armeabi-v7a` process, `LD_PRELOAD`
would have to be honoured by *bionic's* linker, loading uClibc-linked ELFs that
name `/lib/ld-uClibc.so.0` as their interpreter. bionic will not load those, so
you write your own loader — and the interposition, the sysroot path rewriting
and everything else in `syscall.cpp` has to be rebuilt on seccomp-BPF and
SIGSYS, because the guest's syscalls now go straight to the Android kernel.

Combined with `exec from app files DENIED`, that design does not work. See
NOTES-arm32.md, which reaches the same conclusion from the other direction.

---

## What genuinely has to be redesigned

A short list, and none of it is the shim's IPC.

1. **The HLE's second window.** Measured, first run:

   ```
   I/tadpole: hle: no GL window: Android only supports one window
   ```

   The GPU replay path opens a second `SDL_Window` for the guest's GL beside the
   one carrying the chrome. Android has one window per activity. The surface has
   to become a second GL context or an FBO composited into the single window.
   Not optional — it is the difference between 11 fps and ~57.

2. **Everything that shells out.** `tadpole_view.c` `fork`/`execv`s helper
   *scripts*: `tools/install-didj.sh`, `tools/micromods.py`,
   `tools/online-update.sh`. There is no shell and no Python on a stock Android
   device, and `exec from app files DENIED` means they could not be run even if
   there were. Every one of those is a feature of the front end that has to be
   reimplemented in C or dropped. This is a larger surface than the emulation
   work and it is easy to overlook because it all compiles and links fine.

3. **Process lifetime.** `guest_sweep_stragglers` walks `/proc` and kills by
   process group; `kill(-pgid)` assumes a session the viewer owns. Android kills
   and restarts the *activity* on rotation and backgrounding, and it may kill the
   process at any time. The arena must survive that, or a rotation is a crash.

4. **The boot decoder.** `tadpole_boot.c` compiles to its no-decoder path here —
   there is no ogg/theora/vorbis for Android in this tree yet. Three more
   cross-builds, unblocked, not started.

5. **stdout.** Solved, in `tadpole_jni.c`, and listed because it would otherwise
   have been discovered the hard way: an Android app's stdout goes to
   `/dev/null`, and Tadpole's entire debugging story is `printf`.

   A bonus fell out of it that is genuinely useful for the guest work. The
   redirect is a `dup2` over fds 1 and 2, so **child processes inherit it** —
   when the packaged exec probe ran, its own `printf` came back through logcat
   under the same tag:

   ```
   I/tadpole: tadpole exec probe: running as pid 4983 from .../lib/arm64/libtadpoleexec.so, 64-bit
   ```

   Whatever engine ends up running the guest, its output lands in the same
   `adb logcat -s tadpole` stream the viewer's does, with no extra plumbing.
