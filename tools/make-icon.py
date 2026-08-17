#!/usr/bin/env python3
# Tadpole — turn a PNG logo into a Windows .ico.
#
#   tools/make-icon.py glasspole.png glasspole.ico
#
# WHY THE .ico IS COMMITTED rather than built. The Windows build already
# depends on mingw, NSIS and a downloaded CPython; making it also require
# Pillow — on the Linux box, only to convert an image that changes about twice
# a year — is a worse trade than a 60 KB binary in git. So this is run by hand
# when the logo changes, and tools/build-windows.sh just uses the result.
#
# WHY SEVERAL SIZES AND NOT ONE. Windows picks the entry closest to what it is
# drawing and scales if it has to, so a single 256px icon looks soft in the
# 16px taskbar and blurry at 32px on the desktop. Small sizes especially need
# their own downscale — a 16px icon produced by the shell from a 256px source
# is mush, whereas one resampled properly still reads as the logo.
#
# The 256 entry is upscaled from a 173px source. That is not ideal and is
# still worth having: without it Windows upscales the 128 itself and the
# result is the same or worse.

import os
import sys

SIZES = [16, 24, 32, 48, 64, 128, 256]


def main(argv):
    if len(argv) < 3:
        print("usage: make-icon.py <src.png> <dest.ico>", file=sys.stderr)
        return 2
    src, dest = argv[1], argv[2]
    try:
        from PIL import Image
    except ImportError:
        print("this needs Pillow (pip install pillow) — it is only used to\n"
              "regenerate the committed .ico, never during a build.",
              file=sys.stderr)
        return 1

    im = Image.open(src)
    if im.mode != "RGBA":
        im = im.convert("RGBA")

    # THE SOURCE MUST BE AT LEAST THE LARGEST ENTRY. Pillow's ICO writer
    # produces every size by resampling the image it is handed, and it SKIPS
    # any size bigger than that image rather than upscaling — so handing it a
    # 173px logo and asking for 256 silently yields a one-entry icon. Upscale
    # first, then ask.
    #
    # LANCZOS both ways. The logo is a soft-edged render, not pixel art, so the
    # smooth filter is right here — unlike the emulator's framebuffer
    # thumbnails, which use NEAREST for exactly the opposite reason.
    big = max(SIZES)
    if im.size != (big, big):
        im = im.resize((big, big), Image.LANCZOS)
    im.save(dest, format="ICO", sizes=[(s, s) for s in SIZES])
    print("%s -> %s  (%s, %d bytes)"
          % (src, dest, "/".join(str(s) for s in SIZES), os.path.getsize(dest)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
