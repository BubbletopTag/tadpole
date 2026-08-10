# Releasing the Windows build

_Everything here is driven from the LINUX box, from the same commit as the
AppImage. That is deliberate: a release step that needs a second machine is a
release step that eventually gets skipped, and the two halves would drift._

## One command

```
./tools/release.sh                 # tag, build both, publish
./tools/release.sh --dry-run       # build both, publish nothing
```

It tags `tadpole-DDMMYYYY-NNNN` (the scheme `check-update.py` parses), builds
the AppImage and `Glasspole-Setup.exe`, and creates one GitHub release
carrying **both** assets with the commit message as the body.

**The post-commit automation should call this**, not upload by hand. A release
with only the AppImage tells every Windows user their update is missing —
`check-update.py` asks for the asset its own platform can run, and finding
none is reported honestly rather than silently.

## What the pieces are

* `tools/build-windows.sh` — cross-compiles the whole Windows product with
  mingw-w64: `glasspole.exe` (CMake + toolchain file, because dynarmic is a
  CMake project), `tadpole-view.exe` and `tadpole.exe`, then stages them in
  the layout the program expects. `--installer` packages it.
  Needs `mingw-w64` and `nsis` from apt; SDL2 is fetched from the official
  mingw development archive into `build/win/sdl2` (a download, not a vendored
  copy).
* `tools/glasspole.nsi` — the installer. Per-user under
  `%LOCALAPPDATA%\Programs\Glasspole`, so it never asks for administrator;
  desktop and Start-menu shortcuts, one of them `--boot` straight to the
  system menu; an uninstaller that removes **the program only** and leaves
  firmware, games and saves alone.

## What is NOT in the installer, and why

The ARM shim and the firmware. The shim is guest code that links against
LeapFrog's own libraries, and the firmware is LeapFrog's — neither is ours to
redistribute, exactly as the AppImage ships neither. The setup wizard fetches
what it needs on first run, which on Windows now works end to end (see
`docs/windows-firmware.md`).

Python is a real dependency for the game library and the game installer, and
the viewer looks for it in the places it actually lives —
`TADPOLE_PYTHON`, `build/deps/python`, `%LOCALAPPDATA%\Programs\Python\*`,
then PATH — because python.org's per-user installer does not add itself to
PATH and "not found" would be wrong for most machines that have it.

## Updating

`check-update.py` picks `Glasspole-Setup.exe` on Windows and
`Tadpole-x86_64.AppImage` elsewhere; `TADPOLE_ASSET` overrides either. On
Windows the viewer downloads the installer to `%TADPOLE_DIR%`, runs it, and
exits — the running program closing itself IS the last step of the update,
because the installer has to replace files this process holds open. On Linux
the AppImage rename-and-re-exec is unchanged.
