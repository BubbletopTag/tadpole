#!/usr/bin/env python3
"""Move the LeapTV's wand from a script, bypassing the SDL viewer.

Writes $TADPOLE_DIR/pointer.bin the way the viewer does from its mouse, so a
headless run can point and click. Coordinates are panel pixels (1280x720).

    ./tools/wand.py 640 360              point at the centre
    ./tools/wand.py 640 360 --click      ...and press A (left button) for 150 ms
    ./tools/wand.py 100 50 -d /tmp/x     another instance dir
"""
import argparse, os, struct, sys, time

MAGIC = 0x444E4157

def write(path, x, y, buttons, seq):
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o666)
    try:
        os.pwrite(fd, struct.pack("<8I", MAGIC, x, y, buttons, seq, 0, 0, 0), 0)
    finally:
        os.close(fd)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("x", type=int); ap.add_argument("y", type=int)
    ap.add_argument("-d", "--dir", default="/tmp/tadpole-leaptv")
    ap.add_argument("--click", action="store_true", help="press and release A")
    ap.add_argument("--button", type=int, default=1, help="1 left(A) 2 middle 4 right(B)")
    a = ap.parse_args()
    p = os.path.join(a.dir, "pointer.bin")
    seq = int(time.time()) & 0xffff
    write(p, a.x, a.y, 0, seq)
    if a.click:
        time.sleep(0.1)
        write(p, a.x, a.y, a.button, seq + 1)
        time.sleep(0.15)
        write(p, a.x, a.y, 0, seq + 2)
    print("wand at (%d,%d)%s -> %s" % (a.x, a.y, " clicked" if a.click else "", p))

if __name__ == "__main__":
    sys.exit(main())
