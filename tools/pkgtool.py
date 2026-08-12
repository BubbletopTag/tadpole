#!/usr/bin/env python3
"""Tadpole — read and unpack LeapFrog packages without unzip or bzip2.

    pkgtool.py meta    <archive>            first meta.inf, to stdout
    pkgtool.py list    <archive>            member names, one per line
    pkgtool.py extract <archive> <dir>      unpack it
    pkgtool.py ubi     <image.ubi> <dir>    UBIFS volume -> a directory tree

WHY. The two package formats are ordinary archives wearing LeapFrog's
extensions —

    .lfp   a ZIP
    .lf2   a bzip2 tar

— and until now the installer shelled out to `unzip` and `bzcat` to read them.
Both are stdlib in Python (zipfile, tarfile, bz2), and Tadpole already ships a
Python for ubi_reader's sake, so requiring the user to install two more
packages bought nothing. install-firmware.sh still prefers unzip/bzcat when
they are there; this is what it uses when they are not.

`ubi` is a thin front end onto ubi_reader, which has no stable command-line
entry point once its console scripts are pruned out of the bundle.

EXTRACTION IS PATH-CHECKED. A tar or zip member is free to name
"../../etc/whatever", and these archives come off the internet — refusing to
write outside the destination costs four lines here and is not something to
leave to chance.
"""

import bz2
import os
import sys
import tarfile
import zipfile


def die(msg):
    sys.stderr.write("pkgtool: %s\n" % msg)
    raise SystemExit(1)


def is_zip(path):
    with open(path, "rb") as f:
        return f.read(4)[:2] == b"PK"


def _safe_join(dest, name):
    """Where `name` may be written, or None if it escapes `dest`."""
    dest = os.path.realpath(dest)
    full = os.path.realpath(os.path.join(dest, name))
    if full == dest or full.startswith(dest + os.sep):
        return full
    return None


def members(path):
    """(name, opener) for every file in the archive."""
    if is_zip(path):
        z = zipfile.ZipFile(path)
        for info in z.infolist():
            if not info.is_dir():
                yield info.filename, (lambda i=info: z.open(i).read())
    else:
        # bzip2 tar, whatever the extension claims.
        t = tarfile.open(path, "r:bz2")
        for info in t:
            if info.isfile():
                yield info.name, (lambda i=info: t.extractfile(i).read())


def cmd_meta(path):
    for name, read in members(path):
        if os.path.basename(name) == "meta.inf":
            sys.stdout.buffer.write(read())
            return 0
    return 1                       # no meta.inf is not an error, just empty


def cmd_list(path):
    for name, _ in members(path):
        print(name)
    return 0


def cmd_extract(path, dest):
    os.makedirs(dest, exist_ok=True)
    skipped = 0
    if is_zip(path):
        with zipfile.ZipFile(path) as z:
            for info in z.infolist():
                out = _safe_join(dest, info.filename)
                if out is None:
                    skipped += 1
                    continue
                if info.is_dir():
                    os.makedirs(out, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with z.open(info) as src, open(out, "wb") as dst:
                    dst.write(src.read())
    else:
        with tarfile.open(path, "r:bz2") as t:
            for info in t:
                out = _safe_join(dest, info.name)
                if out is None:
                    skipped += 1
                    continue
                if info.isdir():
                    os.makedirs(out, exist_ok=True)
                elif info.isfile():
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with t.extractfile(info) as src, open(out, "wb") as dst:
                        dst.write(src.read())
                # Devices, links and the rest have no business in a content
                # package; ignoring them is the safe reading.
    if skipped:
        sys.stderr.write("pkgtool: skipped %d member(s) with unsafe paths\n" % skipped)
    return 0


def merge_tree(src, dst):
    """Copy a directory over one that may already exist.

    shutil.copytree's dirs_exist_ok does this in one line AND IS PYTHON 3.8.
    The bundled interpreter on Windows is 3.7 — deliberately, because 3.8
    cannot load a .pyd on an unpatched Windows 7 (see tools/build-windows.sh)
    — so that one keyword raised TypeError on the one platform this branch
    exists for. Nothing on Linux ever reached it, which is exactly why it
    survived: the symlink ledger above only replays on hosts that cannot make
    symlinks.
    """
    import shutil as _sh
    os.makedirs(dst, exist_ok=True)
    for name in os.listdir(src):
        s, d = os.path.join(src, name), os.path.join(dst, name)
        if os.path.isdir(s) and not os.path.islink(s):
            merge_tree(s, d)
        elif not os.path.exists(d):
            try:
                os.link(s, d)          # same volume, no privilege needed
            except OSError:
                _sh.copy2(s, d)


def cmd_ubi(image, dest):
    try:
        from ubireader.scripts.ubireader_extract_files import main
    except ImportError as e:
        die("ubi_reader is not available to this Python (%s).\n"
            "         Run tools/fetch-deps.sh, or install ubi_reader." % e)
    os.makedirs(dest, exist_ok=True)

    # SYMLINKS DO NOT SURVIVE WINDOWS, unless caught here. os.symlink needs a
    # privilege an ordinary session lacks, and ubi_reader swallows the failure
    # per link — so the extraction "succeeds" minus every symlink in the image,
    # which for this rootfs means /lib's SONAME chain, the ELF interpreter
    # /lib/ld-uClibc.so.0 and about a hundred busybox applet names: nothing
    # dynamic can load, and nothing says why. Divert to a ledger instead: try
    # the real symlink first (Linux never reaches the ledger), then materialise
    # each recorded link as a HARD link — same volume, no privilege, and native
    # code reads it as the plain file it is — with a copy as the fallback.
    # Multiple passes, because a link's target may itself be a link that a
    # later pass creates.
    import shutil
    pending = []
    real_symlink = os.symlink

    def recording_symlink(src, dst, *a, **k):
        try:
            real_symlink(src, dst, *a, **k)
        except (OSError, NotImplementedError):
            pending.append((src, dst))

    # DEVICE NODES CANNOT BE MADE HERE, AND ARE NOT WANTED. os.mknod does not
    # exist AT ALL on Windows, so ubi_reader's per-node try/except reports
    #
    #     Warn: DEV Fail: module 'os' has no attribute 'mknod'
    #
    # once per node — alarming enough to be reported as a bug, while the
    # firmware it produced was in fact complete. On Linux the same nodes fail
    # just as surely, with EPERM, because mknod on a character device is
    # root-only and this runs as an ordinary user. So NEITHER platform has ever
    # created one, the extraction has always been "missing" /dev/console and
    # friends, and nothing has ever needed them: the guest's /dev is served by
    # the shim, not by the image.
    #
    # Recording them rather than letting each one raise keeps the log honest —
    # the count is reported below — and, unlike ubi_reader's use_dummy_devices,
    # writes nothing to disk. A dummy REGULAR file at /dev/console would be
    # worse than an absent one: the guest's open() would succeed and it would
    # then read and write a file nobody drains.
    real_mknod = getattr(os, "mknod", None)
    skipped_devs = []

    def recording_mknod(path, mode=0o600, device=0, *a, **k):
        if real_mknod is None:
            skipped_devs.append(path)
            return
        try:
            real_mknod(path, mode, device, *a, **k)
        except (OSError, NotImplementedError):
            skipped_devs.append(path)

    os.symlink = recording_symlink
    os.mknod = recording_mknod
    try:
        sys.argv = ["ubireader_extract_files", "-o", dest, image]
        rc = main() or 0
    finally:
        os.symlink = real_symlink
        if real_mknod is None:
            del os.mknod
        else:
            os.mknod = real_mknod

    if skipped_devs:
        sys.stderr.write(
            "pkgtool: %d device node(s) not created (%s and the rest) - the "
            "guest's /dev comes from the shim, so the extraction is complete "
            "without them\n"
            % (len(skipped_devs), os.path.basename(skipped_devs[0])))

    root = os.path.abspath(dest)
    for _ in range(8):                       # link-to-link chains, not loops
        if not pending:
            break
        again = []
        for src, dst in pending:
            # A guest-absolute target ("/bin/busybox") is rooted in the
            # extraction, not the host.
            t = (os.path.join(root, src.lstrip("/\\")) if os.path.isabs(src)
                 else os.path.join(os.path.dirname(dst), src))
            if os.path.isdir(t):
                merge_tree(t, dst)
            elif os.path.isfile(t):
                try:
                    os.link(t, dst)
                except OSError:
                    shutil.copy2(t, dst)
            else:
                again.append((src, dst))     # target not made yet, or dangling
        if len(again) == len(pending):
            break                            # nothing progressed: all dangling
        pending = again
    for src, dst in pending:
        sys.stderr.write("pkgtool: dangling symlink skipped: %s -> %s\n"
                         % (dst, src))
    return rc


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    what, rest = argv[1], argv[2:]
    if what == "meta" and len(rest) == 1:
        return cmd_meta(rest[0])
    if what == "list" and len(rest) == 1:
        return cmd_list(rest[0])
    if what == "extract" and len(rest) == 2:
        return cmd_extract(rest[0], rest[1])
    if what == "ubi" and len(rest) == 2:
        return cmd_ubi(rest[0], rest[1])
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv))
    except (OSError, tarfile.TarError, zipfile.BadZipFile) as e:
        die(str(e))
