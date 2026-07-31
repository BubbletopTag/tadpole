# Tadpole — status and next steps

_Updated 2026-07-27._

## Where we are

Milestones M0–M2 are done, M4 (first pixels) is done, M3 (the shim) is ~80%.
The one thing standing between us and the system UI is a single daemon.

| | state |
|---|---|
| Stock firmware 4.6.0.784 obtained + rootfs extracted, verified | done |
| ARM binaries run under qemu-user | done |
| Device dependency manifest | done — `device-deps.md` |
| Framebuffer + input shim | done — 3 fb, 6 evdev, real names |
| Real display output (pixel-exact) | done — `shots/` |
| Sysroot verified against live hardware | done |
| All 55 content packages installed into Bulk | done |
| `AppManager` completes `SetupSystem` | done |
| **VideoDaemon serves its socket** | **BLOCKED — next** |
| `AppManager` reaches its main loop / draws UI | not yet |

`./tadpole.sh --logo` works and is interactive. `./tadpole.sh` gets AppManager
through `SetupSystem` and then dies waiting on VideoDaemon.

## The blocker

`AppManager` logs `DaemonControl socket connect failed ret=-1` and wants
`/tmp/video_events_socket`. `rcS` starts `VideoDaemon 750 &` alongside it.

Under Tadpole, VideoDaemon prints `VideoDaemon: Started Process`, forks, and
**both processes call `exit_group(0)`** without ever calling `socket()` or
`bind()`. It is choosing to quit, not crashing.

Ruled out so far:
* not a missing shim — it links `libdl.so.0`, which we impersonate
* not the video assets — `/var/sounds/{Startup,Shutdown,Transition}Video.ogg`
  were dangling (the shipped symlinks point at `LucyAssets`, a different
  board; `rcS` repoints them per-platform). Fixed in `setup-sysroot.sh`.
  VideoDaemon still exits.

Paths it references: `/tmp/{play_trans,splash,ui_ready,vdaemon_play,
video_events_socket}`, `/flags/nousb`, `/var/sounds/*.ogg`.

## Next steps, in order

**1. Ask the hardware what VideoDaemon actually does.** The device is the
oracle and we have scripted shell access (`tools/lfsh.py`). With it awake:

```
tools/lfsh.py <ip> "ps"                    # is VideoDaemon even running?
tools/lfsh.py <ip> "ls -la /tmp /flags"    # which sockets/flags exist live
tools/lfsh.py <ip> "cat /proc/<pid>/cmdline | tr '\0' ' '"
tools/lfsh.py <ip> "strace -f -tt -o /tmp/vd.trace VideoDaemon 750"
```

A trace of a *working* VideoDaemon, diffed against ours, should say in one
read what it checks before giving up. This is the highest-value next action
by a wide margin.

**2. Parallel track — bypass AppManager entirely.**
`/LF/Base/Flash/bin/saplayer` is a standalone Flash Lite player. It links
`libdl.so.0` (our shim covers it) plus DisplayMPI, EventMPI, ButtonMPI and
`libflashdidj.so`. Pointing it at a `.swf` could put an interactive Flash app
on screen without AppManager, VideoDaemon or the Lightning UI. Cheap to try
and it exercises the whole display+input path end to end.

**3. Then: AppManager's remaining crash.** Currently a stack-corruption fault
deep in libc with an unsymbolised two-frame backtrace. Likely a knock-on from
the failed socket connect, so retest after (1).

**4. Fill in the shim as things demand it.** Expect more `ioctl`s once the UI
runs, and audio (ALSA) at some point.

## Technique notes worth remembering

* **No LD_PRELOAD.** uClibc here was built without
  `__LDSO_PRELOAD_ENV_SUPPORT__`. We impersonate a library the target already
  links (`libdl.so.0`, `libz.so.1`) by patching the real one's SONAME to a
  same-length name and taking its place. Two variants because the display
  tools link no libdl.
* **`qemu-arm -L` cannot create files.** It only redirects paths that already
  exist, so new files fall through to the host and fail with ENOENT. The shim
  translates creating opens itself.
* **Hooking `open()` is not enough.** uClibc's stdio reaches its own open
  through a hidden alias that never touches the PLT, so `fopen()` is
  invisible. The shim hooks `fopen`/`fopen64` too.
* **gdb works**: `qemu-arm -g <port>` + host `gdb -ex 'set architecture arm'`
  gives real backtraces through the stripped binaries via library symtabs.
  This found every crash so far.
