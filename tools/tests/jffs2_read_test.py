#!/usr/bin/env python3
"""Does pkgtool's JFFS2 reader rebuild a filesystem the way the kernel would?

    ./tools/tests/jffs2_read_test.py

One script, no framework, the same shape as link_resolve_test.py — and, like
that one, it BUILDS ITS FIXTURE HERE. The only JFFS2 image Tadpole has ever
read is inside the Didj firmware, which is LeapFrog's and is not in the
repository; a test that needs a downloaded firmware to run is a test nobody
runs. So this writes JFFS2 images node by node and reads them back.

Writing them is also the point. The reader's understanding of the format is
only worth as much as the layout it assumes, and a fixture written from the
same assumptions would agree with anything. What pins it down is the CRC:
every node carries checksums over an exact byte range, this file computes them
from the kernel's struct definitions, and the reader recomputes them
independently. A field in the wrong place fails the checksum rather than
producing a plausible tree.

WHAT EACH CASE IS FOR, since none of them is arbitrary:

  * versions       a later node overwrites an earlier one. This is the whole
                   reason JFFS2 is a log and not a list, and reading nodes in
                   file order instead of version order gives the OLD contents.
  * fragments      one file written as several non-contiguous nodes.
  * truncation     the newest node's isize is shorter than the bytes written,
                   so the file is the declared length and not the buffer's.
  * deletion       an unlink is a dirent with ino 0, and the older dirent that
                   named the file is still in the log behind it.
  * compression    zlib and stored, the only two the Didj's image uses.
  * garbage        erased flash between nodes, and four bytes that read 0x1985
                   without being a node — the case the header CRC exists for.
"""
import importlib.util
import os
import shutil
import struct
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)

_spec = importlib.util.spec_from_file_location(
    "pkgtool", os.path.join(TOOLS, "pkgtool.py"))
pkgtool = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(pkgtool)

DIRENT, INODE = 0xE001, 0xE002
DT_DIR, DT_REG, DT_LNK = 4, 8, 10

fails = []


def check(what, got, want):
    if got != want:
        fails.append("%s\n     got  %r\n     want %r" % (what, got, want))


# ---- writing JFFS2 ---------------------------------------------------------
#
# crc32(0, buf, len) as mtd-utils computes it: the standard polynomial with
# neither the initial nor the final inversion. Written out independently of
# the reader's copy so that the two agreeing means something.
def crc(buf):
    return zlib.crc32(buf, 0xFFFFFFFF) ^ 0xFFFFFFFF


def pad4(n):
    return (n + 3) & ~3


def node(ntype, body):
    """A node with both header fields filled in. `body` starts after hdr_crc."""
    totlen = 12 + len(body)
    head = struct.pack("<HHI", 0x1985, ntype, totlen)
    out = head + struct.pack("<I", crc(head)) + body
    return out + b"\xff" * (pad4(totlen) - totlen)


def dirent(pino, version, ino, name, dtype):
    name = name.encode()
    body = struct.pack("<IIIIBBBB", pino, version, ino, 0, len(name), dtype, 0, 0)
    # node_crc covers the header up to itself: 32 bytes, the kernel's
    # sizeof(struct jffs2_raw_dirent) - 8.
    upto = struct.pack("<HHI", 0x1985, DIRENT, 12 + len(body) + 8 + len(name))
    upto += struct.pack("<I", crc(upto)) + body
    body += struct.pack("<II", crc(upto), crc(name)) + name
    totlen = 12 + len(body)
    head = struct.pack("<HHI", 0x1985, DIRENT, totlen)
    out = head + struct.pack("<I", crc(head)) + body
    return out + b"\xff" * (pad4(totlen) - totlen)


def inode(ino, version, mode, isize, offset, data, compr=0):
    if compr == 6:
        payload = zlib.compress(data)
    else:
        payload = data
    dsize = len(data)
    totlen = 68 + len(payload)
    fixed = struct.pack("<HHI", 0x1985, INODE, totlen)
    fixed += struct.pack("<I", crc(fixed))
    fixed += struct.pack("<IIIHHIIIIIIIBBH", ino, version, mode, 0, 0, isize,
                         0, 0, 0, offset, len(payload), dsize, compr, 0, 0)
    # 60 bytes so far; data_crc and node_crc are the two fields it excludes.
    out = fixed + struct.pack("<II", crc(payload), crc(fixed)) + payload
    return out + b"\xff" * (pad4(totlen) - totlen)


def image(*nodes):
    return b"".join(nodes)


def extract(blob):
    """Write `blob` to a file, extract it, -> the destination directory."""
    d = tempfile.mkdtemp(prefix="jffs2-test-")
    img = os.path.join(d, "test.jffs2")
    with open(img, "wb") as f:
        f.write(blob)
    out = os.path.join(d, "out")
    pkgtool.cmd_jffs2(img, out)
    return d, out


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    # ---- a whole small filesystem, in one image ---------------------------
    #
    # Inode 1 is the root. Everything below hangs off it, and the awkward
    # cases are deliberately interleaved rather than each given their own
    # image: the reader has to sort a single log correctly, not a tidy one.
    blob = image(
        # /plain — one node, stored
        dirent(1, 1, 10, "plain", DT_REG),
        inode(10, 1, 0o100644, 5, 0, b"hello"),

        # /rewritten — written, then overwritten in the middle by a LATER
        # version. In file order the old bytes come last.
        dirent(1, 1, 11, "rewritten", DT_REG),
        inode(11, 1, 0o100644, 12, 0, b"AAAAAAAAAAAA"),
        inode(11, 2, 0o100644, 12, 4, b"BBBB"),

        # ...and here is the same inode's FIRST version appearing after its
        # second in the log, which is what a garbage-collected volume looks
        # like. Sorting by version is the only thing that saves this.
        dirent(1, 1, 12, "outoforder", DT_REG),
        inode(12, 2, 0o100644, 4, 0, b"NEW!"),
        inode(12, 1, 0o100644, 4, 0, b"old."),

        # /fragmented — three nodes, one of them landing past a gap
        dirent(1, 1, 13, "fragmented", DT_REG),
        inode(13, 1, 0o100644, 10, 0, b"ab"),
        inode(13, 2, 0o100644, 10, 8, b"yz"),

        # /truncated — 20 bytes written, then a 3-byte isize
        dirent(1, 1, 14, "truncated", DT_REG),
        inode(14, 1, 0o100644, 20, 0, b"12345678901234567890"),
        inode(14, 2, 0o100644, 3, 0, b""),

        # /squashed — a zlib node, and one big enough that it really compresses
        dirent(1, 1, 15, "squashed", DT_REG),
        inode(15, 1, 0o100755, 4096, 0, b"Z" * 4096, compr=6),

        # /deleted — named, then unlinked by a higher-version dirent
        dirent(1, 1, 16, "deleted", DT_REG),
        inode(16, 1, 0o100644, 7, 0, b"vanish!"),
        dirent(1, 2, 0, "deleted", DT_REG),

        # /renamed — the old name is unlinked, the new one added, same inode
        dirent(1, 1, 17, "oldname", DT_REG),
        inode(17, 1, 0o100644, 4, 0, b"keep"),
        dirent(1, 2, 0, "oldname", DT_REG),
        dirent(1, 1, 17, "newname", DT_REG),

        # /sub/nested — a directory with a file in it
        dirent(1, 1, 18, "sub", DT_DIR),
        inode(18, 1, 0o40755, 0, 0, b""),
        dirent(18, 1, 19, "nested", DT_REG),
        inode(19, 1, 0o100644, 6, 0, b"inside"),

        # /link -> sub/nested. A symlink's target IS its file contents.
        dirent(1, 1, 20, "link", DT_LNK),
        inode(20, 1, 0o120777, 10, 0, b"sub/nested"),
    )

    tmp, out = extract(blob)
    try:
        p = lambda *a: os.path.join(out, *a)
        check("a stored file reads back", read(p("plain")), b"hello")
        check("a later node overwrites an earlier one",
              read(p("rewritten")), b"AAAABBBBAAAA")
        check("version order beats file order",
              read(p("outoforder")), b"NEW!")
        check("a gap between fragments reads as zeros",
              read(p("fragmented")), b"ab" + b"\0" * 6 + b"yz")
        check("isize truncates the rebuilt buffer",
              read(p("truncated")), b"123")
        check("a zlib node decompresses", read(p("squashed")), b"Z" * 4096)
        check("an unlinked name is gone", os.path.exists(p("deleted")), False)
        check("a renamed file keeps its contents", read(p("newname")), b"keep")
        check("the name it was renamed from is gone",
              os.path.exists(p("oldname")), False)
        check("a nested file reads back", read(p("sub", "nested")), b"inside")
        check("a symlink is made, not a file",
              os.path.islink(p("link")), True)
        check("a symlink's target is its contents",
              os.readlink(p("link")), "sub/nested")

        # PERMISSIONS COME OUT OF THE IMAGE. This is the reason
        # install-firmware.py does NOT run fix-perms.py over a JFFS2 tree: the
        # modes are real, and guessing them from file contents would replace a
        # known answer with an inferred one.
        check("a 0644 file is not executable",
              os.stat(p("plain")).st_mode & 0o777, 0o644)
        check("a 0755 file is executable",
              os.stat(p("squashed")).st_mode & 0o777, 0o755)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ---- garbage between nodes -------------------------------------------
    #
    # Erased flash, a run of zeros, and — the case that matters — four bytes
    # that read as the JFFS2 magic and are not a node. Without the header CRC
    # the scan would try to parse whatever followed.
    noise = (b"\xff" * 64 + b"\x00" * 32
             + struct.pack("<HHI", 0x1985, INODE, 0x7FFFFFFF) + b"\xde\xad\xbe\xef"
             + b"\xff" * 16)
    blob = image(
        noise,
        dirent(1, 1, 10, "survivor", DT_REG),
        noise,
        inode(10, 1, 0o100644, 8, 0, b"intact!!"),
        noise,
    )
    tmp, out = extract(blob)
    try:
        check("a file survives garbage on both sides",
              read(os.path.join(out, "survivor")), b"intact!!")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    # ---- a corrupted node is dropped, not believed ------------------------
    #
    # One byte flipped inside an inode's payload. The data CRC no longer
    # matches, so that write is refused: the file comes out as the zeros it
    # was never given rather than as silently wrong contents.
    good = inode(10, 1, 0o100644, 8, 0, b"intact!!")
    bad = bytearray(good)
    bad[-1] ^= 0xFF
    blob = image(dirent(1, 1, 10, "corrupt", DT_REG), bytes(bad))
    tmp, out = extract(blob)
    try:
        check("a node failing its data CRC contributes nothing",
              read(os.path.join(out, "corrupt")), b"")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if fails:
        print("FAIL (%d)" % len(fails))
        for f in fails:
            print("  " + f)
        return 1
    print("ok — JFFS2 reader")
    return 0


if __name__ == "__main__":
    sys.exit(main())
