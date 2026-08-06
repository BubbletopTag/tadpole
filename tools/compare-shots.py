#!/usr/bin/env python3
"""Tadpole — diff two captures, and show the difference at a size you can see.

    compare-shots.py before.png after.png out.png [--crop X,Y,W,H] [--zoom N]

WHY. A rendering change is judged by looking at a 480x272 screenshot on a
1080p monitor, where it is a postage stamp, and "it looks a bit smoother" is
not a measurement. This prints what actually changed — how many pixels, by how
much — and writes a side-by-side blow-up of one region so the eye can check
what the numbers claim.

The blow-up is NEAREST-neighbour on purpose. Smooth scaling would introduce
exactly the kind of edge softening being evaluated, and the comparison would
flatter every option equally.

Reads and writes PNG with nothing but zlib, like the rest of the tooling here.
"""

import struct
import sys
import zlib


def png_read(path):
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("%s: not a PNG" % path)
    pos, idat, w, h, ct, depth = 8, b"", 0, 0, 0, 8
    while pos + 8 <= len(d):
        n = struct.unpack(">I", d[pos:pos + 4])[0]
        typ, chunk = d[pos + 4:pos + 8], d[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", chunk[:8])
            depth, ct = chunk[8], chunk[9]
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
        pos += 12 + n
    if depth != 8 or ct not in (2, 6):
        raise SystemExit("%s: need 8-bit RGB or RGBA" % path)
    ch = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * ch
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if f == 1:
            for i in range(ch, stride):
                line[i] = (line[i] + line[i - ch]) & 255
        elif f == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 255
        elif f == 3:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
        elif f == 4:
            for i in range(stride):
                a = line[i - ch] if i >= ch else 0
                c = prev[i - ch] if i >= ch else 0
                b = prev[i]
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    # normalise to RGB
    if ch == 4:
        rgb = bytearray(w * h * 3)
        for i in range(w * h):
            rgb[i*3:i*3+3] = out[i*4:i*4+3]
        return w, h, rgb
    return w, h, out


def png_write(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y*w*3:(y+1)*w*3]) for y in range(h))
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))
    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(raw, 9))
           + chunk(b"IEND", b""))
    open(path, "wb").write(png)


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    opts = dict(a[2:].split("=", 1) for a in argv[1:] if a.startswith("--") and "=" in a)
    for i, a in enumerate(argv):
        if a == "--crop":
            opts["crop"] = argv[i + 1]
        if a == "--zoom":
            opts["zoom"] = argv[i + 1]
    if len(args) < 3:
        sys.stderr.write(__doc__)
        return 2
    aw, ah, a = png_read(args[0])
    bw, bh, b = png_read(args[1])
    if (aw, ah) != (bw, bh):
        raise SystemExit("different sizes: %dx%d vs %dx%d" % (aw, ah, bw, bh))

    # ---- what changed ----
    diff = worst = 0
    total = 0
    for i in range(0, aw * ah * 3, 3):
        dr = abs(a[i] - b[i]); dg = abs(a[i+1] - b[i+1]); db = abs(a[i+2] - b[i+2])
        m = max(dr, dg, db)
        if m:
            diff += 1
            total += m
            if m > worst:
                worst = m
    px = aw * ah
    print("%dx%d  pixels differing: %d (%.1f%%)  mean delta %.2f  worst %d"
          % (aw, ah, diff, 100.0 * diff / px, (total / diff) if diff else 0, worst))

    # ---- show it ----
    zoom = int(opts.get("zoom", 4))
    if "crop" in opts:
        cx, cy, cw, chh = (int(v) for v in opts["crop"].split(","))
    else:
        cx, cy, cw, chh = 0, 0, aw, ah
    cx = max(0, min(cx, aw - 1)); cy = max(0, min(cy, ah - 1))
    cw = max(1, min(cw, aw - cx)); chh = max(1, min(chh, ah - cy))

    gap = 4
    ow, oh = cw * zoom * 2 + gap, chh * zoom
    out = bytearray(ow * oh * 3)
    for y in range(oh):
        for x in range(ow):
            if x < cw * zoom:
                src, sx = a, x
            elif x >= cw * zoom + gap:
                src, sx = b, x - cw * zoom - gap
            else:
                o = (y * ow + x) * 3
                out[o:o+3] = b"\xff\xff\xff"       # divider
                continue
            px_ = cx + sx // zoom
            py_ = cy + y // zoom
            s = (py_ * aw + px_) * 3
            o = (y * ow + x) * 3
            out[o:o+3] = src[s:s+3]
    png_write(args[2], ow, oh, out)
    print("wrote %s  (%dx%d, %dx zoom of %d,%d %dx%d — left=%s right=%s)"
          % (args[2], ow, oh, zoom, cx, cy, cw, chh, args[0], args[1]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
