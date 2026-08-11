#!/usr/bin/env python3
"""Collect everything a bug report needs into one folder you can drag anywhere.

    ./tools/bug-report.py                       write a report, print where
    ./tools/bug-report.py --desc "text"         with the user's description
    ./tools/bug-report.py --desc-file FILE      ditto, read from a file
    ./tools/bug-report.py --shot shot.png       move a screenshot in beside it
    ./tools/bug-report.py --version 10082026-0007
    ./tools/bug-report.py --stdout              print the report, write nothing

The output is a folder holding two files:

    report.txt      everything below, plain text
    screen.png      the frame that was on screen, when the viewer supplied one

TWO LOOSE FILES, NOT A ZIP, because of where these actually go. Bugs arrive as
Discord messages, and a zip in a Discord message is a thing the maintainer must
download and unpack before knowing whether it was worth reading — while a .txt
and a .png both preview inline in the client. The folder is so the pair stays
together on the way there; nothing needs to open it to read it.

WHAT IS NOT IN HERE. No machine ID, no MAC, no serial number, nothing that
identifies a person rather than a configuration, and no network round trip:
this writes a file and stops. Home directories are rewritten to `~` and the
account name to `<user>` before anything is written, because the guest log is
full of both and a bug report is a public document.

OUTPUT IS PARSED BY THE VIEWER, so the machine-readable lines are prefixed and
dull, exactly as tools/check-update.py's are:

    @@REPORT /home/…/tadpole-report-11082026-1432

Everything else on stdout is for a human reading a terminal.

WHY THE LOG TAIL IS LAST. It is the longest section and the least often the
answer. The description someone typed is the most important thing in the file
and belongs where it will be read; a report that opens with 100 lines of UART
capture gets skimmed and closed.
"""
import argparse
import os
import platform
import re
import shutil
import sys
import time

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

LOG_LINES = 100        # how much of the log the report quotes
CRASH_LINES = 60
WINDOW_BYTES = 256 * 1024   # how much of it is searched for facts; see read_window


# ---- where things are ------------------------------------------------------
#
# The same chain tadpole_view.c's guest_log_path() walks, spelled the same way:
# XDG override, then the platform's app-data directory, then ~/.local. Getting
# this wrong produces a report with an empty log section on exactly the
# machines that set XDG_STATE_HOME, which is the hardest kind of bug to be
# told about.

def state_dir():
    x = os.environ.get("XDG_STATE_HOME")
    la = os.environ.get("LOCALAPPDATA")
    home = os.path.expanduser("~")
    if x:
        return os.path.join(x, "tadpole")
    if la:
        return os.path.join(la, "Tadpole", "state")
    return os.path.join(home, ".local", "state", "tadpole")


def config_path():
    x = os.environ.get("XDG_CONFIG_HOME")
    la = os.environ.get("LOCALAPPDATA")
    if x:
        return os.path.join(x, "tadpole", "ui.cfg")
    if la:
        return os.path.join(la, "Tadpole", "ui.cfg")
    return os.path.join(os.path.expanduser("~"), ".config", "tadpole", "ui.cfg")


def default_dest():
    """Somewhere the user will actually find it.

    The Desktop, when there is one — a report saved somewhere only the program
    knows about may as well not have been saved. Otherwise the state directory,
    which at least the viewer can open for them.
    """
    home = os.path.expanduser("~")
    for name in ("Desktop", "Bureau", "Escritorio"):
        d = os.path.join(home, name)
        if os.path.isdir(d):
            return d
    return state_dir()


# ---- redaction -------------------------------------------------------------

def redactor():
    """Rewrite home paths and the account name out of anything we quote.

    Built once and applied to every line, rather than to the whole file at the
    end, so a log big enough to matter does not get copied twice.

    The order matters: the home directory contains the user name, so it has to
    go first or the second rule turns `/home/alice/x` into `/home/<user>/x`
    and the first no longer matches.
    """
    home = os.path.expanduser("~")
    user = os.environ.get("USER") or os.environ.get("USERNAME") or ""
    rules = []
    if home and home not in ("/", ""):
        rules.append((re.compile(re.escape(home)), "~"))
        # Windows gives back a backslash path; the log may hold either.
        alt = home.replace("\\", "/")
        if alt != home:
            rules.append((re.compile(re.escape(alt)), "~"))
    if len(user) >= 3:
        rules.append((re.compile(r"\b%s\b" % re.escape(user)), "<user>"))

    def apply(s):
        for pat, sub in rules:
            s = pat.sub(sub, s)
        return s
    return apply


# ---- the facts -------------------------------------------------------------

def read_window(path, nbytes=WINDOW_BYTES):
    """The last nbytes of a file, as lines.

    Debug level 3 is qemu -strace, which produces logs measured in hundreds of
    megabytes. Reading one of those into memory to throw away all but the last
    hundred lines is how a bug report tool becomes a bug.

    WIDER THAN WHAT GETS QUOTED, and that is the point. The facts worth pulling
    out of a log — the GL renderer, which title was launched — are printed once,
    at startup, and so are never in the last hundred lines of anything. The
    first version of this looked for them in the same hundred lines it quoted
    and therefore never found either one: every report said "host GPU replay
    may be off" regardless of whether it was. The viewer truncates the log at
    each launch (tadpole_view.c opens it "w"), so this window is one run, and
    a quarter of a megabyte of it reaches back past the banner unless debug
    tracing is on.
    """
    try:
        size = os.path.getsize(path)
    except OSError:
        return None
    want = min(size, nbytes)
    try:
        with open(path, "rb") as f:
            f.seek(size - want)
            data = f.read(want)
    except OSError:
        return None
    text = data.decode("utf-8", "replace")
    if want < size:
        # Drop the first line: seeking by bytes almost certainly landed in the
        # middle of one, and half a line at the top reads as corruption.
        nl = text.find("\n")
        text = text[nl + 1:] if nl >= 0 else text
    return text.splitlines()


def os_description():
    """One line naming the operating system, as specifically as it will say."""
    sysname = platform.system()
    if sysname == "Linux":
        pretty = ""
        try:
            with open("/etc/os-release") as f:
                for line in f:
                    if line.startswith("PRETTY_NAME="):
                        pretty = line.split("=", 1)[1].strip().strip('"')
                        break
        except OSError:
            pass
        return "%s (kernel %s)" % (pretty or "Linux", platform.release())
    if sysname == "Windows":
        rel, ver, csd, ptype = platform.win32_ver()
        return "Windows %s %s (build %s)" % (rel, csd, ver)
    if sysname == "Darwin":
        return "macOS %s" % platform.mac_ver()[0]
    return "%s %s" % (sysname, platform.release())


def cpu_description():
    if platform.system() == "Linux":
        try:
            with open("/proc/cpuinfo") as f:
                for line in f:
                    if line.startswith("model name"):
                        return line.split(":", 1)[1].strip()
        except OSError:
            pass
    return platform.processor() or platform.machine() or "unknown"


def memory_mb():
    if platform.system() == "Linux":
        try:
            with open("/proc/meminfo") as f:
                for line in f:
                    if line.startswith("MemTotal:"):
                        return int(line.split()[1]) // 1024
        except (OSError, ValueError):
            pass
    return 0


def gpu_from_log(lines):
    """The GL renderer, as the HLE replayer itself reported it.

    tadpole_hle.c prints `tadpole-view: HLE replay on <renderer> (<version>)`
    when it comes up. Reading it back out of the log beats asking the system,
    because it is the string the code that broke was actually looking at — a
    machine with two GPUs can answer the question differently depending on who
    is asking.
    """
    pat = re.compile(r"HLE replay on (.+?) \((.+?)\)\s*$")
    for line in reversed(lines or []):
        m = pat.search(line)
        if m:
            return m.group(1), m.group(2)
    return None, None


def firmware_version():
    """Which system files are installed, from the directory name they unpack to."""
    root = os.path.join(PROJ, "rootfs")
    try:
        names = sorted(d for d in os.listdir(root)
                       if os.path.isdir(os.path.join(root, d)))
    except OSError:
        return None
    return ", ".join(names) if names else None


def newest_crash():
    """The most recent crash.log, if one was kept, and how long ago it was.

    Reported with its age because an unrelated crash from three weeks ago at
    the bottom of a report about a font is a red herring that costs somebody an
    afternoon.
    """
    root = os.path.join(state_dir(), "crashes")
    best, best_mtime = None, 0
    try:
        for d in os.listdir(root):
            p = os.path.join(root, d, "crash.log")
            try:
                mt = os.path.getmtime(p)
            except OSError:
                continue
            if mt > best_mtime:
                best, best_mtime = p, mt
    except OSError:
        return None, 0
    return best, best_mtime


def read_settings():
    """ui.cfg as key/value pairs, in the order it stores them."""
    out = []
    try:
        with open(config_path()) as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                out.append(line)
    except OSError:
        pass
    return out


def running_title(lines):
    """The last app the viewer said it was launching.

    Almost every report is about one title, and almost nobody names it in a way
    that matches what the package is called. The log knows.
    """
    pat = re.compile(r"(?:launch|Launching|running)[: ]+(\S+\.(?:swf|tar))", re.I)
    for line in reversed(lines or []):
        m = pat.search(line)
        if m:
            return os.path.basename(m.group(1))
    return None


# ---- the report ------------------------------------------------------------

def build(desc, version, brand, shot):
    red = redactor()
    log_path = os.path.join(state_dir(), "tadpole.log")
    log = read_window(log_path)
    log_tail = log[-LOG_LINES:] if log else None
    crash_path, crash_mtime = newest_crash()

    out = []
    w = out.append

    w("Tadpole bug report")
    w("=" * 60)
    w("")

    w("WHAT HAPPENED")
    w("-" * 60)
    if desc.strip():
        for line in desc.strip().splitlines():
            w(red(line))
    else:
        w("(nothing was typed — ask what they were doing)")
    w("")

    w("BUILD")
    w("-" * 60)
    w("%-14s %s" % ("Program", brand))
    # "dev" is not a version and should not be presented as one: it means an
    # unreleased working copy, and a report from one has to be read differently.
    w("%-14s %s" % ("Version", version if version and version != "dev"
                    else "%s (unreleased build)" % (version or "unknown")))
    fw = firmware_version()
    w("%-14s %s" % ("Firmware", fw if fw else "not installed"))
    title = running_title(log)
    if title:
        w("%-14s %s" % ("Last launched", title))
    w("%-14s %s" % ("Report time", time.strftime("%Y-%m-%d %H:%M:%S %z")))
    w("")

    w("MACHINE")
    w("-" * 60)
    w("%-14s %s" % ("OS", os_description()))
    w("%-14s %s" % ("Arch", platform.machine()))
    w("%-14s %s" % ("CPU", cpu_description()))
    mem = memory_mb()
    if mem:
        w("%-14s %d MB" % ("Memory", mem))
    renderer, glver = gpu_from_log(log)
    if renderer:
        w("%-14s %s" % ("GL renderer", renderer))
        w("%-14s %s" % ("GL version", glver))
    else:
        w("%-14s %s" % ("GL renderer", "not in the log — host GPU replay may be off"))
    # X11 vs Wayland decides whether half the input and window-size questions
    # are even applicable, and it costs one environment variable to know.
    session = os.environ.get("XDG_SESSION_TYPE")
    if session:
        w("%-14s %s" % ("Session", session))
    w("%-14s %s" % ("Python", platform.python_version()))
    w("")

    settings = read_settings()
    w("SETTINGS")
    w("-" * 60)
    if settings:
        for line in settings:
            w(red(line))
    else:
        w("(ui.cfg not found — defaults)")
    w("")

    w("SCREENSHOT")
    w("-" * 60)
    w("screen.png — the frame that was on screen" if shot
      else "none captured (report made without a running emulator, or from a terminal)")
    w("")

    if crash_path:
        age = time.time() - crash_mtime
        w("LAST CRASH  (%s)" % (
            "just now" if age < 120 else
            "%d minutes ago" % (age / 60) if age < 7200 else
            "%d hours ago" % (age / 3600) if age < 172800 else
            "%d days ago — probably unrelated" % (age / 86400)))
        w("-" * 60)
        for line in (read_window(crash_path) or [])[-CRASH_LINES:]:
            w(red(line))
        w("")

    w("LOG  (last %d lines of %s)" % (LOG_LINES, "tadpole.log"))
    w("-" * 60)
    if log_tail:
        for line in log_tail:
            w(red(line))
    else:
        w("(no log — logging to file may be off; Options -> Debug Settings)")
    w("")

    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--desc", default="", help="what the user says went wrong")
    ap.add_argument("--desc-file", help="read the description from a file")
    ap.add_argument("--version", default=os.environ.get("TADPOLE_VERSION", "dev"))
    ap.add_argument("--brand", default="Tadpole")
    ap.add_argument("--shot", help="a screenshot to move in beside the report")
    ap.add_argument("--out", help="folder to create the report folder inside")
    ap.add_argument("--stdout", action="store_true",
                    help="print the report and write nothing")
    args = ap.parse_args()

    desc = args.desc
    if args.desc_file:
        try:
            with open(args.desc_file, encoding="utf-8", errors="replace") as f:
                desc = f.read()
        except OSError as e:
            print("cannot read description: %s" % e, file=sys.stderr)
            return 2

    shot_ok = bool(args.shot and os.path.isfile(args.shot))
    text = build(desc, args.version, args.brand, shot_ok)

    if args.stdout:
        sys.stdout.write(text)
        return 0

    parent = args.out or default_dest()
    stamp = time.strftime("%d%m%Y-%H%M%S")
    folder = os.path.join(parent, "tadpole-report-%s" % stamp)
    try:
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, "report.txt"), "w",
                  encoding="utf-8") as f:
            f.write(text)
    except OSError as e:
        # SAY WHERE IT FAILED. "Could not save report" with no path is the
        # least useful sentence a program can print about a filesystem.
        print("could not write the report to %s: %s" % (folder, e),
              file=sys.stderr)
        return 1

    if shot_ok:
        try:
            shutil.copyfile(args.shot, os.path.join(folder, "screen.png"))
        except OSError as e:
            print("note: the screenshot could not be copied in: %s" % e)

    print("@@REPORT %s" % folder)
    print("Report written to %s" % folder)
    print("Drag %s into Discord, or attach %s to a GitHub issue."
          % ("report.txt and screen.png" if shot_ok else "report.txt",
             "them" if shot_ok else "it"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
