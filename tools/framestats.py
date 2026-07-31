#!/usr/bin/env python3
"""Summarise a burst of Tadpole captures: content, colour variety, motion.

Built for judging 3D rendering, where the useful questions are not "is it black"
but "is anything being drawn", "does it look like a scene rather than flat fill",
and "does it CHANGE between frames". Trailing artefacts show up as high content
with low frame-to-frame change.
"""
import sys, os, glob, zlib, struct

def readpng(p):
    d = open(p, 'rb').read(); pos = 8; idat = b''; ct = None; w = h = 0
    while pos < len(d):
        ln, = struct.unpack(">I", d[pos:pos+4]); t = d[pos+4:pos+8]
        if t == b'IHDR': w, h = struct.unpack(">II", d[pos+8:pos+16]); ct = d[pos+17]
        elif t == b'IDAT': idat += d[pos+8:pos+8+ln]
        pos += 12 + ln
    raw = zlib.decompress(idat); ch = 3 if ct == 2 else 4
    st = w*ch; prev = bytearray(st); rows = []; i = 0
    for _ in range(h):
        f = raw[i]; i += 1; line = bytearray(raw[i:i+st]); i += st
        for x in range(st):
            a = line[x-ch] if x >= ch else 0
            b = prev[x]; c = prev[x-ch] if x >= ch else 0
            if f == 1: line[x] = (line[x]+a) & 255
            elif f == 2: line[x] = (line[x]+b) & 255
            elif f == 3: line[x] = (line[x]+(a+b)//2) & 255
            elif f == 4:
                pa, pb, pc = abs(b-c), abs(a-c), abs(a+b-2*c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x]+pr) & 255
        rows.append(bytes(line)); prev = line
    return w, h, rows, ch

def sample(path):
    w, h, rows, ch = readpng(path)
    sx, sy = w/480.0, h/272.0            # the window rect in panel coords
    px = []
    for y in range(int(17*sy), int(257*sy), 5):
        for x in range(int(15*sx), int(335*sx), 5):
            px.append(tuple(rows[y][x*ch:x*ch+3]))
    return px

def sample_raw(fbpath, stpath):
    """Sample the game window straight from the shared arena.

    Reading the raw framebuffer avoids decoding a PNG, and more importantly
    avoids ENCODING one during capture — doing that once a second starved the
    viewer and made the emulator look slow when the harness was at fault.
    """
    st = open(stpath, 'rb').read()
    w, h = struct.unpack_from("<2I", st, 8)
    FIELDS = 13
    L1 = struct.unpack_from("<%dI" % FIELDS, st, 20 + FIELDS*4)
    yoff = L1[5]
    vx, vy, vw, vh = L1[9], L1[10], L1[11], L1[12]
    if not vw: vx, vy, vw, vh = 0, 0, w, h
    arena = open(fbpath, 'rb').read()
    off = yoff * w * 4
    px = []
    for y in range(vy, min(vy+vh, h), 5):
        base = off + y*w*4
        for x in range(vx, min(vx+vw, w), 5):
            i = base + x*4
            if i + 3 <= len(arena):
                px.append((arena[i+2], arena[i+1], arena[i]))
    return px


def main(argv):
    if argv and argv[0] == "--raw":
        d = argv[1]
        fs = sorted(glob.glob(os.path.join(d, "f*.bin")))
        prev = None
        print("  %-6s %7s %7s %7s  %s" % ("sample", "lit%", "colours",
                                          "change%", "verdict"))
        for f in fs:
            stp = f.replace("/f", "/s")
            try: px = sample_raw(f, stp)
            except Exception as e: continue
            n = len(px)
            if not n: continue
            lit = sum(1 for p in px if max(p) > 24)
            cols = len(set(px))
            chg = -1 if prev is None or len(prev) != n else sum(
                1 for a, b in zip(px, prev)
                if abs(a[0]-b[0])+abs(a[1]-b[1])+abs(a[2]-b[2]) > 24)
            prev = px
            v = ("BLACK" if lit*100//n < 5 else "flat" if cols < 24 else
                 "static" if 0 <= chg*100//n < 2 else "scene")
            print("  %-6s %6d%% %7d %6s%%  %s" % (
                os.path.basename(f)[:-4], lit*100//n, cols,
                "n/a" if chg < 0 else chg*100//n, v))
        return 0
    files = sorted(glob.glob(os.path.join(argv[0], "t*.png")))
    prev = None
    print("  %-6s %7s %7s %7s  %s" % ("frame", "lit%", "colours", "change%", "verdict"))
    for f in files:
        try: px = sample(f)
        except Exception: continue
        n = len(px)
        lit = sum(1 for p in px if max(p) > 24)
        cols = len(set(px))
        if prev is None or len(prev) != n:
            chg = -1
        else:
            chg = sum(1 for a, b in zip(px, prev)
                      if abs(a[0]-b[0]) + abs(a[1]-b[1]) + abs(a[2]-b[2]) > 24)
        prev = px
        v = ("BLACK" if lit*100//n < 5 else
             "flat"  if cols < 24 else
             "static" if 0 <= chg*100//n < 2 else "scene")
        print("  %-6s %6d%% %7d %6s%%  %s" % (
            os.path.basename(f)[:-4], lit*100//n, cols,
            "n/a" if chg < 0 else chg*100//n, v))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
