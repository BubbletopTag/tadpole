#!/usr/bin/env python3
"""Press a button until the on-screen label matches a reference frame.

WHY THIS EXISTS. Leapster menus cannot be navigated by counting key presses.
The selection ring starts wherever the entry animation left it, so "down, down,
right, right, down" lands on Options in one run and Expansion Pack in the next.
Chasing that wasted several runs and looked like menu non-determinism.

The selected item's NAME is drawn on screen, so close the loop on it instead:
press, capture, compare the label region, repeat. Deterministic regardless of
where the ring began.

    ./tools/nav-label.py --ref shots/refs/options.png --key g:down
    ./tools/nav-label.py --ref r.png --key g:down --max 12 --then a
    ./tools/nav-label.py --ref r.png --keys g:right,g:down --max 16 --then a

--ref is any previously captured frame that shows the wanted label. Only the
label strip is compared, so the rest of the screen (animated bubbles, drifting
background) does not matter.
"""
import os, subprocess, sys, tempfile, zlib, struct

HERE = os.path.dirname(os.path.abspath(__file__))

# Label strip in FRAMEBUFFER coordinates. The game window is 320x240 at (15,17).
# Different screens draw the label at different heights — the main menu puts it
# around y=120, the Options screen around y=155 — so the box has to span both.
# With the narrow box, Controls and Credits differed by only 0.04 because it
# caught neither of them.
REGION = (15, 112, 320, 92)


def read_png(path):
    d = open(path, "rb").read()
    pos, idat, ct, w, h = 8, b"", None, 0, 0
    while pos < len(d):
        (ln,) = struct.unpack(">I", d[pos:pos + 4])
        t = d[pos + 4:pos + 8]
        if t == b"IHDR":
            w, h = struct.unpack(">II", d[pos + 8:pos + 16]); ct = d[pos + 17]
        elif t == b"IDAT":
            idat += d[pos + 8:pos + 8 + ln]
        pos += 12 + ln
    raw = zlib.decompress(idat)
    ch = 3 if ct == 2 else 4
    stride = w * ch
    prev = bytearray(stride)
    rows, i = [], 0
    for _ in range(h):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i + stride]); i += stride
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 255
        rows.append(bytes(line)); prev = line
    return w, h, rows, ch


def crop(path):
    """Per-column count of YELLOW label pixels.

    Comparing the raw strip does NOT work: the bubbles float and SpongeBob
    animates, so the correct label still scored ~22 against its own reference
    while wrong labels scored 29-42 — no usable threshold. The label text is a
    distinctive saturated yellow at a fixed height, so reduce to a signature of
    just those pixels. Measured separation then becomes 0.14 for the same label
    in a different frame versus 1.85-3.27 for a different label.
    """
    w, h, rows, ch = read_png(path)
    x0, y0, cw, chh = REGION
    sig = []
    for x in range(x0, min(x0 + cw, w)):
        n = 0
        for y in range(y0, min(y0 + chh, h)):
            r, g, b = rows[y][x * ch:x * ch + 3]
            if r > 190 and g > 150 and b < 130:
                n += 1
        sig.append(n)
    return sig


def diff(a, b):
    n = min(len(a), len(b))
    if not n:
        return 255.0
    return sum(abs(a[i] - b[i]) for i in range(n)) / float(n)


def shot(d, tmp):
    subprocess.run([os.path.join(HERE, "fbshot.py"), tmp, "-d", d],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return crop(tmp)


def press(d, key):
    if key.startswith("g:"):
        subprocess.run([os.path.join(HERE, "key.py"), "-g", key[2:], "-d", d],
                       stdout=subprocess.DEVNULL)
    else:
        subprocess.run([os.path.join(HERE, "key.py"), key, "-d", d],
                       stdout=subprocess.DEVNULL)


def main(argv):
    ref = key = then = None
    keys = None
    d = os.environ.get("TADPOLE_DIR", "/tmp/tadpole")
    mx, tol = 12, 0.6
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--ref":   ref = argv[i + 1]; i += 2
        elif a == "--key": key = argv[i + 1]; i += 2
        elif a == "--keys": keys = argv[i + 1].split(","); i += 2
        elif a == "--then": then = argv[i + 1]; i += 2
        elif a == "--max": mx = int(argv[i + 1]); i += 2
        elif a == "--tol": tol = float(argv[i + 1]); i += 2
        elif a == "-d":    d = argv[i + 1]; i += 2
        else: i += 1
    if keys is None:
        keys = [key] if key else None
    if not ref or not keys:
        sys.stderr.write(__doc__); return 2

    want = crop(ref)
    tmp = tempfile.mktemp(suffix=".png")
    try:
        cur = shot(d, tmp)
        for n in range(mx + 1):
            dd = diff(cur, want)
            print("  step %-2d label diff %.1f%s" % (n, dd, "  MATCH" if dd <= tol else ""))
            if dd <= tol:
                if then:
                    import time
                    # The confirming press is sometimes swallowed while the
                    # selection is still animating, and the run then ends on the
                    # menu instead of the screen we wanted. Press, check whether
                    # the label is still showing, and press again if so.
                    for attempt in range(3):
                        press(d, then)
                        time.sleep(4)
                        if diff(shot(d, tmp), want) > tol:
                            break
                        print("  confirm press %d did not take; retrying"
                              % (attempt + 1))
                return 0
            if n == mx:
                break
            # CYCLE through the directions. These menus are a 2D grid that does
            # NOT wrap, so repeating one direction parks the ring against an
            # edge and the label stops changing — 12 presses of the same key
            # left the selection on "Play" the whole time.
            press(d, keys[n % len(keys)])
            import time; time.sleep(2.5)
            cur = shot(d, tmp)
        sys.stderr.write("no match after %d presses\n" % mx)
        return 1
    finally:
        if os.path.exists(tmp):
            os.unlink(tmp)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
