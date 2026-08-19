#!/usr/bin/env python3
"""Tadpole — read and unpack LeapFrog packages without unzip or bzip2.

    pkgtool.py meta    <archive>            first meta.inf, to stdout
    pkgtool.py list    <archive>            member names, one per line
    pkgtool.py extract <archive> <dir>      unpack it
    pkgtool.py ubi     <image.ubi> <dir>    UBIFS volume -> a directory tree
    pkgtool.py jffs2   <image.jffs2> <dir>  JFFS2 volume -> a directory tree

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
entry point once its console scripts are pruned out of the bundle. `jffs2` is
not a front end onto anything: it is implemented here, because the Didj is the
only device that needs it and the format costs less to read than a dependency
costs to carry — see the note above cmd_jffs2.

EXTRACTION IS PATH-CHECKED. A tar or zip member is free to name
"../../etc/whatever", and these archives come off the internet — refusing to
write outside the destination costs four lines here and is not something to
leave to chance.
"""

import bz2
import os
import struct
import sys
import tarfile
import zipfile
import zlib


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


# PERMISSIONS ARE IN THE ARCHIVE AND WERE BEING THROWN AWAY.
#
# Both formats record a Unix mode per member, and neither zipfile.extractall
# nor the loop below used to apply it: every extracted file arrived 0644 or
# whatever the umask said. On a LeapPad that was invisible, because packages
# there hold data and the one tree full of binaries — the rootfs — comes out of
# ubi_reader and gets its execute bits back from fix-perms.py.
#
# The Didj is where it shows. Its shell is a package file, bin/AppManager,
# 0755 in DIDJ-0x000E000A-000001.lfp, and extracting it without the mode
# produces a complete and correct install of a device that cannot start:
#
#     Error while loading ./Didj/Base/bin/AppManager: Exec format error
#
# on a perfectly good ARM binary. So the recorded mode is used when there is
# one, which is strictly better than guessing from content the way fix-perms.py
# has to for UBI.
#
# SETUID IS STRIPPED. These archives come off the internet, and nothing in a
# LeapFrog package has any business arriving setuid, setgid or sticky. The
# permission bits are honoured; the mode bits above them are not.
_MODE_MASK = 0o777


def _apply_mode(path, mode):
    if not mode:
        return
    try:
        os.chmod(path, mode & _MODE_MASK)
    except OSError:
        pass


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
                # create_system 3 is Unix; anything else (a ZIP written on
                # Windows, say) has no mode to read and keeps the default.
                if info.create_system == 3:
                    _apply_mode(out, info.external_attr >> 16)
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
                    _apply_mode(out, info.mode)
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



# ---- JFFS2 ------------------------------------------------------------------
#
# WHY THIS IS WRITTEN OUT BY HAND when the UBI path just calls ubi_reader.
#
# The Didj is the only device Tadpole knows whose root filesystem is JFFS2 —
# every LeapPad ships either a UBI volume or a plain tar. Reading it needed
# `jefferson`, which is a third-party package with its own dependency tree, and
# adding one to tools/fetch-deps.sh so that ONE device can be installed is a
# poor trade when the format's whole on-disk structure is two structs and the
# only compressor this image uses is in the standard library.
#
# MEASURED, NOT ASSUMED. DIDJ-0x000E0003-000001.lfp's erootfs.jffs2 is 4637
# nodes: 3911 inodes, 673 dirents, 53 summaries. Every compressed inode is
# JFFS2_COMPR_ZLIB (3352 of them) and the rest are stored (559). No LZO, no
# rtime, no rubin — so zlib, memcpy and a run of zeros cover the image
# completely, and anything else is refused loudly rather than filled with
# plausible garbage.
#
# THE FORMAT, as much of it as matters here. A JFFS2 volume is a bare log:
# nodes back to back, each starting with a 12-byte header of magic 0x1985, a
# type, and its total length, padded up to 4. There is no superblock and no
# index — you read every node and reconstruct.
#
#   DIRENT (0xE001)  binds a name in directory `pino` to inode `ino`.
#                    ino == 0 is an unlink. Highest `version` wins per name.
#   INODE  (0xE002)  one write: `dsize` bytes landing at `offset` in inode
#                    `ino`, plus the metadata the file has AT THAT VERSION.
#                    Replayed oldest-first, they rebuild the file.
#
# So a file's contents are its writes in version order, truncated to the isize
# the last one declares, and the tree is the surviving dirents walked from
# inode 1. Deleted files simply never get walked to.

JFFS2_MAGIC = 0x1985
JFFS2_NODETYPE_DIRENT = 0xE001
JFFS2_NODETYPE_INODE = 0xE002

# The compressors, from the kernel's jffs2/compr.h. Only NONE, ZERO and ZLIB
# appear in the Didj image; the rest are named so an unsupported one reports
# what it is instead of a number.
JFFS2_COMPR = {0: "none", 1: "zero", 2: "rtime", 3: "rubinmips",
               4: "copy", 5: "dynrubin", 6: "zlib", 7: "lzo"}

# DT_* from the dirent's `type` byte — the same values readdir() returns.
DT_FIFO, DT_CHR, DT_DIR, DT_BLK, DT_REG, DT_LNK, DT_SOCK = 1, 2, 4, 6, 8, 10, 12

_DIRENT = struct.Struct("<IIIIBB")            # pino version ino mctime nsize type
_INODE = struct.Struct("<IIIHHIIIIIIIBBHII")  # ino .. node_crc

# THE CRC IS NOT zlib's, AND THE DIFFERENCE IS ONE LINE OF INVERSIONS.
#
# JFFS2 calls crc32(0, buf, len) into mtd's own table loop, which has neither
# the initial nor the final xor with 0xFFFFFFFF that the standard CRC-32 — and
# therefore zlib.crc32 — applies. Undo both and the two agree. Checked against
# every one of the 4637 nodes in the Didj's erootfs.jffs2: all 4637 header
# CRCs, all 673 dirent CRCs, and all 3911 inode node and data CRCs verify.
#
# THAT CHECK IS ALSO HOW THE STRUCTS WERE CONFIRMED. A CRC that covers exactly
# the bytes up to a field only agrees if every field before it is where this
# code thinks it is, so passing on real data is a much stronger statement about
# the layout below than "the extraction looked plausible".
def _jffs2_crc(buf):
    return zlib.crc32(buf, 0xFFFFFFFF) ^ 0xFFFFFFFF


def _jffs2_scan(data):
    """Every live node in the image -> (dirents by parent, inodes by ino, bad).

    UNALIGNED GARBAGE IS EXPECTED, not exceptional. Erased flash reads as
    0xFF, a partly-written node can be followed by anything, and the image is
    padded out to the partition size — so a bad magic is skipped four bytes at
    a time rather than treated as the end of the volume. Nodes are 4-byte
    aligned, so stepping by four cannot walk past a good one.

    A HEADER CRC IS WHAT SEPARATES A NODE FROM FOUR BYTES THAT HAPPEN TO READ
    0x1985, which is the same test the kernel makes and the reason the scan
    can afford to be this trusting about where nodes start. Body CRCs are
    checked too and a node that fails one is dropped rather than believed:
    silently shortening a file is a worse outcome than saying so, and `bad`
    is reported by the caller.
    """
    dirents, inodes, bad = {}, {}, 0
    off, end = 0, len(data)
    while off + 12 <= end:
        magic, ntype, totlen = struct.unpack_from("<HHI", data, off)
        hdr_crc = struct.unpack_from("<I", data, off + 8)[0]
        if (magic != JFFS2_MAGIC or totlen < 12 or off + totlen > end
                or _jffs2_crc(data[off:off + 8]) != hdr_crc):
            off += 4
            continue
        step = (totlen + 3) & ~3
        if ntype == JFFS2_NODETYPE_DIRENT:
            # node_crc covers the struct up to its last two fields — 32 bytes
            # of a 40-byte dirent header, in the kernel's sizeof(*rd) - 8.
            if _jffs2_crc(data[off:off + 32]) != struct.unpack_from("<I", data, off + 32)[0]:
                bad += 1
                off += step
                continue
            pino, ver, ino, _mctime, nsize, dtype = _DIRENT.unpack_from(data, off + 12)
            name = data[off + 40:off + 40 + nsize].decode("utf-8", "replace")
            dirents.setdefault(pino, []).append((ver, ino, name, dtype))
        elif ntype == JFFS2_NODETYPE_INODE:
            # 60 bytes of a 68-byte inode header, again sizeof(*ri) - 8: the
            # data CRC and the node CRC are the two it does not cover.
            if _jffs2_crc(data[off:off + 60]) != struct.unpack_from("<I", data, off + 64)[0]:
                bad += 1
                off += step
                continue
            (ino, ver, mode, _uid, _gid, isize, _atime, mtime, _ctime,
             doff, csize, dsize, compr, _uc, _fl, data_crc, _nc) = \
                _INODE.unpack_from(data, off + 12)
            payload = data[off + 68:off + 68 + csize]
            if len(payload) != csize or _jffs2_crc(payload) != data_crc:
                bad += 1
                off += step
                continue
            inodes.setdefault(ino, []).append(
                (ver, mode, isize, mtime, doff, dsize, compr, payload))
        off += step
    return dirents, inodes, bad


def _jffs2_build(ino, nodes):
    """Replay one inode's writes -> (mode, mtime, contents).

    Oldest version first, each write landing at its own offset, because a
    later node may overwrite part of an earlier one — that is what makes this
    a log rather than a list of files. `isize` from the newest node is the
    real length: the last write can be a truncation, in which case the buffer
    is longer than the file.
    """
    buf = bytearray()
    mode = mtime = isize = 0
    for ver, m, sz, mt, doff, dsize, compr, payload in sorted(nodes):
        mode, mtime, isize = m, mt, sz
        if not dsize:
            continue
        if compr == 0:
            chunk = payload[:dsize]
        elif compr == 1:
            chunk = b"\0" * dsize
        elif compr == 6:
            chunk = zlib.decompress(payload)[:dsize]
        else:
            die("inode %d uses the %s compressor, which this reader does not "
                "implement" % (ino, JFFS2_COMPR.get(compr, "unknown (%d)" % compr)))
        if len(buf) < doff + dsize:
            buf.extend(b"\0" * (doff + dsize - len(buf)))
        buf[doff:doff + dsize] = chunk
    return mode, mtime, bytes(buf[:isize])


def cmd_jffs2(image, dest):
    """Extract a JFFS2 image into `dest`.

    Symlinks and device nodes are handled exactly as cmd_ubi handles them, and
    for the same reasons — see the long note there. In short: os.symlink needs
    a privilege Windows does not hand out, so failures are recorded and
    materialised as hard links afterwards; device nodes cannot be created by an
    unprivileged process on either platform and are not wanted, because the
    guest's /dev comes from the shim.
    """
    import shutil
    with open(image, "rb") as f:
        data = f.read()
    dirents, inodes, bad = _jffs2_scan(data)
    if not dirents:
        die("%s does not look like a JFFS2 image (no directory entries)" % image)
    if bad:
        sys.stderr.write(
            "pkgtool: %d node(s) in %s failed their checksum and were dropped "
            "- the extracted tree is INCOMPLETE; re-download the package\n"
            % (bad, os.path.basename(image)))

    # ONE ENTRY PER NAME, HIGHEST VERSION. A rename or a delete leaves the old
    # dirent in the log; taking them in file order would resurrect it.
    tree = {}
    for pino, ents in dirents.items():
        live = {}
        for ver, ino, name, dtype in ents:
            if name not in live or ver > live[name][0]:
                live[name] = (ver, ino, dtype)
        tree[pino] = {n: (i, t) for n, (_v, i, t) in live.items() if i}

    root = os.path.realpath(dest)
    os.makedirs(root, exist_ok=True)
    pending, devs, seen = [], [], set()

    def walk(pino, path):
        # A directory loop cannot happen in a valid image, but this reads
        # whatever it is handed; visiting each inode once bounds the walk.
        if pino in seen:
            return
        seen.add(pino)
        for name, (ino, dtype) in sorted(tree.get(pino, {}).items()):
            full = _safe_join(path, name)
            if full is None:            # a name that escapes the destination
                continue
            if dtype == DT_DIR:
                os.makedirs(full, exist_ok=True)
                walk(ino, full)
            elif dtype == DT_LNK:
                _mode, _mtime, target = _jffs2_build(ino, inodes.get(ino, []))
                try:
                    os.symlink(target.decode("utf-8", "replace"), full)
                except (OSError, NotImplementedError):
                    pending.append((target.decode("utf-8", "replace"), full))
            elif dtype == DT_REG:
                mode, mtime, blob = _jffs2_build(ino, inodes.get(ino, []))
                with open(full, "wb") as f:
                    f.write(blob)
                # MODE 0 MEANS "NO METADATA SEEN", NOT "NO PERMISSIONS". An
                # inode whose nodes all failed their checksums leaves nothing
                # to read a mode out of, and chmod(0) on the empty file that
                # results makes it unreadable even to the user who extracted
                # it — a confusing second failure on top of the real one,
                # which the checksum warning already reports.
                if mode & 0o777:
                    os.chmod(full, mode & 0o777)
                if mtime:
                    os.utime(full, (mtime, mtime))
            else:
                devs.append(full)       # char, block, fifo, socket

    walk(1, root)                       # inode 1 is the root directory

    if devs:
        sys.stderr.write(
            "pkgtool: %d device node(s) not created (%s and the rest) - the "
            "guest's /dev comes from the shim, so the extraction is complete "
            "without them\n" % (len(devs), os.path.basename(devs[0])))

    # Windows only: turn the recorded symlinks into hard links, repeatedly,
    # because a link's target may itself be a link a later pass creates.
    for _ in range(8):
        if not pending:
            break
        again = []
        for target, dst in pending:
            t = (os.path.join(root, target.lstrip("/\\")) if os.path.isabs(target)
                 else os.path.join(os.path.dirname(dst), target))
            if os.path.isdir(t):
                merge_tree(t, dst)
            elif os.path.isfile(t):
                try:
                    os.link(t, dst)
                except OSError:
                    shutil.copy2(t, dst)
            else:
                again.append((target, dst))
        if len(again) == len(pending):
            break
        pending = again
    for target, dst in pending:
        sys.stderr.write("pkgtool: dangling symlink skipped: %s -> %s\n"
                         % (dst, target))
    return 0

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
    if what == "jffs2" and len(rest) == 2:
        return cmd_jffs2(rest[0], rest[1])
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv))
    except (OSError, tarfile.TarError, zipfile.BadZipFile) as e:
        die(str(e))
