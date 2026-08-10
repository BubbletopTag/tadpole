#!/usr/bin/env python3
# Tadpole — turn a raw cartridge dump into an installable .tar.
#
#   tools/cart2tar.py cart.bin [more.bin ...] [-o DIR]
#
# WHAT A .bin IS AND WHERE IT COMES FROM
# --------------------------------------
# People back their cartridges up on the device itself, which is the one place
# that can read them:
#
#     dd if=/dev/mtdblock6 of=/LF/Bulk/cart.bin
#
# and then pull the file off over FTP. What you get is a byte-for-byte image of
# the cartridge's filesystem — mkdosfs, volume label "Cartridge", FAT32 on the
# 64 MB and 128 MB carts. It is a good backup: complete, verifiable, and it
# needs no tools on the device beyond what LeapFrog already shipped.
#
# Tadpole could not install one. It installs .tar, and there was no route from
# one to the other that did not involve mounting a loop device as root — which
# is not a thing on Windows at all.
#
# WHAT THIS PRODUCES. The cartridge's filesystem root, tarred verbatim:
#
#     BOO/meta.inf                 the title
#     BOO/cartLauncher/...
#     lib/meta.inf                 the cartridge's own libraries
#
# Nothing is rearranged, and that is deliberate. tools/install-game.py already
# understands an archive with several meta.inf in it — it installs each one to
# wherever its Type says it belongs — so the faithful copy is also the one that
# installs correctly. Rearranging it into a "cleaner" shape would mean deciding
# which package is the real one, and on Cars 2 that would silently drop three.
#
# The FAT reading is in tools/fatread.py, hand-written for the reason given
# there: every off-the-shelf way to read a FAT image is missing on Windows, on
# Linux, or both.

import argparse
import io
import os
import sys
import tarfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import fatread  # noqa: E402  — a sibling tool, not a package


def meta_name(fs):
    """The title's own name, out of the first meta.inf that has one.

    Only ever used to tell the user what they just converted. The output file
    is named after the input, because a dump called BOO.bin becoming
    "Monsters University.tar" is surprising when you are looking for it.
    """
    import re
    for path, is_dir, size, _mtime, read in fs.walk():
        if is_dir or os.path.basename(path).lower() != "meta.inf" or size > 65536:
            continue
        try:
            txt = read().decode("utf-8", "replace")
        except Exception:
            continue
        m = re.search(r'Name="([^"]*)"', txt)
        if m and m.group(1):
            return m.group(1)
    return None


def convert(src, outdir=None, quiet=False):
    base = os.path.splitext(os.path.basename(src))[0]
    dest = os.path.join(outdir or os.path.dirname(os.path.abspath(src)),
                        base + ".tar")

    def say(s):
        if not quiet:
            print(s, flush=True)

    try:
        fs = fatread.FatFS(src)
    except fatread.FatError as e:
        # THE COMMON MISTAKE IS AN .lf2, not a broken dump. Say which.
        print("  %s: %s" % (os.path.basename(src), e), file=sys.stderr)
        print("  A cartridge dump is a raw filesystem image, made on the "
              "device with\n    dd if=/dev/mtdblock6 of=/LF/Bulk/cart.bin\n"
              "  If this came from LFConnect it is already a package — "
              "install it directly.", file=sys.stderr)
        return None
    except OSError as e:
        print("  cannot read %s: %s" % (src, e), file=sys.stderr)
        return None

    with fs:
        say("==> %s  (FAT%d, label %r)" % (os.path.basename(src), fs.bits,
                                           fs.label or ""))
        title = meta_name(fs)
        if title:
            say("    %s" % title)

        n_files = n_dirs = 0
        total = 0
        tmp = dest + ".part"
        try:
            # No compression. These install straight after and the archive is
            # read more often than it is stored; gzip would also make the
            # progress figures meaningless.
            with tarfile.open(tmp, "w") as tar:
                for path, is_dir, size, mtime, read in fs.walk():
                    info = tarfile.TarInfo(path)
                    info.mtime = mtime or int(time.time())
                    if is_dir:
                        info.type = tarfile.DIRTYPE
                        info.mode = 0o755
                        tar.addfile(info)
                        n_dirs += 1
                        continue
                    data = read()
                    if len(data) != size:
                        # A short read means the chain ended early: the dump is
                        # truncated or the FAT is damaged. Keep what there is
                        # and say so — a partial cartridge still installs, and
                        # silence here would look like a Tadpole bug later.
                        say("    short: %s (%d of %d bytes)"
                            % (path, len(data), size))
                    info.size = len(data)
                    info.mode = 0o644
                    tar.addfile(info, io.BytesIO(data))
                    n_files += 1
                    total += len(data)
                    if not quiet and n_files % 200 == 0:
                        say("    %d files, %.1f MB" % (n_files, total / 1048576.0))
        except (OSError, fatread.FatError) as e:
            print("  failed: %s" % e, file=sys.stderr)
            try:
                os.remove(tmp)
            except OSError:
                pass
            return None

        # WRITTEN TO .part AND RENAMED, so an interrupted run cannot leave a
        # half-written .tar sitting in the games folder looking installable.
        try:
            if os.path.exists(dest):
                os.remove(dest)
            os.rename(tmp, dest)
        except OSError as e:
            print("  cannot write %s: %s" % (dest, e), file=sys.stderr)
            return None

        say("    %d files, %d directories, %.1f MB"
            % (n_files, n_dirs, total / 1048576.0))
        if n_files == 0:
            say("    nothing in it — is this the right image?")
        say("Wrote %s" % dest)
        return dest


def main(argv):
    ap = argparse.ArgumentParser(
        description="Convert a raw cartridge dump (.bin) to an installable .tar")
    ap.add_argument("images", nargs="+")
    ap.add_argument("-o", "--outdir", default=None,
                    help="where to write (default: beside the .bin)")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args(argv[1:])

    if args.outdir:
        try:
            os.makedirs(args.outdir, exist_ok=True)
        except OSError as e:
            print("cannot use %s: %s" % (args.outdir, e), file=sys.stderr)
            return 1

    ok = 0
    for src in args.images:
        if convert(src, args.outdir, args.quiet):
            ok += 1
    print()
    print("%d of %d converted." % (ok, len(args.images)))
    if ok:
        print("Install with:  Add a game, and pick the .tar")
    return 0 if ok == len(args.images) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
