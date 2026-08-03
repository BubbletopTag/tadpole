#!/usr/bin/env python3
"""Restore execute permissions on an extracted LeapPad root filesystem.

    ./tools/fix-perms.py <rootfs dir>

WHY. ubireader_extract_files only preserves permissions with -k, which requires
root, so a normal extraction produces everything 0644 — including AppManager,
which then fails with a baffling "Exec format error" on a perfectly valid ARM
binary, and every .so, which fails as "can't load library".

Rather than demand root, mark what is genuinely executable: ELF files and
anything with a shebang. Checked against a known-good hand-extracted tree, that
reproduces 537 of its 538 executable files; the single miss is a meta.inf that
happens to carry +x and that nothing ever runs.

A SEPARATE FILE, not a heredoc. install-firmware.sh embedded this as
`python3 - "$dir" <<'EOPY'`, which hung: the script is being read from stdin
while the surrounding shell has its own claim on stdin, and the install stopped
dead at "restoring execute permissions". A plain script file has no such
ambiguity.
"""
import os
import stat
import sys


def main(argv):
    if len(argv) != 1:
        sys.stderr.write(__doc__)
        return 2
    root = argv[0]
    if not os.path.isdir(root):
        sys.stderr.write("fix-perms: not a directory: %s\n" % root)
        return 1

    marked = scanned = 0
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            try:
                if os.path.islink(path):
                    continue
                # REGULAR FILES ONLY. A root filesystem contains FIFOs and
                # device nodes, and open() on a FIFO BLOCKS FOREVER waiting for
                # a writer — which is what made this hang at "restoring execute
                # permissions" with no output and no way to tell it apart from
                # slow work. os.stat follows nothing here because islink was
                # already excluded.
                st = os.stat(path)
                if not stat.S_ISREG(st.st_mode):
                    continue
                scanned += 1
                # O_NONBLOCK as well, belt and braces: a device node that slips
                # through must not be able to block either.
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
                try:
                    head = os.read(fd, 4)
                finally:
                    os.close(fd)
                if head[:4] == b"\x7fELF" or head[:2] == b"#!":
                    os.chmod(path, st.st_mode | stat.S_IXUSR | stat.S_IXGRP
                                             | stat.S_IXOTH)
                    marked += 1
            except OSError:
                pass          # unreadable or vanished; nothing useful to do

    print("    %d executables marked (%d files scanned)" % (marked, scanned))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
