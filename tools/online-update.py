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
PROJ = os.path.dirname(HERE)


def reachable(host="https://digitalcontent.leapfrog.com/packages/"):
    """Is the package server answering?

    UP FRONT, AND THAT IS THE POINT. "Connection refused" three minutes into a
    download reads as a broken emulator; said before anything starts it reads
    as no internet, which is what it is. A 403 or 404 still proves the host
    answered, so only a transport failure counts as unreachable.
    """
    import urllib.request
    try:
        urllib.request.urlopen(host, timeout=20)
        return True
    except Exception as e:
        return getattr(e, "code", None) is not None


def run(script, args):
    """Run a sibling tool with THIS interpreter.

    sys.executable, not "python3": on Windows the interpreter running us is
    very often the one Tadpole ships in build/deps/python, which is not on
    PATH and answers to no name. Handing the child a name instead of a path is
    how a working setup turns into "python is not recognised" one call deep.
    """
    return subprocess.call([sys.executable, os.path.join(HERE, script)] + args)


def main(argv):
    stage = argv[1] if len(argv) > 1 else os.path.join(PROJ, "sources", "online-update")
    cache = os.path.join(stage, "cache")

    print("==> Online System Update")
    print("    from digitalcontent.leapfrog.com")
    print()

    try:
        os.makedirs(cache, exist_ok=True)
    except OSError as e:
        print("cannot write to %s: %s" % (cache, e), file=sys.stderr)
        return 1

    if not reachable():
        print("cannot reach digitalcontent.leapfrog.com.", file=sys.stderr)
        print("  Check the network and try again; nothing has been changed.",
              file=sys.stderr)
        return 1

    print("==> downloading packages")
    sys.stdout.flush()
    if run("fetch-firmware.py", ["--get", "all", "-o", cache]) != 0:
        print("download failed; nothing has been installed.", file=sys.stderr)
        return 1

    print()
    print("==> installing")
    sys.stdout.flush()
    return run("install-firmware.py", [stage])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
