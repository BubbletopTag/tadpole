# Tadpole on Windows — brief 4: a native viewer, GUI first

_Written 2026-08-09 on the Linux box. The goal here is deliberately narrow: a
real Tadpole window, drawing the real UI, on Windows — even with no emulator
behind it and every button that would start a guest greyed out._

## Why this is worth doing before the emulator works

It is the part a person can look at, it is independent of everything else, and
it is where old drivers and unfamiliar path conventions will embarrass us. It
also has almost no unknowns left: the OpenGL question — the one thing that could
have sunk it — was answered on this box, and the loader work is already done and
verified on both platforms.

## The surface, measured rather than guessed

The viewer is 8,740 lines across `tadpole/viewer/`. Counting the calls that
actually need attention:

| file | lines | what needs work |
|---|---|---|
| `tadpole_ui.c` | 3,163 | `access`, `getenv`, `opendir`/`readdir`, `mkdir`, `stat` — **all of these mingw-w64 already provides.** Expect this file to need close to nothing. |
| `tadpole_hle.c` | 1,726 | one `mmap` plus `ftruncate` — the GL ring. With no guest, this can be plain memory. |
| `tadpole_view.c` | 2,709 | the real work: `fork`/`execv`/`waitpid`/`kill`/`dup2`, which start and supervise the guest. |

So the job is smaller than the line count suggests. The UI is portable almost by
accident, the GL layer is one mapping, and the process supervision is the only
genuinely Unix-shaped thing in there — and for a GUI-only build it does not need
to work at all, only to compile and report honestly that it is unavailable.

## Order of work

1. **Get it compiling.** Add a `CMakeLists.txt` for the viewer rather than
   fighting the GNU Makefile under MSYS2 — it already has to work for
   `glasspole`, and the two can share a toolchain file later. SDL2 comes from
   `mingw-w64-x86_64-SDL2`.
2. **The one `mmap` in `tadpole_hle.c`.** With no guest there is nobody to share
   with, so a plain allocation is correct and honest. Do not reach for
   `CreateFileMapping` yet; see the note on one-process below.
3. **Stub the guest-spawning path in `tadpole_view.c`.** Everything that
   `fork`s, `exec`s, `waitpid`s or `kill`s. It should compile, and at runtime
   say plainly that running a guest is not available in this build. Do not
   emulate `fork`.
4. **Paths.** `/tmp/tadpole` is the default `TADPOLE_DIR` and is wrong on
   Windows. Use `%LOCALAPPDATA%\Tadpole`, and check what `tadpole_ui.c` assumes
   when it browses for games and firmware.
5. **Run it and send back a screenshot.** The setup wizard is the interesting
   page: it re-tests real state on every page rather than remembering that it
   ran, so it doubles as a diagnosis of what the Windows build can and cannot
   see. `--ui-shot wiz0..wiz4` renders the pages for inspection.

## Constraints

* **Keep it building on Linux.** Same rule as the GL loader, and it is checked
  here. No `#ifdef _WIN32` where a portable call exists.
* **Do not start on the one-process merge.** Making the viewer and emulator
  threads of one process is the right end state and it is what removes the need
  for shared file mappings and FIFOs on Windows — but it is a design change
  affecting both sides, and it should not be smuggled in through a viewer port.
  Flag anything that seems to need it and leave it.
* **No `mmap` emulation layer.** If a shared mapping seems necessary, that is
  the one-process question wearing a disguise. Report it.

## What "done" looks like

A `.exe` that opens a window on the OptiPlex, draws the real Tadpole UI with its
fonts and layout, navigates the wizard, and says clearly that there is no
emulator behind it yet. Nothing more.
