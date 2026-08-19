#!/usr/bin/env python3
"""Derive the shim's "real" helper libraries from the LIVE device's rootfs.

    ./tools/real-libs.py              refresh them for whatever device is live
    ./tools/real-libs.py --rootfs DIR from that tree instead
    ./tools/real-libs.py --check      say what would change, write nothing

WHAT THESE ARE. Every shim variant impersonates a library the guest already
has — libdl.so.0, libz.so.1, libEGL.so, libWebServices.so.1 — and then chains
into the real one. Two libraries cannot answer to one SONAME in a single link
map, so the real one is copied out of the rootfs with its SONAME patched to an
otherwise unused version: libdl.so.9, libz.so.9, libWebServices.so.9. The
replacement string is the same length as the original, so it patches in place
without shifting a single offset.

WHY THIS IS A TOOL AND NOT A MAKEFILE RULE ANY MORE.

These files are per-DEVICE, and nothing treated them that way. The Makefile
picked its rootfs with

    ROOTFS := $(firstword $(wildcard ../rootfs/*/emmc_rfs ../rootfs/*/ubi_rfs ...))

— the bare glob that tadpole.sh, setup-sysroot.sh and device.sh were all fixed
away from when a second device became installable, and that the Makefile's own
comment promised to keep in step with them. `emmc_rfs` is first in that list,
so installing a LeapPad3 next to a Leapster GS silently repointed the helpers
at the LeapPad3.

WHAT THAT COSTS, because it is not a near miss. uClibc's libdl is not a
standalone library: it reaches into the loader's own structures, and the
LeapPad3's is 0.9.33.1 where the Leapster GS's is 0.9.32.1. Mixed, dlsym does
not fail — it RETURNS NULL FOR EVERY SYMBOL. The shim resolves its whole
real_* table to NULL, and the first one it calls without checking takes the
process down:

    qemu: uncaught target signal 11 (Segmentation fault)

before AppManager's first line of output. Nothing names libdl, nothing names
the device, and the same binary works perfectly on the device the helper came
from. The Didj is a third uClibc again (0.9.29).

So the source of truth is the live device, and these are refreshed wherever the
live device can change: `make` builds them, and runtime/setup-sysroot.sh
refreshes them on every build and every device switch — which is the case
`make` cannot see, because switching devices does not run it.

WRITES ONLY ON A CONTENT CHANGE, and that is what makes it safe to call every
time. A timestamp rule cannot work here: the sources are extracted firmware
dated 2013, so they are always older than anything derived from them and make
would never rebuild. Comparing bytes is exact, costs a few hundred KB of
reading, and means a device switch self-corrects.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
RUNTIME = os.path.join(PROJ, "runtime")


def say(msg):
    print(msg, flush=True)


def live_rootfs():
    """-> the rootfs of the device that is live, or "".

    Asks runtime/device.sh rather than globbing, so this answers the same
    question tadpole.sh and setup-sysroot.sh answer, in the same way. Its
    functions need bash, not sh.
    """
    script = ('. "%s/device.sh"; d="$(tad_active_device)"; '
              '[ -n "$d" ] && tad_rootfs_for_device "$d"' % RUNTIME)
    try:
        out = subprocess.run(["bash", "-c", script], capture_output=True,
                             text=True, timeout=60)
        return out.stdout.strip().splitlines()[0] if out.stdout.strip() else ""
    except Exception:
        return ""


def any_rootfs():
    """The old bare glob, kept ONLY as a last resort.

    It is wrong whenever two devices are installed, which is the whole point of
    this file — but a checkout with one firmware and no sysroot built yet has
    no live device to ask about, and refusing there would break the first build
    after a fresh install.
    """
    import glob
    for pat in ("rootfs/*/emmc_rfs", "rootfs/*/ubi_rfs", "rootfs/*/jffs2_rfs",
                "rootfs/*/*/ubi_rfs"):
        hits = sorted(glob.glob(os.path.join(PROJ, pat)))
        if hits:
            return hits[0]
    return ""


def first_existing(*paths):
    for p in paths:
        if p and os.path.exists(p):
            return p
    return ""


def glob_one(pattern):
    import glob
    hits = sorted(glob.glob(pattern))
    return hits[0] if hits else ""


# EVERY DERIVED FILE, IN ONE TABLE.
#
# (output, how to find the source, [(old, new) SONAME patches])
#
# The patches are byte replacements over the whole file, each pair the same
# length, so nothing in the ELF moves. A NUL terminator is part of the match:
# without it "libz.so.1" would also rewrite "libz.so.1.2.3" and any string that
# merely starts the same way.
def plan(rootfs):
    lib = os.path.join(rootfs, "lib")
    usrlib = os.path.join(rootfs, "usr", "lib")
    dl_src = glob_one(os.path.join(lib, "libdl-*.so"))
    return [
        ("shimlibs/libdl.so.9", dl_src,
         [(b"libdl.so.0\0", b"libdl.so.9\0")]),
        ("shimlibs-z/libz.so.9",
         first_existing(os.path.join(usrlib, "libz.so.1"),
                        os.path.join(lib, "libz.so.1")),
         [(b"libz.so.1\0", b"libz.so.9\0")]),
        ("shimlibs-pkg/libWebServices.so.9",
         os.path.join(usrlib, "libWebServices.so.1"),
         [(b"libdl.so.0\0", b"libdl.so.9\0"),
          (b"libWebServices.so.1\0", b"libWebServices.so.9\0")]),
        # Copies of the first, beside the variants that name it in NEEDED. A Qt
        # guest gets exactly one impersonation directory on its
        # LD_LIBRARY_PATH, so each has to be self-contained.
        ("shimlibs-egl/libdl.so.9", None, None),
        ("shimlibs-pkg/libdl.so.9", None, None),
    ]


def derive(src, patches):
    with open(src, "rb") as f:
        blob = f.read()
    for old, new in patches:
        assert len(old) == len(new), "patch changes length: %r -> %r" % (old, new)
        blob = blob.replace(old, new)
    return blob


def write_if_changed(path, blob, check):
    """-> "same" | "wrote" | "would write" """
    try:
        with open(path, "rb") as f:
            if f.read() == blob:
                return "same"
    except OSError:
        pass
    if check:
        return "would write"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".new"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, path)
    return "wrote"


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--rootfs", default="")
    ap.add_argument("--check", action="store_true",
                    help="report what would change; write nothing")
    ap.add_argument("--quiet", action="store_true",
                    help="say nothing when everything is already correct")
    a = ap.parse_args(argv[1:])

    rootfs = a.rootfs or live_rootfs() or any_rootfs()
    if not rootfs or not os.path.isdir(rootfs):
        print("real-libs: no extracted firmware to derive from", file=sys.stderr)
        return 1

    changed, missing, base = 0, [], None
    for rel, src, patches in plan(rootfs):
        out = os.path.join(RUNTIME, rel)
        if patches is None:
            # A copy of shimlibs/libdl.so.9, which the table puts first.
            if base is None:
                continue
            blob = base
        else:
            if not src or not os.path.exists(src):
                # NOT AN ERROR. No device has all four: the Didj predates
                # libWebServices entirely, and a variant whose real library is
                # absent is one this device never loads.
                missing.append(rel)
                continue
            blob = derive(src, patches)
            if rel.endswith("shimlibs/libdl.so.9"):
                base = blob
        what = write_if_changed(out, blob, a.check)
        if what != "same":
            changed += 1
            say("    %s %s" % (what, os.path.relpath(out, PROJ)))

    if changed:
        say("    (from %s)" % os.path.relpath(rootfs, PROJ))
    elif not a.quiet:
        say("    shim helper libraries already match %s"
            % os.path.relpath(rootfs, PROJ))
    if missing and not a.quiet:
        say("    not in this firmware, skipped: %s" % ", ".join(missing))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
