#!/usr/bin/env python3
"""Sample the framebuffer rapidly and report how much consecutive frames differ.

    ./tools/burst.py [-n 12] [-i 0.25] [-d DIR] [-o OUTDIR]

WHAT IT ANSWERS. Standing still in a race, almost nothing should change between
frames — a timer digit, maybe an idle animation. If a large fraction of pixels
changes every frame while the camera is stationary, the renderer is unstable
rather than merely wrong, and that is a different bug from "the geometry is in
the wrong place". It is the measurable version of "the textures look like
they're rolling".

RAW BYTES, NOT PNGs. fbshot.py encodes PNG in pure Python, which is slow enough
to starve the viewer it is sampling — that has already corrupted a measurement
on this project once. Copying the visible page is a memcpy and perturbs nothing.

PER-LAYER, because the answer differs by layer: fb0 carries the 2D chrome
(steady), fb1 the 3D scene (the interesting one), and averaging them together
hides exactly the signal being looked for.
"""
import os
import struct
import sys
import time

NUM_FB = 3
HDR = 20                       # magic, version, width, height, vsync_count
LAYER_FIELDS = ("enabled", "xres", "yres", "bpp", "xoffset", "yoffset",
                "nonstd", "alpha", "blank", "win_x", "win_y", "win_w", "win_h",
                "vid_w", "vid_h")
LAYER = 4 * len(LAYER_FIELDS)
# The screen fields the shim publishes after the layers — screen, screen_seq
# and a 64-byte PackageID. Nothing here reads them; they are counted so the
# size check below still recognises a state.bin this script can read.
TAIL = 4 + 4 + 64


def read_state(d):
    with open(os.path.join(d, "state.bin"), "rb") as f:
        b = f.read()
    want = HDR + NUM_FB * LAYER + TAIL
    if len(b) != want:
        sys.stderr.write("burst: state.bin is %d bytes, expected %d — "
                         "LAYER_FIELDS is out of date with tadpole_shim.c\n"
                         % (len(b), want))
    magic, ver, w, h, vsync = struct.unpack_from("<5I", b, 0)
    layers = []
    for i in range(NUM_FB):
        vals = struct.unpack_from("<%dI" % len(LAYER_FIELDS), b, HDR + i * LAYER)
        layers.append(dict(zip(LAYER_FIELDS, vals)))
    return w, h, vsync, layers


def visible(fb, w, h, ls):
    """The bytes actually on screen for one layer, honouring its pan offset."""
    pitch = w * (ls["bpp"] or 32) // 8
    off = ls["yoffset"] * pitch
    end = off + pitch * h
    if end > len(fb):
        off, end = 0, pitch * h
    return fb[off:end]


def nonblack(a):
    """Fraction of sampled pixels with any colour in them.

    CHANGE ALONE IS AMBIGUOUS: a layer holding a correct but stationary image
    and a layer holding nothing both report 0% changed. Reporting content
    separately is what tells "the scene is static" apart from "the scene never
    arrived", and those have completely different causes.
    """
    n = len(a) // 4
    if n == 0:
        return 0.0
    step = 7
    seen = lit = 0
    for i in range(0, n, step):
        o = i * 4
        seen += 1
        if a[o:o + 3] != b"\x00\x00\x00":
            lit += 1
    return lit / seen if seen else 0.0


def diff(a, b):
    """Fraction of 32-bit pixels that differ. Sampled every 7th pixel: the
    question is 'roughly how much moved', and reading one word in seven answers
    it ~7x faster without changing the conclusion."""
    n = min(len(a), len(b)) // 4
    if n == 0:
        return 0.0, 0
    step = 7
    seen = changed = 0
    for i in range(0, n, step):
        o = i * 4
        seen += 1
        if a[o:o + 4] != b[o:o + 4]:
            changed += 1
    return (changed / seen if seen else 0.0), seen


def main(argv):
    d = os.environ.get("TADPOLE_DIR", "/tmp/tadpole")
    n, interval, outdir = 12, 0.25, None
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "-n":   i += 1; n = int(argv[i])
        elif a == "-i": i += 1; interval = float(argv[i])
        elif a == "-d": i += 1; d = argv[i]
        elif a == "-o": i += 1; outdir = argv[i]
        i += 1

    fb0 = os.path.join(d, "fb0.bin")
    if not os.path.exists(fb0):
        sys.stderr.write("burst: no %s\n" % fb0)
        return 1

    samples = []
    for k in range(n):
        try:
            w, h, vsync, layers = read_state(d)
            with open(fb0, "rb") as f:
                fb = f.read()
        except OSError as e:
            sys.stderr.write("burst: %s\n" % e)
            return 1
        samples.append((time.time(), vsync, w, h, layers, fb))
        time.sleep(interval)

    print("%d samples, %.2fs apart, panel %dx%d\n"
          % (len(samples), interval, samples[0][2], samples[0][3]))
    print("%-6s %-8s %s" % ("sample", "vsync", "  ".join(
        "fb%d chg/lit" % i for i in range(NUM_FB))))

    prev = None
    totals = [[] for _ in range(NUM_FB)]
    for k, (t, vsync, w, h, layers, fb) in enumerate(samples):
        cols = []
        for li in range(NUM_FB):
            ls = layers[li]
            if not ls["enabled"] or ls["blank"]:
                cols.append("    --   ")
                continue
            cur = visible(fb, w, h, ls)
            lit = nonblack(cur)
            if prev is None:
                cols.append("   . /%5.1f%%" % (lit * 100.0))
            else:
                pw, ph, players, pfb = prev
                pv = visible(pfb, pw, ph, players[li])
                frac, _ = diff(pv, cur)
                totals[li].append(frac)
                cols.append("%5.2f%%/%5.1f%%" % (frac * 100.0, lit * 100.0))
        print("%-6d %-8d %s" % (k, vsync, " ".join(cols)))
        prev = (w, h, layers, fb)

    print()
    for li in range(NUM_FB):
        if not totals[li]:
            continue
        avg = sum(totals[li]) / len(totals[li])
        mx = max(totals[li])
        print("fb%d: mean %.2f%% changed, peak %.2f%%" % (li, avg * 100, mx * 100))
    print("\nStanding still, a correct renderer changes a few percent at most "
          "(a timer digit,\nan idle animation). Tens of percent every frame "
          "means the frame is being rebuilt\ndifferently each time — instability, "
          "not misplacement.")

    if outdir:
        os.makedirs(outdir, exist_ok=True)
        for k, (t, vsync, w, h, layers, fb) in enumerate(samples):
            with open(os.path.join(outdir, "raw-%02d.bin" % k), "wb") as f:
                f.write(fb)
        print("\nraw arenas written to %s" % outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
