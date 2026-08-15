# Tadpole on Android

The state of it, honestly: **the front end works. The emulator does not run
yet.** The viewer cross-compiles, installs, launches and draws its own UI on a
phone; there is no ARM engine behind it, so it shows the setup wizard and says
so. Getting the engine there is the next piece of work and it is smaller than it
looks — see [NOTES-arm32.md](NOTES-arm32.md).

Nothing under `tadpole/viewer/` or `tadpole/shim/` was modified to get this far.
Everything new is in this directory.

---

## From nothing to a running app

```sh
android/setup-sdk.sh            # JDK, SDK, NDK, Gradle, emulator — no root
. android/env.sh                # PATH and the ABI/API choices
android/fetch-sdl.sh            # SDL2 2.32.10, built for both ARM ABIs
android/build-apk.sh all        # build, install, launch, follow the logs
```

`setup-sdk.sh` needs no `sudo` and no `pacman`; everything lands in `~/Android`
and `rm -rf ~/Android` undoes it. About 6 GB.

If no phone is attached:

```sh
emulator -avd tadpole35 -no-window -no-audio &
```

## Day to day

```sh
android/build-apk.sh build      # cross-compile only
android/build-apk.sh package    # ... and make the APK
android/build-apk.sh install    # ... and push it
android/build-apk.sh run        # ... and launch it
android/build-apk.sh logs       # follow the output
android/build-apk.sh device     # what ABIs the attached device actually has
```

### Getting the logs back, which is the part that matters

Tadpole's whole debugging story is `printf`. Every harness in `tools/` works by
grepping the viewer's output and HANDOVER's reproducers are literally
`grep -a "guest exited with status 139" run.log`.

**An Android app's stdout and stderr go to `/dev/null`** — not an unread pipe,
`/dev/null`. `android/app/jni/tadpole_jni.c` fixes that in a constructor, before
`SDL_main`: `pipe()`, `dup2` over fds 1 and 2, and a thread feeding the far end
into `__android_log_write` under the tag `tadpole`.

So this is `run.log`:

```sh
adb logcat -s tadpole                       # everything the viewer printed
adb logcat -d -s tadpole | grep -a 'hle:'   # and every existing grep works
```

`build-apk.sh logs` adds `SDL`, `DEBUG` (native tombstones — the faulting
address and a backtrace) and `AndroidRuntime` to the same stream.

The redirect is a `dup2`, so **children inherit it**. Whatever ends up running
the guest, its output arrives in the same place with no extra plumbing.

## What is here

| | |
|---|---|
| `env.sh` | where the toolchain is, which ABIs, which API |
| `setup-sdk.sh` | installs all of it, user-local |
| `fetch-sdl.sh`, `sdl/` | SDL2 source and its per-ABI build |
| `viewer/` | the viewer's Android CMake, and the three GL entry points ES spells differently |
| `app/` | manifest, the SDLActivity subclass, and the JNI: logcat redirect, `TADPOLE_DIR`, the platform probe |
| `hello/` | a minimal APK built without Gradle, and the ABI probe |
| `glasspole/` | building the ARM engine for arm64, and exactly where it stops |
| `NOTES-arm32.md` | can the guest's ARM32 code run — the answer, measured |
| `NOTES-shim.md` | what of the shim design survives Android |

## Things that will surprise you

**There is no writable `/tmp`.** On the emulator `/tmp` exists as
`drwxrwx--x shell shell`, created by `adb shell` itself; an app cannot write to
it and on another ROM it may not exist at all. `TADPOLE_DIR` moves the arena and
the FIFOs to `getFilesDir()` — a variable the viewer already had, because
Windows needed it for `LOCALAPPDATA`.

**`mkfifo` works.** In the app's private directory, with a successful round
trip. The shim's IPC needs no redesign; only its path moves.

**You may not execute a file you wrote.** Measured:

```
probe: exec from app files   DENIED (execve refused — SELinux/noexec)
probe: exec from APK lib     OK — a packaged binary CAN be executed
```

Anything to be `exec`ed must be inside the APK, named `lib*.so`. This is fatal
for running the user's firmware binaries directly and harmless to a JIT, which
never execs anything.

**The emulator cannot test the guest.** The SDK emulator refuses ARM guests on
an x86 host outright —

```
FATAL | QEMU2 emulator does not support arm64 CPU architecture
```

— and the last `armeabi-v7a` system image is API 25. The x86_64 emulator runs
the arm64 APK through ndk-translation, which is plenty for the viewer and
nothing for the engine. **ARM work needs real hardware.**

**Only one window.** The GPU replay path opens a second `SDL_Window`; Android
has one per activity, and the viewer says so and falls back to software:

```
I/tadpole: hle: no GL window: Android only supports one window
```

**No Gradle.** `build-apk.sh` drives `cmake`, `aapt2`, `javac`, `d8`, `zipalign`
and `apksigner` directly. Not on principle — the SDL template's Gradle build
works — but the native side is all CMake and the Java side is ten files, so
Gradle would only add an AGP download and a daemon that can fail for reasons
unrelated to Tadpole. `hello/build.sh` is the same idea at minimum size and is
the thing to run when you need to know whether the SDK itself is sound.
