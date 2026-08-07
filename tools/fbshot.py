#!/usr/bin/env python3
"""Screenshot the Tadpole framebuffer without running the SDL viewer.

Composites the shared arena exactly the way tadpole/viewer/tadpole_view.c does
(fb2 -> fb1 -> fb0, per-layer yoffset into one arena) and writes a PNG. Lets us
drive and inspect the guest headlessly, which the SDL window cannot do from a
script.

    ./tools/fbshot.py out.png [-d /tmp/tadpole]
"""
import os, struct, sys, zlib

NUM_FB = 3
MAGIC = 0x54414450  # 'TADP'

# struct layer_state, in declaration order — MUST match tadpole_shim.c and
# tadpole_view.c. A field added there and not here silently shifts the stride,
# so every layer after the first reads garbage: the composite then picks the
# wrong page out of the arena and the capture comes out blank.
LAYER_FIELDS = ("enabled", "xres", "yres", "bpp", "xoffset", "yoffset",
                "nonstd", "alpha", "blank", "win_x", "win_y", "win_w", "win_h")
LAYER_SIZE = 4 * len(LAYER_FIELDS)
HDR_SIZE = 20                      # magic, version, width, height, vsync_count


def check_size(nbytes):
    """state.bin is exactly the header plus NUM_FB layers. Anything else means
    this script and the shim disagree about the struct."""
    want = HDR_SIZE + NUM_FB * LAYER_SIZE
    if nbytes != want:
        sys.stderr.write(
            "fbshot: state.bin is %d bytes, expected %d — LAYER_FIELDS is out "
            "of date with tadpole_shim.c\n" % (nbytes, want))


def read_state(d):
    with open(os.path.join(d, "state.bin"), "rb") as f:
        b = f.read()
    check_size(len(b))
    magic, ver, w, h, vsync = struct.unpack_from("<5I", b, 0)
    layers = []
    for i in range(NUM_FB):
        vals = struct.unpack_from("<%dI" % len(LAYER_FIELDS), b,
                                  HDR_SIZE + i * LAYER_SIZE)
        layers.append(dict(zip(LAYER_FIELDS, vals)))
    return magic, ver, w, h, vsync, layers


def layer_window(L, w, h):
    """Where this layer lands on the panel, and how much of it is picture.

    Must stay in step with layer_window() in tadpole_view.c — this script exists
    to composite the arena EXACTLY as the viewer does, and a capture that placed
    layers differently would quietly disagree with what is on screen.
    """
    x, y = L["win_x"], L["win_y"]
    cw, ch = L["win_w"], L["win_h"]
    if cw <= 0 or ch <= 0 or x < 0 or y < 0 or x + cw > w or y + ch > h:
        return 0, 0, w, h
    return x, y, cw, ch


def composite(d, w, h, layers):
    with open(os.path.join(d, "fb0.bin"), "rb") as f:
        arena = f.read()
    px = bytearray(w * h * 3)
    drawn = 0
    for i in range(NUM_FB - 1, -1, -1):
        L = layers[i]
        enabled, blank = L["enabled"], L["blank"]
        yoff, alpha = L["yoffset"], L["alpha"]
        if not enabled or blank:
            continue
        bpp = L["bpp"] or 32
        pitch = w * bpp // 8
        wx, wy, ww, wh = layer_window(L, w, h)
        off = yoff * pitch
        if off + pitch * wh > len(arena):
            off = 0
        if off + pitch * wh > len(arena):
            continue
        first = drawn == 0
        for y in range(wh):
            row = off + y * pitch
            o = ((wy + y) * w + wx) * 3
            if bpp == 16:
                for x in range(ww):
                    p = arena[row + x * 2] | (arena[row + x * 2 + 1] << 8)
                    px[o + x * 3] = ((p >> 11) & 0x1F) << 3
                    px[o + x * 3 + 1] = ((p >> 5) & 0x3F) << 2
                    px[o + x * 3 + 2] = (p & 0x1F) << 3
            else:
                for x in range(ww):
                    q = row + x * 4
                    a = arena[q + 3]
                    if first or a:
                        px[o + x * 3] = arena[q + 2]
                        px[o + x * 3 + 1] = arena[q + 1]
                        px[o + x * 3 + 2] = arena[q]
        drawn += 1
    return px


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + bytes(rgb[y * w * 3:(y + 1) * w * 3]) for y in range(h))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main():
    out = "fb.png"
    d = os.environ.get("TADPOLE_DIR", "/tmp/tadpole")
    args = sys.argv[1:]
    probe = None
    i = 0
    while i < len(args):
        if args[i] == "-d":
            d = args[i + 1]; i += 2
        elif args[i] == "--probe":
            # --probe X,Y,W,H,RRGGBB — "is that patch mostly that colour?"
            probe = args[i + 1]; i += 2
        else:
            out = args[i]; i += 1

    magic, ver, w, h, vsync, layers = read_state(d)
    if magic != MAGIC:
        print(f"bad magic 0x{magic:08x} in {d}/state.bin", file=sys.stderr)
        return 1

    if probe is not None:
        # ASKING THE SCREEN A QUESTION, for scripts that have to know what is
        # on it before deciding what to do next.
        #
        # This exists because the race harness waited on a LOG marker to decide
        # the home screen was ready — and the picker loads its icons BEHIND the
        # Connect nag, so the marker appeared while a full-screen dialog still
        # covered everything. Every later tap then landed on the dialog, and
        # the run failed a minute later looking like the emulator ignoring
        # input. The framebuffer knew the truth the whole time.
        px, py, pw, ph, want = probe.split(",")
        px, py, pw, ph = int(px), int(py), int(pw), int(ph)
        want = int(want, 16)
        wr, wg, wb = (want >> 16) & 255, (want >> 8) & 255, want & 255
        rgb = composite(d, w, h, layers)
        hit = tot = 0
        for y in range(max(0, py), min(h, py + ph)):
            row = y * w * 3
            for x in range(max(0, px), min(w, px + pw)):
                o = row + x * 3
                tot += 1
                if (abs(rgb[o] - wr) < 48 and abs(rgb[o+1] - wg) < 48
                        and abs(rgb[o+2] - wb) < 48):
                    hit += 1
        pct = (100 * hit // tot) if tot else 0
        print(f"probe {px},{py} {pw}x{ph} vs {want:06X}: {pct}% match")
        return 0 if pct >= 25 else 1

    for i, L in enumerate(layers):
        print(f"  fb{i}: enabled={L['enabled']} {L['xres']}x{L['yres']} "
              f"bpp={L['bpp']} yoff={L['yoffset']} alpha={L['alpha']} "
              f"blank={L['blank']}  win={L['win_w']}x{L['win_h']}"
              f"+{L['win_x']}+{L['win_y']}")
    write_png(out, w, h, composite(d, w, h, layers))
    print(f"{out}  {w}x{h}  vsync={vsync}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
