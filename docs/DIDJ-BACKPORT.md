# Running a LeapsterExplorer title on a Didj

A tutorial for the LFHacks backporters, written from the first title that
worked: Ni Hao, Kai-lan (`LST3-0x00180002`), a 2010 LeapsterExplorer game,
plays on the Didj's own 2009 firmware — in Tadpole first, and the same files
are what a real Didj needs. It was never a rebuild of anything. It is one
library and two lines of metadata, and this page says what they are, how to
apply them, how to try the next title, and how to read the failure when it
does not work.

Everything here was measured on the emulator with the stock Didj firmware
(1.35.2.4222). Nothing in that firmware is changed by any step below;
removing the title's directory and the library puts the device back to
stock. **Hardware testing is the community's, not Tadpole's** — see "On a
real Didj" for the one thing that was built specifically so it can be tried
there, and for what nobody has verified yet.

## Why an Explorer title refuses, in two steps

Drop an Explorer package at `/Didj/ProgramFiles/<3LD>/` and the Didj's shell
dies at boot, before it draws a menu:

    !ASSERT: [0] CTexturePNG::LoadPNG() unexpected problem reading: /Didj/ProgramFiles/NIH/icon.swf

Explorer packages carry a Flash icon (`Icon="icon.swf"` in `meta.inf`). The
Didj's shell reads every installed title's icon as a PNG while it builds the
game ring, and asserts on the first that is not one. On a real Didj that is
the "brick": the shell asserts on every boot until the package is removed.
On the emulator it is a log line and a restart.

Point `Icon=` at a PNG and the title is listed, previewed and selectable.
Launching it then fails one step later:

    !BRIO WARNING: unable to open module '/Didj/ProgramFiles/NIH/App.so', File not found
    !ASSERT: [0] CGamesManager::PushApp(...), Could not dlopen the application so

uClibc's dlopen says "File not found" for a dependency it cannot resolve, and
`App.so` is right there. Two things are unresolved: one library it names
(`libCartridgeMPI.so`, which it then imports nothing from) and sixteen
symbols the Didj's Brio and Lightning framework never had. The full
measurement — every symbol the title imports minus every symbol the Didj
exports, with a native Didj title as the control — is in [DIDJ.md](DIDJ.md),
"what an Explorer title is missing". The short version: 217 of 236 imports
resolve against the Didj's own libraries by exact C++ signature. The title
is closer to the Didj than its name suggests.

## The compatibility library

`libCartridgeMPI.so` — built from `tadpole/shim/tadpole_didj_backport.c`,
shipped in `runtime/didj-backport/`, installed to `/Didj/Base/Brio/lib/`.

It is not a cartridge library. `App.so` names that file in its dependencies
and takes no symbol from it, so the loader only needs a file by that name to
exist; give it this one and the loader brings it into the process, at which
point its sixteen exports resolve everything else. Each stub does the
least-wrong thing, and wherever possible builds a real Didj object by
calling the Didj's own code — read out of the Leapster GS's implementations
and matched to what the Didj exports:

| what the title asks for | what it gets on the Didj |
|---|---|
| `tPackageType`, `tUploadDataType` constructors | 16-byte strings, byte for byte what the GS does |
| `CGameAreaStart`, `CGameAreaExit`, `CAchievementEarned` log events | the Didj's own `CGameStart`, `CGameExit`, `CLevelUp`, so its log file gets a real record |
| `CMicroDownloads()` | the Didj's `CMicroDownloads(path)` with the path its own `GetMDLsPath()` gives |
| `CSystemData::GetBaseTutorialPath()` | the Didj's `GetBasePath()` — a directory that exists |
| `CMilestones` | a singleton with no milestones: `Add` does nothing, `GetMatch` is an empty vector |
| `CPlayerProfile::AddBadge` | no badges on a Didj; reports false |
| `CTouchEventQueue` | a real Brio event listener built through the Didj's own base constructor, registered for no event types, whose queue is always empty. The Didj has no touchscreen; Kai-lan constructs this and does not need it to play |

So: badges, milestones and micro-downloads silently do nothing; the two
newer analytics events are logged under older names; touch never arrives.
Everything else is the Didj's own code running the title.

## In the emulator

You need the Didj installed and live (`./tools/online-update.sh` with the
Didj chosen, or `./tadpole.sh --device didj` if it already is), and the
title as it ships: a `.tar` or `.zip` with `App.so` and `meta.inf` at the
top, or a directory holding the same.

    ./tools/didj-backport.py install "Ni Hao, Kai-lan (USA).tar"

which prints what it did:

    Ni Hao, Kai-lan
      Device=LeapsterExplorer  PackageID=LST3-0x00180002-000000
      1111 files -> runtime/sysroot/Didj/ProgramFiles/NIH
      meta.inf Icon: icon.swf -> icon64.png  (the package's own 64x64 icon)
      meta.inf PreviewImage: None -> preview.png  (must be a PNG that exists)
      runtime/sysroot/Didj/Base/Brio/lib/libCartridgeMPI.so installed
      check: 232 imports, 216 resolved by the Didj, 16 by libCartridgeMPI.so, 0 still missing
      -> will load. Whether it runs is the experiment.

Three things happened. The title went to `/Didj/ProgramFiles/<3LD>/`, the
three-letter ID from its own `meta.inf`, which is how the Didj lays out its
own titles. Two `meta.inf` lines were rewritten to name PNGs — the original
is kept beside it as `meta.inf.orig` — with a 64x64 icon taken from the
package when it has one (the Didj's own icons are 64x64) and resampled from
its largest art when it does not. And the library was put where the loader
will find it. Then `check` ran, which is the measurement above applied to
this title.

Then:

    ./tadpole.sh --device didj

The title is on the game ring (the shell also remembers the last title it
launched and goes straight back into it at boot). Kai-lan reaches its title
screen, its Play menu, and the overworld, at the panel's 60 fps on host-GPU
replay. That is as far as anyone has driven it.

## On a real Didj

The library was built for the Didj's own processor, not for the emulator's.
Every other guest shim in Tadpole is ARMv7 with VFP, which is what qemu-arm
and the later devices run; the Didj's LF1000 is an ARM926EJ-S — ARMv5TE, no
floating-point unit — and a v7 build would fault on the first instruction it
does not know. `libCartridgeMPI.so` is built with `-march=armv5te
-mfloat-abi=soft` against the same uClibc 0.9.29 the Didj runs; `readelf -A`
says `Tag_CPU_arch: v5TE`, and the disassembly has no v7 or VFP instruction
in it. It is the same file the emulator uses.

To build a tree to copy over rather than installing into the emulator:

    ./tools/didj-backport.py install "Ni Hao, Kai-lan (USA).tar" --out /path/to/staging

    /path/to/staging/Didj/ProgramFiles/NIH/...            the title, meta.inf fixed
    /path/to/staging/Didj/Base/Brio/lib/libCartridgeMPI.so  the library

Copy `Didj/` onto the device's Didj partition with the paths kept, by
whatever route you already use to put files on a Didj. Only the library, not
the emulator's other shims: `/Didj/Base/Brio/lib/` is on the device's own
`LD_LIBRARY_PATH` (`/etc/profile` puts it there), and nothing else in this
directory belongs on hardware.

**What has NOT been verified on hardware, honestly:**

- That the shell accepts the rewritten `meta.inf` the same way. It should:
  the emulator runs the stock shell binary against the stock firmware.
- That LeapFrog's own `libopengles_lite.so` renders the title. The emulator
  replaces that one library with its own GLES implementation (the stock one
  maps the LF1000's 3D registers and has no hardware to map here). Kai-lan
  asks that library for 35 entry points, all of which the stock library
  exports, but nobody has watched the LF1000's 3D engine draw it.
- Speed and memory. The emulator does not model either.

If it bricks the shell, the way back is the way in: delete
`/Didj/ProgramFiles/<3LD>/` (and, if you like, the library) and the device is
stock. Keep a way to reach the filesystem that does not depend on the shell
booting before you try.

## Trying the next title

    ./tools/didj-backport.py check "Some Other Title.tar"

runs the measurement without installing anything:

    Some Other Title.tar: 240 imports, 219 resolved by the Didj, 16 by libCartridgeMPI.so, 5 still missing
        LeapFrog::Brio::CCameraMPI::CCameraMPI()
        ...

Zero still missing means it will load, and only then does the interesting
part start. A list means a bigger compatibility library: each line is a
symbol to add to `tadpole/shim/tadpole_didj_backport.c`, and the way to
decide what it should do is the way the sixteen were decided — disassemble
the Leapster GS's implementation (`llvm-objdump -d --triple=armv7-linux-gnueabi
--disassemble-symbols=<mangled name>` on the library `check` names), see
what it constructs, and build the same thing out of what the Didj exports.
The file's header comment walks through each existing stub for exactly that
reason. Then `cd tadpole && make didj-backport` rebuilds and reinstalls it.

Some things are not a missing symbol and no library fixes: a title that
needs the touchscreen for play, the camera, a microphone, or Explorer-only
hardware paths. `check --installed` audits everything currently in
`ProgramFiles`, which is how to tell whether a change to the library helped
the whole set.

## Reading a failure

Run with `--debug` and read `[tadpole]` and `!ASSERT` lines:

    ./tadpole.sh --device didj --debug

- `CTexturePNG::LoadPNG() unexpected problem reading:` — the icon fix did not
  take. Check `meta.inf` names a PNG that exists in the title directory.
- `unable to open module ... File not found` — the loader. Run `check`; the
  list it prints is what is missing.
- A crash report naming `libCartridgeMPI.so+0x...` — one of the stubs. The
  offset resolves with `tools/elfsyms.py` against
  `runtime/didj-backport/libCartridgeMPI.so`; the stub is doing less than the
  title needed.
- A crash report naming the title's `App.so` or a Didj library — the title
  ran and hit something the Didj's Brio does differently. That is the real
  backport work, and the emulator will let you take it one function at a
  time.
