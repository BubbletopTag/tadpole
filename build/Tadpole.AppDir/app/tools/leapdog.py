#!/usr/bin/env python3
"""LeapDog — watch a real LeapPad2 and compare it against Tadpole.

    leapdog.py term [-o FILE]                 DEVICE: logging serial terminal
    leapdog.py pipe [-o FILE]                 EMULATOR: timestamp stdin
    leapdog.py capture [-o FILE]              read-only capture, no keyboard
    leapdog.py diff DEVICE.log EMULATOR.log   find the FIRST divergence
    leapdog.py states FILE                    compact state-transition signature
    leapdog.py stalls FILE                    where the console went quiet
    leapdog.py normalize FILE                 show what the comparison sees

TWO FAILURE MODES, TWO INSTRUMENTS. The shim's crash handler
(tadpole_crash.c) catches faults — anything that raises a signal. It is blind
to the other half of the bug list: white screens and softlocks, which hang
without dying and so produce no signal at all. What those DO produce is
silence, and silence on the console is measurable. `--stall` is the watchdog
proper: it timestamps every line and flags gaps, on hardware and in the
emulator alike.

WHY A DIFF AND NOT JUST A WATCHDOG. A watchdog says something went wrong; a
differential trace says WHERE the two stopped agreeing, which is the only part
that has a cause. Everything after the first divergence is downstream
consequence, and chasing that means debugging a symptom several layers from the
bug.

WHY NOTHING NEEDS INSTALLING ON THE DEVICE. AppManager and the Flash UI already
narrate themselves to the console:

    trace:  ----------HomePickerState::KeyDown----------
    [0x200] SystemPlugin::ResetTouchscreenSampleRate(): false

Tadpole's guest runs the same binaries and emits the same lines, so the two
logs are directly comparable with no injection, no patched libraries and no
risk to the hardware. A GL-level tracer is a bigger hammer and can come later;
this channel is free.

USE `term`, NOT `capture`, IF ANYTHING ELSE IS ATTACHED. Two readers on one
serial port split the byte stream between them, so each sees a random half and
the log looks corrupted rather than contended — the same trap as two guests
holding the ev2 FIFO. `term` replaces minicom rather than competing with it:
it forwards the keyboard as well, so the shell stays usable. Quit with Ctrl-].

SERIAL SETUP. 115200 8N1 on a CP2102. The port is root:uucp, so either run
under sudo or join the group once:

    sudo usermod -aG uucp $USER     # then log out and back in

pyserial is deliberately not a dependency — stty configures the line and the
device node is then just a file. (The DEVICE has no Python at all, so anything
that must run on the LeapPad itself has to be busybox sh or a compiled ARM
binary. Everything here is host-side.)
"""
import difflib
import os
import re
import select
import subprocess
import sys
import termios
import time
import tty


# ---- what counts as a comparable line -----------------------------------
# The two logs are full of things that cannot match by construction: the
# device prints kernel messages and shell prompts, the emulator prints qemu
# and shim diagnostics. Only the application's own narration is common to
# both, so that is what gets compared.
APP_LINE = re.compile(r"^(trace:|\[0x[0-9a-fA-F]+\]|AppManager|Brio)")

# LeapDog's own annotations. Written into the log so a stall is visible in
# context, stripped before comparison so they can never create a divergence.
MARK = re.compile(r"^\s*\*\*\* LeapDog:")

# The capture timestamp, "[   12.345] ". Stripped before comparison: the whole
# point is that the device and the emulator take different amounts of time.
TS = re.compile(r"^\[\s*\d+\.\d+\]\s?")

# Normalisation. Deliberately conservative: over-normalising invents matches
# that are not real, which is worse than a noisy diff.
SUBS = [
    (re.compile(r"0x[0-9a-fA-F]{4,}"), "0xADDR"),   # pointers, handles
    (re.compile(r"\b\d{6,}\b"), "NUM"),             # pids, timestamps, serials
    (re.compile(r"Explorer-[0-9A-F]+"), "HOST"),    # the device's hostname
    (re.compile(r"-{4,}"), "--"),                   # trace decoration varies
    (re.compile(r"\s+"), " "),
]

STATE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*(?:State|LPAD|Plugin|Manager))"
                   r"::([A-Za-z_][A-Za-z0-9_]*)")


def normalize(line):
    s = TS.sub("", line.rstrip("\r\n"))
    s = s.replace("trace:", "", 1).strip()
    for rx, rep in SUBS:
        s = rx.sub(rep, s)
    return s.strip()


def load(path, app_only=True, frm=None, to=None, ignore=()):
    """Return [(original_lineno, normalised_text)] for comparable lines.

    `frm`/`to` window the log, applied to each file independently so the two
    captures need not start at the same moment — which they never do.

    WHY WINDOWING MATTERS MORE THAN IT LOOKS. The device and the emulator do
    not have the same titles installed, so boot and the home screen differ for
    reasons that are already understood: different icon counts, a different
    app enumeration, different pages. Diffing from t=0 buries the interesting
    divergence under hundreds of lines of known-uninteresting one. Anchor on
    the app launch and compare from there:

        leapdog.py diff dev.log emu.log --from 'LaunchApp|ReplaceTopApp'
    """
    started = frm is None
    out = []
    with open(path, "r", errors="replace") as fh:
        for n, line in enumerate(fh, 1):
            if MARK.match(line):
                continue
            body = TS.sub("", line.lstrip())
            if not started:
                if frm.search(body):
                    started = True
                else:
                    continue
            if to is not None and to.search(body):
                break
            if app_only and not APP_LINE.match(body):
                continue
            if any(rx.search(body) for rx in ignore):
                continue
            s = normalize(line)
            if s:
                out.append((n, s))
    return out


# ---- capture / terminal --------------------------------------------------
QUIT = 0x1D          # Ctrl-], as telnet uses. Ctrl-A belongs to minicom and
                     # Ctrl-C must reach the device's shell.


def parse_common(argv):
    opt = {"dev": "/dev/ttyUSB0", "baud": "115200", "out": None,
           "stall": 10.0, "quiet": False}
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ("-o", "--out"):
            i += 1; opt["out"] = argv[i]
        elif a in ("-d", "--device"):
            i += 1; opt["dev"] = argv[i]
        elif a in ("-b", "--baud"):
            i += 1; opt["baud"] = argv[i]
        elif a in ("-s", "--stall"):
            i += 1; opt["stall"] = float(argv[i])
        elif a in ("-q", "--quiet"):
            opt["quiet"] = True
        i += 1
    if opt["out"] is None:
        opt["out"] = "device-%s.log" % time.strftime("%Y%m%d-%H%M%S")
    return opt


def open_serial(opt):
    try:
        subprocess.run(["stty", "-F", opt["dev"], opt["baud"], "raw", "-echo"],
                       check=True, capture_output=True)
    except FileNotFoundError:
        sys.stderr.write("leapdog: stty not found\n")
        return None
    except subprocess.CalledProcessError as e:
        sys.stderr.write("leapdog: cannot configure %s: %s\n"
                         % (opt["dev"], e.stderr.decode(errors="replace").strip()))
        return None
    try:
        return open(opt["dev"], "r+b", buffering=0)
    except PermissionError:
        sys.stderr.write(
            "leapdog: permission denied on %s\n"
            "  run under sudo, or once:  sudo usermod -aG uucp $USER\n"
            % opt["dev"])
    except OSError as e:
        sys.stderr.write("leapdog: %s\n" % e)
    return None


class Recorder:
    """Line assembly, timestamping, and the stall watchdog.

    Timestamps are relative to the start of the capture, not wall clock: the
    only question ever asked of them is "how long was the gap", and a relative
    clock makes that readable without arithmetic.
    """

    def __init__(self, fh, stall, echo=True):
        self.fh = fh
        self.stall = stall
        self.echo = echo
        self.t0 = time.time()
        self.last = self.t0
        self.buf = b""
        self.lines = 0
        self.stalls = 0
        self.in_stall = False

    def _write(self, text):
        self.fh.write("[%9.3f] %s\n" % (time.time() - self.t0, text))
        if self.echo:
            sys.stdout.write(text + "\r\n")
            sys.stdout.flush()

    def note(self, text):
        self._write("*** LeapDog: %s ***" % text)

    def feed(self, chunk):
        now = time.time()
        if self.in_stall:
            self.note("console resumed after %.1fs of silence" % (now - self.last))
            self.in_stall = False
        self.last = now
        # Normalise line endings before splitting. A serial console emits
        # CRLF, and a chunk boundary can fall between the CR and the LF — so
        # splitting on LF alone leaves a stray CR that turns into a blank line
        # in the log. Folding both to LF and dropping the empties survives any
        # split point, because a CRLF broken across two reads becomes a real
        # line followed by an empty one that is then discarded.
        self.buf += chunk
        self.buf = self.buf.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
        while b"\n" in self.buf:
            raw, self.buf = self.buf.split(b"\n", 1)
            # latin1 never raises: a serial line picks up the occasional
            # partial byte and a UnicodeDecodeError must not end a capture
            # that has been running for ten minutes.
            text = raw.decode("latin1")
            if not text.strip():
                continue
            self._write(text)
            self.lines += 1

    def tick(self):
        """Call periodically; flags a stall once per silent period."""
        if self.stall <= 0 or self.in_stall:
            return
        quiet = time.time() - self.last
        if quiet >= self.stall:
            self.in_stall = True
            self.stalls += 1
            self.note("console SILENT for %.1fs — possible hang" % quiet)

    def summary(self):
        dur = time.time() - self.t0
        return "%d lines in %.1fs, %d stall(s)" % (self.lines, dur, self.stalls)


def cmd_term(argv):
    """Interactive logging terminal: replaces minicom rather than fighting it."""
    opt = parse_common(argv)
    ser = open_serial(opt)
    if ser is None:
        return 1

    interactive = sys.stdin.isatty()
    saved = None
    sys.stderr.write(
        "LeapDog %s @ %s -> %s\n"
        "  stall warning after %.1fs of silence\n"
        "  %s\n"
        % (opt["dev"], opt["baud"], opt["out"], opt["stall"],
           "quit with Ctrl-]" if interactive
           else "stdin is not a tty — read-only, ctrl-c to stop"))

    try:
        with open(opt["out"], "w", buffering=1) as fh:
            rec = Recorder(fh, opt["stall"], echo=not opt["quiet"])
            rec.note("capture started")
            if interactive:
                saved = termios.tcgetattr(sys.stdin.fileno())
                tty.setraw(sys.stdin.fileno())
            watch = [ser]
            if interactive:
                watch.append(sys.stdin)
            while True:
                ready, _, _ = select.select(watch, [], [], 0.5)
                for src in ready:
                    if src is ser:
                        chunk = ser.read(4096)
                        if chunk:
                            rec.feed(chunk)
                    else:
                        data = os.read(sys.stdin.fileno(), 1024)
                        if not data:
                            raise KeyboardInterrupt
                        if QUIT in data:
                            raise KeyboardInterrupt
                        ser.write(data)
                rec.tick()
    except KeyboardInterrupt:
        pass
    finally:
        if saved is not None:
            termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, saved)
        ser.close()
    sys.stderr.write("\nstopped: %s -> %s\n" % (rec.summary(), opt["out"]))
    return 0


def cmd_pipe(argv):
    """Timestamp a log arriving on stdin — the EMULATOR half of a comparison.

        ./tadpole.sh --boot 2>&1 | leapdog.py pipe -o emu.log

    Exists so both sides of a diff are recorded by the same code. The device
    log gets its timestamps from `term`; without this the emulator log would
    have none, `stalls` could not run on it at all, and the one measurement
    that matters most — "the emulator goes quiet where hardware does not" —
    would be unavailable on the side actually being debugged.
    """
    opt = parse_common(argv)
    src = sys.stdin.buffer
    sys.stderr.write("piping stdin -> %s (stall %.1fs)\n"
                     % (opt["out"], opt["stall"]))
    try:
        with open(opt["out"], "w", buffering=1) as fh:
            rec = Recorder(fh, opt["stall"], echo=not opt["quiet"])
            rec.note("capture started")
            while True:
                # A timeout on the select is what makes the watchdog work: a
                # blocking read would notice a hang only once it ended.
                ready, _, _ = select.select([src], [], [], 0.5)
                if ready:
                    chunk = src.read1(4096) if hasattr(src, "read1") \
                        else os.read(src.fileno(), 4096)
                    if not chunk:
                        break
                    rec.feed(chunk)
                rec.tick()
    except KeyboardInterrupt:
        pass
    sys.stderr.write("\nstopped: %s -> %s\n" % (rec.summary(), opt["out"]))
    return 0


def cmd_capture(argv):
    """Read-only. Only safe when nothing else holds the port."""
    argv = list(argv)
    opt = parse_common(argv)
    ser = open_serial(opt)
    if ser is None:
        return 1
    sys.stderr.write("capturing %s -> %s   (ctrl-c to stop)\n"
                     % (opt["dev"], opt["out"]))
    try:
        with open(opt["out"], "w", buffering=1) as fh:
            rec = Recorder(fh, opt["stall"], echo=not opt["quiet"])
            rec.note("capture started")
            while True:
                ready, _, _ = select.select([ser], [], [], 0.5)
                if ready:
                    chunk = ser.read(4096)
                    if chunk:
                        rec.feed(chunk)
                rec.tick()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    sys.stderr.write("\nstopped: %s -> %s\n" % (rec.summary(), opt["out"]))
    return 0


# ---- stalls --------------------------------------------------------------
def cmd_stalls(argv):
    """Report every gap in a captured log, longest first.

    Works on any log with LeapDog timestamps, from the device or the emulator,
    which is the point: "the emulator goes quiet for 4s where hardware never
    pauses longer than 0.2s" is a measurement, not an impression.
    """
    if not argv:
        sys.stderr.write("usage: leapdog.py stalls FILE [--min SECONDS]\n")
        return 2
    path = argv[0]
    lo = 1.0
    if "--min" in argv:
        lo = float(argv[argv.index("--min") + 1])

    prev_t, prev_line, gaps = None, "", []
    with open(path, "r", errors="replace") as fh:
        for line in fh:
            m = re.match(r"^\[\s*(\d+\.\d+)\]\s?(.*)$", line.rstrip("\n"))
            if not m:
                continue
            t, body = float(m.group(1)), m.group(2)
            # Skip LeapDog's own stall banners. They are written DURING the gap
            # they describe, so counting them makes every gap look like it
            # ended the moment it was detected, and reports the banner as the
            # line that broke the silence.
            if MARK.match(body):
                continue
            if prev_t is not None and t - prev_t >= lo:
                gaps.append((t - prev_t, prev_t, prev_line, body))
            prev_t, prev_line = t, body

    if not gaps:
        print("No gaps >= %.1fs." % lo)
        return 0
    print("%d gap(s) >= %.1fs, longest first:\n" % (len(gaps), lo))
    for dur, at, before, after in sorted(gaps, reverse=True):
        print("  %6.1fs silent at t=%.3f" % (dur, at))
        print("      last: %s" % before[:100])
        print("      next: %s" % after[:100])
        print()
    return 0


# ---- diff ----------------------------------------------------------------
def cmd_diff(argv):
    app_only = True
    ctx = 6
    frm = to = None
    ignore = []
    files = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--all":
            app_only = False
        elif a in ("-c", "--context"):
            i += 1; ctx = int(argv[i])
        elif a == "--from":
            i += 1; frm = re.compile(argv[i])
        elif a == "--to":
            i += 1; to = re.compile(argv[i])
        elif a == "--ignore":
            i += 1; ignore.append(re.compile(argv[i]))
        elif a == "--in-game":
            # The install sets differ, so everything before the app starts is
            # expected to diverge. This is the shorthand for "compare the game,
            # not the menu".
            frm = re.compile(r"LaunchApp|ReplaceTopApp|LoadNewApp")
            ignore += [re.compile(r"LoadIconImage"), re.compile(r"GetAllIcons")]
        else:
            files.append(a)
        i += 1
    if len(files) != 2:
        sys.stderr.write(
            "usage: leapdog.py diff DEVICE.log EMULATOR.log [--in-game]\n"
            "                      [--from RE] [--to RE] [--ignore RE] [-c N]\n")
        return 2

    a = load(files[0], app_only, frm, to, ignore)
    b = load(files[1], app_only, frm, to, ignore)
    if not a or not b:
        sys.stderr.write("leapdog: no comparable lines in %s\n"
                         % (files[0] if not a else files[1]))
        return 1

    # Report what was dropped as well as what was kept. The emulator log
    # carries plenty the device cannot produce — [tadpole] shim lines, [hle]
    # status, qemu diagnostics — and silently discarding most of a file is
    # exactly the sort of thing that should be visible rather than assumed.
    for label, path, kept in (("device", files[0], a), ("emulator", files[1], b)):
        with open(path, "r", errors="replace") as fh:
            total = sum(1 for _ in fh)
        print("%-8s %-44s %d comparable of %d lines"
              % (label, os.path.basename(path), len(kept), total))
    print()

    sm = difflib.SequenceMatcher(None, [t for _, t in a], [t for _, t in b],
                                 autojunk=False)
    ops = sm.get_opcodes()
    matched = sum(i2 - i1 for tag, i1, i2, _, _ in ops if tag == "equal")

    first = next((o for o in ops if o[0] != "equal"), None)
    if first is None:
        print("No divergence: the comparable lines are identical.")
        return 0

    tag, i1, i2, j1, j2 = first
    print("Matched %d line(s) in common; FIRST DIVERGENCE at" % matched)
    print("  device   line %d" % (a[i1][0] if i1 < len(a) else -1))
    print("  emulator line %d" % (b[j1][0] if j1 < len(b) else -1))
    print()

    for k in range(max(0, i1 - ctx), i1):
        print("   both | %s" % a[k][1])
    print("  ", "-" * 60)
    for k in range(i1, min(len(a), i2 if i2 > i1 else i1 + ctx)):
        print("  DEV  | %s" % a[k][1])
    for k in range(j1, min(len(b), j2 if j2 > j1 else j1 + ctx)):
        print("  EMU  | %s" % b[k][1])
    print("  ", "-" * 60)

    later = [o for o in ops if o[0] != "equal"]
    print("\n%d divergent region(s) overall. The first is the one with a "
          "cause;\nthe rest are almost certainly consequences of it." % len(later))
    return 0


# ---- states --------------------------------------------------------------
def cmd_states(argv):
    if not argv:
        sys.stderr.write("usage: leapdog.py states FILE\n")
        return 2
    seen = []
    with open(argv[0], "r", errors="replace") as fh:
        for line in fh:
            m = STATE.search(line)
            if m:
                sig = "%s::%s" % (m.group(1), m.group(2))
                # Collapse immediate repeats: the UI calls EnableArrows three
                # times in a row and that says nothing about ordering.
                if not seen or seen[-1] != sig:
                    seen.append(sig)
    for s in seen:
        print(s)
    sys.stderr.write("\n%d transition(s)\n" % len(seen))
    return 0


def cmd_normalize(argv):
    if not argv:
        sys.stderr.write("usage: leapdog.py normalize FILE\n")
        return 2
    for n, s in load(argv[0]):
        print("%6d  %s" % (n, s))
    return 0


def main(argv):
    if not argv:
        sys.stderr.write(__doc__)
        return 2
    cmd, rest = argv[0], argv[1:]
    if cmd == "term":
        return cmd_term(rest)
    if cmd == "capture":
        return cmd_capture(rest)
    if cmd == "pipe":
        return cmd_pipe(rest)
    if cmd == "stalls":
        return cmd_stalls(rest)
    if cmd == "diff":
        return cmd_diff(rest)
    if cmd == "states":
        return cmd_states(rest)
    if cmd == "normalize":
        return cmd_normalize(rest)
    sys.stderr.write("leapdog: unknown command '%s'\n" % cmd)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
