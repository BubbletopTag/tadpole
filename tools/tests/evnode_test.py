#!/usr/bin/env python3
"""Does evnode.py find the keyboard where the shim says it is?

    ./tools/tests/evnode_test.py

One script, no framework, like its neighbours. The fixture is a state.bin
built here to the shim's layout: a 20-byte header, three 60-byte layers, the
72-byte screen tail, then eight u32 roles. The roles are the Didj's — keys,
power, USB — because that is the device whose order differs from the LeapPad2's
and the one where assuming the LeapPad2's sent every keypress to the Power
Button.
"""
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import evnode  # noqa: E402

MAGIC = 0x54414450


def state_bin(roles):
    b = struct.pack("<5I", MAGIC, 1, 320, 240, 0)
    b += bytes(3 * 60)                       # the layers: nothing here reads them
    b += struct.pack("<II", 0, 0) + bytes(64)  # screen, screen_seq, PackageID
    if roles is not None:
        b += struct.pack("<8I", *(list(roles) + [0] * (8 - len(roles))))
    return b


def check(cond, what):
    print("  %-62s %s" % (what, "ok" if cond else "FAIL"))
    return bool(cond)


def main():
    ok = True
    with tempfile.TemporaryDirectory() as d:
        # main's tadpole_state.h: the struct is "exactly 272" bytes before
        # anything appended, and the roles come first after that.
        ok &= check(evnode.ROLE_OFFSET == 272, "roles start at byte 272")

        with open(os.path.join(d, "state.bin"), "wb") as f:
            f.write(state_bin([evnode.ROLE_KEYS, evnode.ROLE_POWER, evnode.ROLE_USB]))
        ok &= check(evnode.ev_node(d, evnode.ROLE_KEYS) == 0, "Didj: keys are ev0")
        ok &= check(evnode.ev_node(d, evnode.ROLE_POWER) == 1, "Didj: power is ev1")
        ok &= check(evnode.ev_node(d, evnode.ROLE_TOUCH) == -1, "Didj: no touch node at all")

        lf2000 = [evnode.ROLE_USB, evnode.ROLE_KEYS, evnode.ROLE_TOUCH,
                  evnode.ROLE_TOUCH_RAW, evnode.ROLE_ACCEL, evnode.ROLE_POWER]
        with open(os.path.join(d, "state.bin"), "wb") as f:
            f.write(state_bin(lf2000))
        ok &= check(evnode.ev_node(d, evnode.ROLE_KEYS) == 1
                    and evnode.ev_node(d, evnode.ROLE_TOUCH) == 2
                    and evnode.ev_node(d, evnode.ROLE_POWER) == 5,
                    "LeapPad2: published order is the old fixed one")

        with open(os.path.join(d, "state.bin"), "wb") as f:
            f.write(state_bin(None))
        ok &= check(evnode.ev_node(d, evnode.ROLE_KEYS) == 1,
                    "a state.bin from before the field: LF2000 order assumed")

        with open(os.path.join(d, "state.bin"), "wb") as f:
            f.write(state_bin([]))
        ok &= check(evnode.ev_node(d, evnode.ROLE_TOUCH) == 2,
                    "the field present but all zero: LF2000 order assumed")

        os.remove(os.path.join(d, "state.bin"))
        ok &= check(evnode.ev_node(d, evnode.ROLE_KEYS) == 1,
                    "no guest at all: LF2000 order assumed")
    print("all ok" if ok else "FAILED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
