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
                "nonstd", "alpha", "blank", "win_x", "win_y", "win_w", "win_h",
                "vid_w", "vid_h")
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
    # Bottom-up. The video plane is not fixed at the bottom: MLC priority 0/1
    # put it above fb1. Must match the order in tadpole_view.c or a capture
    # disagrees with the screen, which is the one thing this script must never
    # do. See soc_dpc_set_vid_priority in the kernel's dpc.c.
    v = layers[2]
    order = ([1, 2, 0] if (v["nonstd"] >> 20) & 7 == 1
             and ((v["nonstd"] >> 24) & 3) <= 1 else [2, 1, 0])
    for i in order:
        L = layers[i]
        enabled, blank = L["enabled"], L["blank"]
        yoff, alpha = L["yoffset"], L["alpha"]
        if not enabled or blank:
            continue
        bpp = L["bpp"] or 32
        pitch = w * bpp // 8
        wx, wy, ww, wh = layer_window(L, w, h)
        # xoffset is part of the address — see the note in tadpole_view.c.
        off = yoff * pitch + L["xoffset"] * bpp // 8
        if off + pitch * wh > len(arena):
            off = 0
        if off + pitch * wh > len(arena):
            continue
        if (L["nonstd"] >> 20) & 7 == 1:      # LAYER_FORMAT_YUV420
            # The MLC video plane. Plane layout and the scaler are both the
            # driver's — see blit_layer_yuv420() in tadpole_view.c.
            sw = min(L["vid_w"] or ww, L["xres"]) or ww
            sh = min(L["vid_h"] or wh, L["yres"]) or wh
            for y in range(wh):
                sy = y if wh == sh else y * sh // wh
                yr = off + sy * pitch
                cb = off + (sy // 2) * pitch + pitch // 2
                cr = off + (sh // 2 + sy // 2) * pitch + pitch // 2
                o = ((wy + y) * w + wx) * 3
                for x in range(ww):
                    sx = x if ww == sw else x * sw // ww
                    Y = arena[yr + sx]
                    U = arena[cb + sx // 2] - 128
                    V = arena[cr + sx // 2] - 128
                    r = Y + ((91881 * V) >> 16)
                    g = Y - ((22554 * U + 46802 * V) >> 16)
                    b = Y + ((116130 * U) >> 16)
                    px[o + x * 3]     = 0 if r < 0 else (255 if r > 255 else r)
                    px[o + x * 3 + 1] = 0 if g < 0 else (255 if g > 255 else g)
                    px[o + x * 3 + 2] = 0 if b < 0 else (255 if b > 255 else b)
            drawn += 1
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


def layer_report(d, w, h, layers):
    """What is IN each layer, as opposed to what the composite makes of it.

    The composite answers "what does the screen look like", which is the wrong
    question when a layer is suspected of holding pixels in a format we decode
    wrongly — noise and black composite to "looks broken" either way. This
    reports the layer's own bytes: how much of it is non-zero, how many distinct
    values it holds, and the raw `nonstd` word, whose format field says whether
    the layer is RGB at all. fb2 is the MLC's video overlay and is the reason
    this exists.
    """
    with open(os.path.join(d, "fb0.bin"), "rb") as f:
        arena = f.read()
    for i, L in enumerate(layers):
        bpp = L["bpp"] or 32
        pitch = w * bpp // 8
        wx, wy, ww, wh = layer_window(L, w, h)
        off = L["yoffset"] * pitch + L["xoffset"] * bpp // 8
        note = ""
        if off + pitch * wh > len(arena):
            off, note = 0, " (yoffset past the arena, rewound to 0)"
        nz = tot = 0
        vals = set()
        if off + pitch * wh <= len(arena):
            step = max(1, wh // 64)          # sample rows; this runs per frame
            for y in range(0, wh, step):
                row = off + y * pitch
                seg = arena[row:row + ww * bpp // 8]
                tot += len(seg)
                nz += len(seg) - seg.count(0)
                vals.update(seg[::7])
        pct = (100 * nz // tot) if tot else 0
        # lf1000fb.h: LF1000_NONSTD_FORMAT=20, mask 0x7.
        # 0 = RGB, 1 = YUV420, 2 = YUV422.
        fmt = (L["nonstd"] >> 20) & 0x7
        print(f"  fb{i}: enabled={L['enabled']} blank={L['blank']} "
              f"{L['xres']}x{L['yres']} bpp={bpp} "
              f"off={L['xoffset']},{L['yoffset']} alpha={L['alpha']}")
        print(f"        win={ww}x{wh}+{wx}+{wy} nonstd=0x{L['nonstd']:08x} "
              f"(fmt={fmt} prio={(L['nonstd'] >> 24) & 3})")
        print(f"        bytes non-zero {pct}% of {tot}, {len(vals)} distinct"
              f"{note}")


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
    layers_only = False
    i = 0
    while i < len(args):
        if args[i] == "-d":
            d = args[i + 1]; i += 2
        elif args[i] == "--probe":
            # --probe X,Y,W,H,RRGGBB — "is that patch mostly that colour?"
            probe = args[i + 1]; i += 2
        elif args[i] == "--layers":
            # What each layer HOLDS, no PNG. See layer_report().
            layers_only = True; i += 1
        else:
            out = args[i]; i += 1

    magic, ver, w, h, vsync, layers = read_state(d)
    if magic != MAGIC:
        print(f"bad magic 0x{magic:08x} in {d}/state.bin", file=sys.stderr)
        return 1

    if layers_only:
        layer_report(d, w, h, layers)
        print(f"  panel {w}x{h}  vsync={vsync}")
        return 0

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
