#!/usr/bin/env python3
# Tadpole — Online System Update, with no shell involved.
#
#   tools/online-update.py [stage-dir]
#
# This is tools/online-update.sh as one program, for the same reason
# install-firmware.py exists: Windows has no shell to run the .sh, and the
# viewer refused it honestly rather than half-working. The shell version
# remains the Linux entry point and is unchanged.
#
# It is thin on purpose. Everything interesting already lives in the two tools
# it drives — fetch-firmware.py knows where LeapFrog keeps the packages, and
# install-firmware.py knows what to do with them — so this is the reachability
# check, the download, and the handoff.
#
# WHAT IT DOES NOT GET, unchanged from the shell version and neither a bug:
#
#   * "Firmware-BulkEmpty" is exactly what its name says — a 15 MB UBI volume
#     of zeros. /LF/Bulk is populated by the CONTENT packages.
#   * .lf3 packages are encrypted and Tadpole ships no key. They are skipped
#     unless keys/lf3.keys exists; everything else installs regardless.

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import netssl  # noqa: E402  — a sibling tool, not a package

PROJ = os.path.dirname(HERE)


def reachable(host="https://digitalcontent.leapfrog.com/packages/"):
    """Is the package server answering?  -> (ok, why-not)

    UP FRONT, AND THAT IS THE POINT. "Connection refused" three minutes into a
    download reads as a broken emulator; said before anything starts it reads
    as no internet, which is what it is. A 403 or 404 still proves the host
    answered, so only a transport failure counts as unreachable.

    AND IT SAYS WHICH FAILURE. This used to swallow the exception and report
    "cannot reach the server" for everything, which sent a Windows 7 user
    checking a network that was working perfectly: the real error was that the
    machine could not verify LeapFrog's certificate, and the message named the
    one thing that was not wrong. A diagnosis that points somewhere specific is
    most of the value of checking at all.
    """
    try:
        netssl.urlopen(host, timeout=20)
        return True, None
    except Exception as e:
        if getattr(e, "code", None) is not None:
            return True, None        # 403/404 still proves the host answered
        return False, netssl.explain(e)


def run(script, args):
    """Run a sibling tool with THIS interpreter.

    sys.executable, not "python3": on Windows the interpreter running us is
    very often the one Tadpole ships in build/deps/python, which is not on
    PATH and answers to no name. Handing the child a name instead of a path is
    how a working setup turns into "python is not recognised" one call deep.
    """
    return subprocess.call([sys.executable, os.path.join(HERE, script)] + args)


def device():
    """Which device to download, which is the one question this never asked.

    IT DOWNLOADED A LEAPPAD2 WHATEVER THE WIZARD SAID. fetch-firmware.py
    defaults to --device leappad2, and this passed no --device at all, so the
    Windows Online System Update ignored the "Which device?" page entirely —
    the page that exists for exactly this decision, since which firmware to
    fetch is the one thing that cannot be autodetected before there IS any
    firmware. online-update.sh grew the same fix; this is its other half.

    NOT the device that happens to be installed: that is the right answer for
    booting and the wrong one for a download, where the user's stated choice
    outranks what is on disk. TADPOLE_DEVICE first, then the wizard's saved
    answer, then the historical default.
    """
    dev = os.environ.get("TADPOLE_DEVICE", "").strip()
    if dev:
        return dev
    # The wizard's answer, out of the same file tad_ui_cfg_device() reads:
    # $XDG_CONFIG_HOME/tadpole/ui.cfg, whose lines are "key value" separated by
    # whitespace — NOT key=value. See ui_cfg_save() in the viewer.
    cfg = os.path.join(os.environ.get("XDG_CONFIG_HOME")
                       or os.path.join(os.path.expanduser("~"), ".config"),
                       "tadpole", "ui.cfg")
    found = ""
    try:
        with open(cfg, "r", errors="replace") as f:
            for line in f:
                bits = line.split(None, 1)
                if len(bits) == 2 and bits[0] == "device":
                    found = bits[1].strip()      # last one wins, as sed | tail
    except OSError:
        pass
    return found or "leappad2"


def main(argv):
    dev = device()
    # ONE CACHE PER DEVICE. A single shared sources/online-update/cache let a
    # Leapster GS downloaded last week supply the firmware for a Didj
    # downloaded today — see the long note in online-update.sh.
    stage = argv[1] if len(argv) > 1 else os.path.join(PROJ, "sources",
                                                       "online-update", dev)
    cache = os.path.join(stage, "cache")

    print("==> Online System Update")
    print("    from digitalcontent.leapfrog.com")
    print()

    try:
        os.makedirs(cache, exist_ok=True)
    except OSError as e:
        print("cannot write to %s: %s" % (cache, e), file=sys.stderr)
        return 1

    ok, why = reachable()
    if not ok:
        print("cannot reach digitalcontent.leapfrog.com — %s" % why,
              file=sys.stderr)
        print("  Nothing has been changed.", file=sys.stderr)
        return 1

    print("==> downloading packages for %s" % dev)
    sys.stdout.flush()
    if run("fetch-firmware.py", ["--device", dev, "--get", "all", "-o", cache]) != 0:
        print("download failed; nothing has been installed.", file=sys.stderr)
        return 1

    print()
    print("==> installing")
    sys.stdout.flush()
    return run("install-firmware.py", ["--device", dev, stage])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
