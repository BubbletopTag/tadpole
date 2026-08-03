#!/usr/bin/env python3
"""Inject button presses into a running Tadpole guest, bypassing the viewer.

Companion to tap.py. Leapster titles are BUTTON driven, not touch driven — the
ViewFrame draws A/B/L/R around the picture and the menu carries a selection
ring — so navigating past a title's first screen needs keys, not taps.

    ./tools/key.py right right a        press right, right, then A
    ./tools/key.py -g down down a       GAME-space directions (see below)
    ./tools/key.py -d /tmp/x down a     another instance dir
    ./tools/key.py --hold 0.2 a         longer press

Codes are the Linux input codes the shim's gpio-keys device reports, and match
tadpole/viewer/tadpole_view.c exactly — keep the two in step.
"""
import os, struct, sys, time

EV_SYN, EV_KEY = 0x00, 0x01
SYN_REPORT = 0
EV_GPIO_KEYS = 1          # index of "gpio-keys" in the shim's device table

KEYS = {
    "esc": 1, "back": 1,
    "a": 30, "b": 48, "h": 35, "p": 25, "m": 50, "menu": 50, "home": 50,
    "up": 103, "left": 105, "right": 106, "down": 108,
    "voldown": 114, "volup": 115,
    # The ViewFrame's shoulder buttons: LeapPad2 reports them as H and P.
    "l": 35, "r": 25,
}

# THE D-PAD IS ROTATED IN LEAPSTER TITLES.
#
# The LeapPad2's D-pad codes assume the device is held PORTRAIT. A Leapster game
# is played turned on its side, so the physical "up" button is LEFT as far as the
# game is concerned. Ignoring this makes navigation look random rather than
# wrong: you press "down" four times, the ring moves sideways, and you conclude
# the menu is non-deterministic. It is not — the directions were simply wrong.
#
#     physical up -> game left, right -> up, down -> right, left -> down
#
# so to ASK for a game direction, send the physical button that produces it:
GAME_TO_PHYSICAL = {
    "left":  "up",
    "up":    "right",
    "right": "down",
    "down":  "left",
}


def ev(fd, type_, code, value):
    t = time.time()
    os.write(fd, struct.pack("<IIHHi", int(t), int((t % 1) * 1e6),
                             type_, code, value))


def press(fd, code, hold):
    ev(fd, EV_KEY, code, 1)
    ev(fd, EV_SYN, SYN_REPORT, 0)
    time.sleep(hold)
    ev(fd, EV_KEY, code, 0)
    ev(fd, EV_SYN, SYN_REPORT, 0)


def main(argv):
    d = os.environ.get("TADPOLE_DIR", "/tmp/tadpole")
    hold = 0.08
    gap = 0.25
    names = []
    game_space = False
    i = 0
    while i < len(argv):
        if argv[i] == "-d" and i + 1 < len(argv):
            d = argv[i + 1]; i += 2
        elif argv[i] == "--hold" and i + 1 < len(argv):
            hold = float(argv[i + 1]); i += 2
        elif argv[i] == "--gap" and i + 1 < len(argv):
            gap = float(argv[i + 1]); i += 2
        elif argv[i] in ("-g", "--game"):
            game_space = True; i += 1
        else:
            names.append(argv[i]); i += 1

    if not names:
        sys.stderr.write(__doc__)
        return 2
    # -g: the names given are GAME-space directions; rotate them to physical.
    if game_space:
        names = [GAME_TO_PHYSICAL.get(n.lower(), n) for n in names]
    bad = [n for n in names if n.lower() not in KEYS]
    if bad:
        sys.stderr.write("unknown key(s): %s\nknown: %s\n"
                         % (", ".join(bad), " ".join(sorted(KEYS))))
        return 2

    path = os.path.join(d, "ev%d" % EV_GPIO_KEYS)
    if not os.path.exists(path):
        sys.stderr.write("no %s — is a guest running?\n" % path)
        return 1
    # O_RDWR so the open never blocks waiting for a reader on the other end.
    fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
    try:
        for n in names:
            press(fd, KEYS[n.lower()], hold)
            print("key %s" % n)
            time.sleep(gap)
    finally:
        os.close(fd)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
