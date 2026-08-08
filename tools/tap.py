#!/usr/bin/env python3
"""Inject a touch into a running Tadpole guest, bypassing the SDL viewer.

Writes straight into the shim's event FIFO, so what the guest receives is
exactly what we typed -- no window sizing, no HiDPI, no renderer scaling in the
way. That makes it the reference for deciding whether a touch bug is on the
VIEWER side (window -> framebuffer mapping) or the GUEST side (what Brio does
with the values it is handed).

    ./tools/tap.py 240 136              tap the centre of the screen
    ./tools/tap.py 240 136 -d /tmp/x    another instance dir
    ./tools/tap.py 100 50 --raw 1023    scale into a 0..1023 range first

Device ranges, from `evtest /dev/input/event2` on real hardware: ABS_X and
ABS_Y advertise min=1 max=1023, but the driver actually emits panel pixels
(X 2..482, Y 0..271). Pressure advertises 1..1023 and emits 10..70.
"""
import os, struct, sys, time

EV_SYN, EV_KEY, EV_ABS = 0x00, 0x01, 0x03
SYN_REPORT = 0
BTN_TOUCH = 0x14A
ABS_X, ABS_Y, ABS_PRESSURE = 0x00, 0x01, 0x18
EV_TOUCH = 2          # index of "touchscreen interface" in the shim's table
W, H = 480, 272


def ev(fd, type_, code, value):
    t = time.time()
    os.write(fd, struct.pack("<IIHHi", int(t), int((t % 1) * 1e6),
                             type_, code, value))


def tap(d, x, y, press=60, hold=0.8):
    """Hold matters. Flash Lite polls the touchscreen on its own frame tick,
    and under qemu that tick is much slower than on hardware. With a short
    hold the guest observes only the release -- you get ProcessMouseUp with no
    matching ProcessMouseDown, and nothing responds. 0.8s is comfortably
    longer than a poll interval on a loaded host."""
    fd = os.open(os.path.join(d, f"ev{EV_TOUCH}"), os.O_RDWR | os.O_NONBLOCK)
    try:
        ev(fd, EV_KEY, BTN_TOUCH, 1)
        # STREAM THE POSITION, DO NOT SEND IT ONCE.
        #
        # A real touchscreen reports continuously for as long as a finger is
        # down. One sample looks equivalent and is not, because tslib's chain
        # is made of filters with memory: `variance delta=30` and
        # `dejitter delta=100` (this device's /etc/ts.conf) both need a run of
        # samples before they will emit anything at all. Send one and the
        # whole tap is swallowed inside tslib — no error, no event, nothing
        # for the application to respond to.
        #
        # That is what made the Ultra look like it was ignoring touch after
        # tslib was already working: the plumbing was right and the tap was
        # too short-lived to survive the filters.
        n = max(1, int(hold / 0.02))
        for _ in range(n):
            ev(fd, EV_ABS, ABS_X, x)
            ev(fd, EV_ABS, ABS_Y, y)
            ev(fd, EV_ABS, ABS_PRESSURE, press)
            ev(fd, EV_SYN, SYN_REPORT, 0)
            time.sleep(0.02)
        ev(fd, EV_KEY, BTN_TOUCH, 0)
        ev(fd, EV_ABS, ABS_PRESSURE, 0)
        ev(fd, EV_SYN, SYN_REPORT, 0)
    finally:
        os.close(fd)


def main():
    d = os.environ.get("TADPOLE_DIR", "/tmp/tadpole")
    raw = 0
    hold = 0.8
    pos = []
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "-d":
            d = args[i + 1]; i += 2
        elif args[i] == "--raw":
            raw = int(args[i + 1]); i += 2
        elif args[i] == "--hold":
            hold = float(args[i + 1]); i += 2
        else:
            pos.append(int(args[i])); i += 1
    if len(pos) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    x, y = pos
    if raw:
        x = x * raw // (W - 1)
        y = y * raw // (H - 1)
    tap(d, x, y, hold=hold)
    print(f"tap ({x},{y}) -> {d}/ev{EV_TOUCH}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
