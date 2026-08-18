#!/usr/bin/env python3
"""Tadpole — hardlink the identical files shared by two installed firmwares.

    ./tools/dedupe-rootfs.py            report what could be saved
    ./tools/dedupe-rootfs.py --link     actually link them

WHY THIS EXISTS. Tadpole can hold several devices' system files at once (see
runtime/device.sh). An assembled sysroot costs almost nothing extra — it is
mostly symlinks into its own rootfs plus the handful of directories the guest
writes to — so the real cost of a second device is its extracted firmware under
rootfs/, and that is 100-300 MB a piece.

Much of it is the same bytes. The Leapster GS is a LeapPad2 with a smaller
screen: /usr/bin/app, AppManager and VideoDaemon are byte-identical, as are 30
of 31 Brio libraries and the entire OpenGL ES stack (docs/LEAPSTER-GS.md, which
measured it file by file). Two LeapPad firmware versions of the same device
overlap even more heavily.

WHY HARDLINKS AND NOT A SHARED STORE. The trees have to stay whole. Every part
of Tadpole — setup-sysroot.sh, install-firmware.sh, the guest resolver in
install-firmware.py — treats rootfs/<version>/ as a complete filesystem it can
walk, symlink into and re-root absolute links against. A content-addressed
store with symlinks pointing into it would break that in a way that surfaces
much later as "the guest cannot find its loader". A hardlink is invisible: the
tree is exactly as it was, one inode is shared, and deleting either copy is
still correct.

THE ONE HAZARD, AND IT IS WHY THIS IS NOT AUTOMATIC. Linked files share
storage, so a write through one path changes the other. rootfs/ is read-only by
construction — that is the whole reason setup-sysroot.sh assembles a separate
sysroot and copies meta.inf rather than symlinking it — but "by construction"
is a promise about today's code, and a future writer through a rootfs path
would corrupt the other device silently. So: opt in, dry-run by default, and
meta.inf is skipped outright because that IS a file something rewrites.
"""

import argparse
import hashlib
import os
import sys

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Never linked. meta.inf is rewritten in place by package-manager's
# RebuildPackageDatabase — the reason setup-sysroot.sh shadows it — and sharing
# one between two devices would let a boot of one blank the other's Device=
# field, which is what identifies it.
SKIP_NAMES = {"meta.inf"}

# Below this, an inode saves nothing worth having: the filesystem allocates a
# block either way, and the small files in these trees are configuration, which
# is exactly the part that differs between two devices anyway.
MIN_SIZE = 4096


def rootfs_trees(proj):
    """Every extracted firmware tree, the same three layouts device.sh knows."""
    out = []
    root = os.path.join(proj, "rootfs")
    if not os.path.isdir(root):
        return out
    for a in sorted(os.listdir(root)):
        p1 = os.path.join(root, a)
        if not os.path.isdir(p1):
            continue
        for name in ("emmc_rfs", "ubi_rfs"):
            if os.path.isdir(os.path.join(p1, name)):
                out.append(os.path.join(p1, name))
        for b in sorted(os.listdir(p1)):
            p2 = os.path.join(p1, b, "ubi_rfs")
            if os.path.isdir(p2):
                out.append(p2)
    return out


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.digest()


def human(n):
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024 or unit == "GB":
            return "%.1f %s" % (n, unit)
        n /= 1024.0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--link", action="store_true",
                    help="do it; without this nothing is changed")
    ap.add_argument("--min-size", type=int, default=MIN_SIZE,
                    help="ignore files smaller than this (default %d)" % MIN_SIZE)
    ap.add_argument("--proj", default=PROJ)
    a = ap.parse_args(argv[1:])

    trees = rootfs_trees(a.proj)
    if not trees:
        print("no firmware extracted under rootfs/ — nothing to do.")
        return 0
    # ONE TREE IS STILL WORTH SCANNING, which was a surprise: measured against
    # a stock LeapPad3 image, a single firmware carries about 16 MB of files
    # that are byte-identical to each other — the same library shipped in
    # /lib and /LF/Base/Brio/lib, and a dozen fonts and assets duplicated
    # between packages. Cross-device is the reason this tool exists, but there
    # is no reason to refuse the easy half of the saving.
    for t in trees:
        print("  tree: %s" % os.path.relpath(t, a.proj))

    # SIZE FIRST, HASH SECOND. Hashing every file in two 260 MB trees is a
    # minute of I/O; grouping by size first leaves only the handful of
    # candidates that could possibly match.
    by_size = {}
    for t in trees:
        for dirpath, _dirs, files in os.walk(t):
            for name in files:
                if name in SKIP_NAMES:
                    continue
                p = os.path.join(dirpath, name)
                if os.path.islink(p):
                    continue
                try:
                    st = os.lstat(p)
                except OSError:
                    continue
                if not os.path.isfile(p) or st.st_size < a.min_size:
                    continue
                by_size.setdefault(st.st_size, []).append((p, st))

    linked = saved = groups = 0
    for size, entries in sorted(by_size.items()):
        if len(entries) < 2:
            continue
        by_hash = {}
        for p, st in entries:
            try:
                by_hash.setdefault(digest(p), []).append((p, st))
            except OSError:
                continue
        for same in by_hash.values():
            if len(same) < 2:
                continue
            # ALREADY ONE INODE? Then this ran before, or the tree was copied
            # with cp -al. Nothing to do and nothing to report.
            inodes = {st.st_ino for _p, st in same}
            if len(inodes) < 2:
                continue
            groups += 1
            keep = same[0][0]
            for p, st in same[1:]:
                if st.st_ino == same[0][1].st_ino:
                    continue
                saved += size
                linked += 1
                if a.link:
                    tmp = p + ".dedupe-tmp"
                    try:
                        os.link(keep, tmp)
                        os.replace(tmp, p)
                    except OSError as e:
                        # A cross-device link is the expected failure: two
                        # trees on different filesystems cannot share an inode
                        # at all. Say so once and stop rather than reporting a
                        # saving that never happened.
                        if os.path.exists(tmp):
                            os.unlink(tmp)
                        print("cannot link %s: %s" % (p, e), file=sys.stderr)
                        return 1

    print()
    if a.link:
        print("linked %d files in %d groups, %s recovered"
              % (linked, groups, human(saved)))
    else:
        print("%d files in %d groups are byte-identical: %s could be recovered"
              % (linked, groups, human(saved)))
        print("Run again with --link to do it. rootfs/ is treated as read-only"
              " everywhere,")
        print("so sharing inodes across it is safe — see the note at the top of"
              " this file.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
