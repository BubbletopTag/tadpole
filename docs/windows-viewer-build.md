# Building and running the viewer on Windows

_Recorded 2026-08-09 from the session that did it, on the OptiPlex 3020. What
worked, verbatim, plus everything that had to be done the Windows way. See
`docs/windows-gl-probe.md` for this machine's GL capabilities and
`docs/windows-viewer.md` for the brief this answers._

## Build steps that worked

From an MSYS2 MINGW64 shell (`C:\msys64`), packages once:

```
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-pkg-config mingw-w64-x86_64-SDL2
```

Then:

```
cd tadpole/viewer
cmake -S . -B build -G Ninja
ninja -C build
./build/tadpole-view.exe
```

`viewer/CMakeLists.txt` is the Windows build; the GNU Makefile remains the
Linux one. The .exe is fully static — static SDL2, static zlib, static
runtime — and imports only Windows system DLLs: copy the one file.

`--ui-shot wiz0..wiz4 out.png` renders the wizard pages headlessly, exactly as
on Linux.

## What had to be the Windows way

* **Console subsystem, for now.** sdl2.pc injects `-mwindows` (GUI subsystem,
  stdout discarded); the build forces `-mconsole` after it so bring-up
  diagnostics land somewhere. A release build should flip this — and decide
  where logs go instead, because `%LOCALAPPDATA%\Tadpole\state` already
  exists for exactly that.
* **The GL ring is plain `calloc` memory.** There is no guest process, so
  there is nothing to share the ring file with. When the emulator arrives it
  becomes the one-process design's in-memory ring — NOT a `CreateFileMapping`
  port of the file, which both briefs explicitly fence off.
* **Guest supervision is stubbed.** `spawn_script` fails immediately with
  "no emulator in this Windows build yet" on stderr and the status line;
  fork/waitpid/kill remain POSIX-only code the Windows build never compiles.
* **Paths.** `TADPOLE_DIR` defaults to `%LOCALAPPDATA%\Tadpole\run`; config
  and the games cache live under `%LOCALAPPDATA%\Tadpole`. This is done by
  inserting `LOCALAPPDATA`/`USERPROFILE` into the existing XDG fallback
  chains — environment-driven, no `#ifdef`, correct on both platforms.

## What the window shows today, honestly

The full UI works: menus, fonts, the wizard, `--ui-shot`, correct greying of
actions that need a guest. The wizard's state detection is truthful on
Windows — firmware/sysroot/games all report missing on a fresh box, and every
page re-tests real state.

Two rough edges a new Windows user will meet, both messaging rather than
mechanism, left for a deliberate fix rather than patched in passing:

1. **The first status line reads like an error.** The automatic update check
   spawns a script, the spawn stub refuses, and the corner says "Checking for
   updates could not start". True, but it looks like something broke. The
   honest framing would be "updates and emulation need the full build" or
   simply skipping the check when spawning is known-unavailable.
2. **wiz0 gives Linux advice.** The dependency probe reports "qemu-arm is
   missing — run ./tools/fetch-deps.sh". On Windows the eventual answer is
   glasspole.exe, and the wizard's platform awareness should arrive together
   with the launch path, not before it.

The window opens on the physical display (the machine also carries a phantom
`IddSampleDriver` adapter — see `docs/windows-gl-probe.md` — which caused no
trouble here, but keep it in mind if a window ever opens nowhere visible).

And once more for the record, because it is the sentence the whole GL plan
rested on, printed by the shipping viewer on this machine:

```
tadpole-view: HLE replay on Intel(R) HD Graphics 4600 (4.3.0 - Build 20.19.15.4835)
```
