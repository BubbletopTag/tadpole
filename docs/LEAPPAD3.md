# LeapPad3 — porting notes

Working notes for the `leappad-emu-3` branch, in the spirit of `ULTRA.md`:
what was tried, what it did, and the measurement behind it.

Firmware under test: **6.2.0.654**, built 2016-03-22, `Device="LeapPad3"`.
Board **CABO**, NXP4330, 480x272.

---

## The premise, and how it differs from the Ultra

The LeapPad3 is **the Ultra's shell on the LeapPad2's panel**. Same AppServer /
Qt 4.8.4 QWS shell, same `-qws` launch, same `libEGL` injection vector, same
package-manager daemon over D-Bus — but 480x272 instead of 1024x600, so the two
geometry bugs the Ultra hit cannot bite here.

Three things are genuinely new.

**It is eMMC, not NAND.** Every LeapPad before it ships a UBI volume needing
ubireader. This one is `Type="DiskImage"` and carries
`firmware/emmc/2ext4/3/RFS`, which despite a path naming a filesystem is a
plain GNU tar of the root. `tar -xf` is the whole extraction. It lands in
`emmc_rfs`, a name four separate globs now know.

**Its firmware is not under a device directory.** LeapFrog counted the Ultra as
its third LeapPad, so `PAD3FW` is the ULTRA's directory and holds `PHR1-*`. The
retail LeapPad3 is `PAD3-*`, served from the ordinary content layout:

```
packages/0x002A0001/PAD3-0x002A0001-000000.lfp   141,684,486  Firmware Base
packages/0x002A0003/PAD3-0x002A0003-000000.lfp     6,701,279  Surgeon
packages/0x002A0001/PAD3-0x002A0001-000002.lfp         3,855  Bulk Empty
```

107 of the 109 packages in its list are downloadable (478 MB). The two that are
not are the "In-Home Only" LeapFrog Learning Songs pair — which is why the
Music app is empty, and not a playback fault.

**Its GPU is Mali, not vr5.** See the GL section below; this one is subtle and
cost a whole debugging round.

---

## What works

Boots to the home screen in ~10s. Touch, audio, icons, sign-in, parent
settings, and real internet.

`shots/` — `lp3-home.png`, `lp3-home-icons.png`, `lp3-touch.png`,
`lp3-signin.png`, `lp3-title-runs.png`.

---

## Four faults between a finger and the guest

Touch was the longest thread, and every one of these looked like the others.

**1. The evdev fake was a queue where the kernel is a broadcast.** Each node
was one FIFO handed to whoever opened it. `open()` on `/dev/input/eventN` gives
you your OWN queue and the kernel copies every event into all of them; a byte
read from a FIFO is gone for everyone else. The touchscreen has THREE readers
inside AppServer — Qt's tslib handler and both of Brio's
`ButtonPowerUSBTask` loads. Measured, one tap: `93 fd=24`, `71 fd=8`. Neither
saw a whole gesture. Fixed with a private pipe per open, fed by a pump thread.

**2. The pump needs its own thread.** Pumping from `read()` deadlocks: readers
sleep in `select()` until their fd is readable, and the pipe only fills if
somebody pumps. Every reader slept in front of a full FIFO.

**3. The viewer sent one sample per click.** A mouse reports on movement; a
touchscreen streams while a finger is down, and tslib's filters have memory.
`tap.py` had streamed at 50 Hz since the Ultra branch for exactly this reason.

**4. The identity pointercal was not the identity.** tslib's linear module is

```
x' = (a[2] + a[0]*x + a[1]*y) / a[6]
y' = (a[5] + a[3]*x + a[4]*y) / a[6]
```

so the file reads (xx, xy, xoff, yx, yy, yoff, scale) — offsets THIRD and
SIXTH. Written as `0 65536 0 0 0 65536 65536` it computes `x' = y, y' = 1`:
every tap to the top edge, wherever you pressed. The device's own numbers
settle it — `39032` is a scale and `-4245764` an offset, at positions 0 and 2.
The identity is `65536 0 0 0 65536 0 65536`.

---

## Nothing but the home screen could start

Reported as three faults — no app launched, no sign-in screen, no parent
settings — and it was one. AppServer starts its screens BY BARE NAME and none
could be resolved. MainPicker was the exception only because `DEV_FIRST_APP`
passed it by absolute path, which is what hid it.

The symptom is `Application: 'BrioWrapper' crashed, exit=0, error=0`, where
`error=0` is `QProcess::FailedToStart` — it never ran.

1. **The guest had no PATH.** `/etc/profile` sets it; we exec the shell
   directly and never source one.
2. **All eight entries in `/LF/Base/Qt/bin` are ABSOLUTE symlinks** pointing at
   `/LF/Base/Qt/Modules/<n>/<n>`. Through a symlinked directory the host
   resolves them against its own root and they dangle.
3. **`execvp` handed the PATH search to uClibc**, whose internal `execve` never
   goes through the PLT, so our hook was never entered and every candidate
   reached the host kernel as a raw guest path. Only `qemu -strace` showed it.

---

## The sign-in screen, and the out-of-box flow

Three more causes, in order found.

**The system bus was never started.** `rcS` starts two — `dbus-1` (system) and
`dbus-session`. We started one, and `libQtRioConnman` asks for
`QDBusConnection::systemBus()`.

The system bus needs a config of our own, because three things in
`/etc/dbus-1/system.conf` cannot work here and none can be turned off from the
command line on a dbus this old: the pidfile (dbus-daemon links none of the
shim's vectors, so it runs UNSHIMMED and cannot create files at all),
`<user>messagebus</user>`, and — **worth care** — a `<listen>` on
`/var/run/dbus/system_bus_socket`. `bind()` is not translated either, so that
path names the DEVELOPER'S OWN system bus. It failed only because a desktop bus
was already there.

**Nineteen of twenty-four UI strings did not exist.** The `src json` package
installs correctly and is simply a DIFFERENT REVISION from the firmware image.
A label that resolves to nothing still takes its place in the layout, so the
WiFi page's "Yes, Skip" confirmation was a blank, unhittable box — which is why
setup looped. `tools/fill-missing-text.py` fills the gaps; **the strings are
Tadpole's, not LeapFrog's**, and the file says so where they are written.

**`/flags/skip_oobe`** is the firmware's own escape hatch, found in
`libQtAppServer`'s strings. Without it AppServer runs InitialSetup, whose
second step is WiFi, which on a machine with no wireless hardware cannot be
satisfied AND cannot be skipped.

The **Parent Lock** is set by the device's own `PasscodeApp` ("Reset Parent
Lock… enter a new code now", no old code required). Where it persists was never
found: nothing in `LF/Bulk`, `flags` or `dev` changes, and mfgdata is all
zeros. An unset lock may simply read as `0000`.

---

## Networking: the guest had internet all along

qemu-user passes sockets to the host, so the guest shares its stack. From
inside the emulated device, with certificate verification on:

```
HTTP 200 in 0.997s
```

The only fault was **root certificates from 2016**: 158 of them, including
`DigiCert_Global_Root_CA` but not **G2**, which is what LeapFrog's chain is
rooted at today. `tools/netssl.py` exists for the same gap on the host side.
`setup-sysroot.sh` overlays the host's bundle.

**What the shell needed was a description, not a connection.** `runtime/`
`tadpole-connman` is a guest-side ARM daemon owning `net.connman`. The real
`connmand` is stopped by default and for a reason worth keeping:

> Under qemu-user it is not sandboxed. It sees the HOST's interfaces and
> attempted `Could not clear IPv4 address index 2`, `Adding interface enp5s0
> [ethernet]`. It failed only because we are not root. **Never run it
> elevated.** `TADPOLE_REAL_CONNMAN=1` restores it behind a warning.

The device then verifies for itself — it fetches
`connman.leapfrog.com/online/status.html` (still up, HTTP 200) before
believing it. So "online" is true, not merely asserted.

**LeapFrog's commerce backend is gone.** `endpointurls_leappad3explorer.json`
is a 404 from a live server; LeapSearch returns HTTP 403. The device has no way
to express "the internet works but LeapFrog is gone" — its model assumes
beacon-reachable implies services-reachable — so it enables the stores and they
fail at the network layer. The four SOAP endpoints are seeded at
`upca.tadpole.invalid` (RFC 2606, cannot resolve). They were briefly set to
libWebServices' own `localhost:8080` defaults, which is wrong here for the
reason this area keeps being wrong: **localhost in the guest is your machine.**

---

## GL: Mali bundles EGL into its client libraries

Titles reached their entry point and died on `<ASSERT>: eglInitialize() failed`
— the LeapPad2's oldest GL symptom, on a device where the GL shim was on.

It was on. It was never called. Instrumenting our `eglInitialize` printed
**nothing** while the assert still fired, which turned the question from "why
does ours fail" into "whose is this".

On the LeapPad2 the vr5 stack keeps EGL and GLES in separate libraries, so
impersonating `libEGL.so` suffices. Mali's client libraries are combined builds
that define the whole EGL API themselves:

```
libGLESv2.so    eglGetDisplay eglInitialize eglMakeCurrent eglSwapBuffers
libGLES_CM.so   eglGetDisplay eglInitialize  (+ __egl_platform_*, mali_egl_image_*)
```

and both sit BEFORE `libEGL.so` in BrioWrapper's `DT_NEEDED`. First definition
in the global scope wins. Aliasing both at the GLES1 shim fixes it; neither
BrioWrapper nor `libLightning2D` imports a GLES2-only entry point, so nothing
loses an API — only a second EGL.

Mali's `libEGL.so` also has a **malformed symbol table**: `.dynsym`'s `sh_info`
says 3 while symbols 3 and 4 are still `STB_LOCAL`. uClibc's loader never
looks; lld does, and refuses to link. `tools/make-egl-real.py` raises it to 5,
which states what is already true of the symbol order.

---

## Open: every Brio title dies in the wireless MPI

Two seconds into every Brio title, at a fixed address. **BrioWrapper links
`libWirelessMPI` and no title's own `App.so` does**, so this stands between the
shell and all of them.

```
signal SIGSEGV, fault 0x00000000
pc  libWirelessMPI.so+0x00002a28   =  CWirelessMPI::CWirelessMPI()+104
```

Disassembly and gdb agree: the instruction is `str r3, [r4]` with **r4 = 0**
(`this`), and it is in the **C++ exception cleanup path** — the block ends in
`__cxa_end_cleanup`. The same store at +64 on the normal path ran fine moments
earlier with a good pointer, so the unwinder is handing the landing pad a frame
it cannot trust. That is why a throw the caller might have caught is fatal.

**The exception is a `DBus::Error`**, read off `__cxa_throw`'s `type_info`:

```
Thread 1 hit Breakpoint 1, __cxa_throw () from .../libstdc++.so.6
0x464b6b1c:  "N4DBus5ErrorE"
```

thrown out of `Module::Connect(ICoreModule*&, "Wireless", 2)`. The wireless
module is the ad-hoc networking for local multiplayer — its own symbols say so:
`CWirelessModule::JoinAdhocNetwork`, `LeaveAdhocNetwork`,
`GetBroadcastAddress`, `GetWPAInterface` — and it is a D-Bus client for
wpa_supplicant (`fi.w1.wpa_supplicant1`, interface `wlan0`).

### Seven things it is NOT, each measured

1. **the fake ConnMan** — identical crash with it disabled entirely
2. **Brio module loading** — `libWireless.so` opens, fd 71; and unlike the
   Ultra, Brio is healthy here (the display module maps its three framebuffers)
3. **wpa_supplicant absent** — the rootfs ships it, `tadpole.sh` starts it with
   `-u` (D-Bus control interface, no hardware needed), it runs, no change
4. **`/LF/System/Wireless`** — a path in the module's strings that exists
   nowhere in the image; created it, no change, removed it again
5. **`Parent_AllowP2P=0`** — BrioWrapper builds the MPI regardless; the setting
   gates the in-game feature, not the constructor
6. **two copies of the MPI** — there is only one, `runtime/libs` links it, and
   the version handshake matches (both sides report 2)
7. **a missing bus address in the child** — BrioWrapper inherits both
   `DBUS_SYSTEM_BUS_ADDRESS` and `DBUS_SESSION_BUS_ADDRESS` correctly, read out
   of `/proc/<pid>/environ` while stopped

### Where to pick it up

`TADPOLE_GDB_MATCH=BrioWrapper TADPOLE_GDB_PORT=1234 ./tadpole.sh --boot`, then
`gdb -ex 'set architecture arm' -ex 'target remote :1234'`. Only the matched
binary waits, so the shell still boots and the title can be started by hand.

**A trap that cost a round:** `libdbus-1` is **dlopened** by the wireless
module, so it is not mapped when gdb attaches at the entry point. Breakpoints
on `dbus_bus_get` and friends are unresolvable there and silently never fire —
which reads as "this function is never called" and means nothing of the sort.
`__cxa_throw` works because libstdc++ is a direct `NEEDED`. Set libdbus
breakpoints AFTER the module is loaded, or break on the dlopen first.

The open question is which D-Bus operation fails and why. Reading it out of the
`DBus::Error` object was attempted; the stack layout moves between runs, so
pointers chased in one boot are not the same in the next. It wants a scripted
walk of the object's members, or a breakpoint inside dbus-c++ set once the
library is up.

---

## Smaller things worth not rediscovering

**`meta.inf` is not read-only.** The package-manager daemon's
`RebuildPackageDatabase` parses every `meta.inf` and WRITES EACH ONE BACK
normalised — dropping `Device=` and appending `Size=0`. Through a symlink that
lands in `rootfs/`, which is supposed to be pristine, and `Device=` is exactly
what autodetect matches. So the first boot of a LeapPad3 erased the evidence
that it was one, and the second detected a LeapPad2. 52 files.

**A sysfs write is a command, not a record.** `lf2000-power/shutdown` is how
the guest powers off, and the idle timeout does it after four minutes as
designed. Ours is a real file, so the instruction sat there being obeyed on
every later boot — about a second in, through a shutdown path that segfaults
without VideoDaemon. `tadpole.sh` resets it per boot.

**The device powers itself off after four minutes.** `/flags/idle_timeouts`
overrides it: ONE VALUE PER LINE, in MILLISECONDS, and do not ask for zero —
zero reaches a branch that prints "Disabling" and then powers off immediately.

**Every installed title needs a save directory and nothing creates it.** Saves
live at `/LF/Bulk/Data/Local/<profile>/<ProductID>/` — the BARE ProductID, not
the PackageID. Without it Brio's atomic write fails and the title draws
`Missing:LOAD_ERROR_TEXT`, its own placeholder for a load failure it has no
string to describe.

**`-strace` did not reach anything AppServer launched.** Every screen and title
is a child through the shim's execve rewrite, which built a fresh qemu without
the flag. "Trace the guest" traced the shell and none of the titles.

---

## Still to do

* the wireless MPI crash above — it blocks every Brio title
* black screens and heavy lag in titles that do start: the GLES1 shim is doing
  very little real drawing yet
* `runtime/tadpole-connman` ships in no build; neither do `shimlibs-egl` or
  `shimlibs-pkg`, so Qt devices are unpackaged as a whole
* the WiFi settings page (`RioConnmanApp`) has never been opened — only the
  status-bar path is measured
* glasspole cannot run this device yet: it needs `eventfd` (351), `pipe2` (359)
  and `pipe` (42). GLib's GWakeup tries all three and aborts. `fstatfs` (100)
  and `clock_settime` (264) also appeared and were survivable.
