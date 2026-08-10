#!/usr/bin/env python3
# Tadpole — remove the installed system files, returning to a first-run state.
#
#   tools/erase-firmware.py [--really-delete]
#
# This is tools/erase-firmware.sh as one program, for the same reason
# install-firmware.py exists: Windows has no shell to run the .sh. The shell
# version remains the Linux entry point and is unchanged.
#
# For testing the setup wizard, which is otherwise awkward to reach once an
# install works — and, on Windows especially, for backing out of a half-done
# install without deleting anything you cannot get back.
#
# IT MOVES RATHER THAN DELETES, by default.
#
# Re-installing needs the firmware packages AND ubi_reader, and if either is
# missing a genuine delete leaves you unable to run anything until you sort
# that out. A rename costs nothing and is reversible, so the destructive
# version is opt-in.
#
# Never touches: games/ (your cartridge backups), sources/ (your firmware
# downloads), or anything you built.

import errno
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.environ.get("TADPOLE_PROJECT") or os.path.dirname(HERE)


def rel(path):
    try:
        return os.path.relpath(path, PROJ)
    except ValueError:                      # different drive on Windows
        return path


def main(argv):
    hard = "--really-delete" in argv[1:]
    backup = os.path.join(PROJ, ".erased-" + time.strftime("%Y%m%d-%H%M%S"))
    moved = 0

    def takeaway(path):
        nonlocal moved
        if not os.path.exists(path):
            return
        if hard:
            shutil.rmtree(path, ignore_errors=True)
            print("  deleted  %s" % rel(path))
        else:
            # PRESERVE THE RELATIVE PATH inside the backup, so restoring is a
            # single copy. Flattening everything into one directory loses where
            # each piece came from — rootfs/<ver> and runtime/sysroot land side
            # by side and the restore command becomes a guess.
            dest = os.path.join(backup, os.path.dirname(rel(path)))
            os.makedirs(dest, exist_ok=True)
            try:
                shutil.move(path, os.path.join(dest, os.path.basename(path)))
            except OSError as e:
                # A file still open by a running guest cannot be renamed on
                # Windows, and it is the common way this fails. Say which one.
                print("  cannot move %s: %s" % (rel(path), e.strerror or e),
                      file=sys.stderr)
                if e.errno in (errno.EACCES, errno.EPERM):
                    print("  close Tadpole and any running title, then retry.",
                          file=sys.stderr)
                return
            print("  moved    %s" % rel(path))
        moved += 1

    print("Erasing installed system files...")

    # The extracted firmware.
    rootfs = os.path.join(PROJ, "rootfs")
    if os.path.isdir(rootfs):
        for name in sorted(os.listdir(rootfs)):
            d = os.path.join(rootfs, name)
            if os.path.isdir(d):
                takeaway(d)

    # The sysroot is GENERATED — links into rootfs plus the writable
    # directories the guest needs. Removing it is safe and it is rebuilt.
    takeaway(os.path.join(PROJ, "runtime", "sysroot"))

    if moved == 0:
        print("  nothing installed — already at a first-run state")
        return 0

    print()
    if hard:
        print("Deleted. Re-install with:  tools/install-firmware.py <LFC_Downloads>")
    else:
        print("Moved to: %s" % rel(backup))
        print("Restore by copying its contents back over the project directory.")
        print("Or re-install:  tools/install-firmware.py <LFC_Downloads>")
    print("Tadpole will show the setup wizard on next launch.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
