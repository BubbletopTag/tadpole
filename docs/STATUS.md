# Tadpole — status

_Updated 2026-08-08. Supersedes the 2026-07-27 version entirely._

## What this file is for

`HANDOVER.md` is the engineering log. It is append-only on purpose — findings
go on the end, nothing is deleted, and an entry that was true in July is still
sitting there in August next to the one that overturned it. That makes it the
right place to answer *why is it like this* and the wrong place to answer
*where are we*.

This file answers the second question only, and it is the one document here
that is **expected to go stale**. If it disagrees with a later HANDOVER entry,
HANDOVER wins; then fix this file.

The version it replaces had said "VideoDaemon is the blocker" for twelve days
after that stopped being true, and a stale status file is worse than none — it
sends the next session at a bug that was fixed a week ago.

## Where we are

Tadpole boots the stock system software to its home screen, signs a profile in,
and launches titles — Flash and native Brio, 2D and 3D, with audio, touch,
video and host-GPU rendering.

| | state |
|---|---|
| Stock firmware obtained, rootfs extracted and verified | done |
| ARM binaries run under qemu-user | done |
| Framebuffer, input, audio, filesystem shim | done |
| `AppManager` reaches its main loop and draws the UI | done |
| Profile creation, sign-in, home screen | done |
| Flash titles | done |
| Native Brio titles (2D) | done |
| 3D — geometry, textures, perspective, the skinned player character | done |
| Host-GPU replay (HLE) | done — ~57 fps against 11.5 software |
| FMV / the video plane | done |
| Launching one title directly, with no home screen | done — `TADPOLE_LAUNCH` |
| Firmware install, online update, setup wizard, AppImage | done |
| 28 of 110 installed titles still fail to launch | **where the work is** |

## The old blocker is gone, and here is how to tell

The previous status said `VideoDaemon` never created its socket and that
nothing could proceed until it did. Both halves are now false.

* **The daemon runs and serves.** It binds the AF_UNIX stream socket
  `/tmp/video_events_socket` and accepts on it — check with `ss -xl | grep
  video` while a boot is up, and note that two Tadpole instances share that one
  host path, so a second boot leaves the first daemon listening on an orphaned
  inode. It was never crashing: it double-forks, and the parent's
  `exit_group(0)` was being measured as death (HANDOVER 4.7).
* **There is a four-second reproduction of the whole video path.**
  `tools/vdplay.py transition` connects to that socket and asks the running
  daemon to play a system clip. If the socket were absent the script would say
  so, in those words.
* **Video reaches the panel.** Sneak Peeks plays its trailers — fb2 is the
  MLC's YUV420 video plane and it is not always the bottom layer; see the
  2026-08-07 HANDOVER entry.

One line survives from that era and **is not evidence of a fault**:

```
[0x0] DaemonControl socket connect failed ret=-1
```

AppManager logs it in runs where VideoDaemon is up *and* in runs where it was
never started at all — every direct launch, including the whole compatibility
sweep, since `tadpole.sh` only starts the daemon in its `ui` mode. Video plays
either way. Do not reopen this on the strength of that line alone.

## Where the work actually is

`tools/compat-sweep.sh` launched all 110 installed titles on 2026-08-08 and
`tools/compat-report.py` turned the run into a page. 75 launch to a real
screen, 28 crash, 6 draw nothing, 1 draws and then stops.

**The clustering is the finding, not the list.** The 28 crashes share six fault
sites:

| | |
|---|---|
| 19 | `libuClibc+0x00059920`, SIGABRT — `locale::facet::_S_create_c_locale name not valid` |
| 3 | `App.so+0x00127c48`, SIGSEGV — one engine build, three titles |
| 3 | `App.so+0x00071340`, SIGSEGV — likewise |
| 2 | `libLightningJSON.so`, two nearby offsets — GalleryWidget, StoryGalleryWidget |
| 1 | `App.so+0x00011480` |

Nineteen titles aborting in one place with one message is one bug. "28 titles
are broken" and "six fault sites" call for completely different work, which is
the whole reason the sweep exists rather than a list of names.

**Carry the caveat with the numbers.** Every title in that run was launched
straight in, with no home screen and no player signed in. A crash means
"crashed this way", not "crashed", until it has been checked both ways — and
that matters most for the widgets, since Camera, Gallery and Keyboard are
exactly the things that would want profile data.

Next, in rough order:

1. **The locale cluster.** One `_S_create_c_locale` abort accounts for 19 of
   the 28. Nothing else on this list is worth 19 titles.
2. **Re-run the failures with a profile signed in**, so the caveat above stops
   being a caveat.
3. **The two `App.so` trios.** Identical offsets, so each trio is one bug in
   one shared engine build.
4. **The remaining GL stubs.** Still no-ops; the shim now names the first hit
   on each and writes `gl-warnings.log`, so a title says what it wanted rather
   than merely looking wrong. A stubbed *getter* is the dangerous kind — see
   the `glGetFixedv` entry in HANDOVER.

## Technique notes worth remembering

Kept from the previous version because they are still true, and each one costs
a session to rediscover.

* **`qemu-arm -s 67108864`.** The default 8 MB stack is too small: Brio and
  Flash Lite recurse deeply through their scene graphs, and both AppManager and
  saplayer faulted on `str r1, [sp]` at exactly 8 MB below the stack base. One
  flag took this project from "crashes everywhere" to "runs", and
  `runtime/run.sh` points here for the reason.
* **No `LD_PRELOAD`.** This uClibc was built without
  `__LDSO_PRELOAD_ENV_SUPPORT__`. We impersonate a library the target already
  links (`libdl.so.0`, `libz.so.1`) by patching the real one's SONAME to a
  same-length name and taking its place. This is also how a title gets launched
  directly: define `CAppManager::PushApp` and win the lookup.
* **`qemu-arm -L` cannot create files.** It only redirects paths that already
  exist, so a creating `open()`, a `rename()` to a new name, or a `mkdir()`
  falls through to the host and fails. The shim translates them itself.
* **Hooking `open()` is not enough.** uClibc's stdio reaches its own open
  through a hidden alias that never touches the PLT, so `fopen()` is invisible
  to an interposed `open`. The shim hooks `fopen`/`fopen64` too.
* **gdb works**: `qemu-arm -g <port>` plus host `gdb -ex 'set architecture arm'`
  gives real backtraces through stripped binaries via library symtabs. For a
  crash you did not catch live, `tadpole/shim/tadpole_crash.c` and
  `tools/crash-triage.py` name the library and offset after the fact.
* **C++ exceptions unwind correctly** under qemu-user, our shim and the guest's
  own unwinder — tested, so it is not the explanation for the SIGABRTs above.
