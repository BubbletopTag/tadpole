"""Which of the shim's event nodes is which, read out of state.bin.

The shim serves /dev/input/event0.. in an order that depends on the device
family — the LeapPad2's keyboard is event1, the Didj's is event0 — and it
publishes each node's purpose in state.bin (ev_role[], appended after the
screen tail; see struct tadpole_state in tadpole/shim/tadpole_shim.c). Before
that field existed every tool here assumed the LeapPad2's order, which on a
Didj wrote button presses into its Power Button node.

    from evnode import ev_node, ROLE_KEYS
    n = ev_node("/tmp/tadpole-didj", ROLE_KEYS)     # -> 0 on a Didj, 1 on a LeapPad2

-1 means the running device has no such node. With no state.bin, or one from
a shim older than the field, the LeapPad2 order is assumed, as the viewer does.
"""
import os, struct

ROLE_NONE, ROLE_USB, ROLE_KEYS, ROLE_TOUCH, ROLE_TOUCH_RAW, ROLE_ACCEL, ROLE_POWER = range(7)
SLOTS = 8

# header (5 u32) + NUM_FB (3) layers of 15 u32 + screen, screen_seq, 64-byte
# PackageID. The same arithmetic fbshot.py does, and it must stay in step with
# the shim: the roles start immediately after the PackageID.
ROLE_OFFSET = 5 * 4 + 3 * 15 * 4 + 4 + 4 + 64

LF2000 = {ROLE_USB: 0, ROLE_KEYS: 1, ROLE_TOUCH: 2, ROLE_TOUCH_RAW: 3,
          ROLE_ACCEL: 4, ROLE_POWER: 5}


def roles(d):
    """The ev_role[] slots as published, or None if the file does not say."""
    try:
        with open(os.path.join(d, "state.bin"), "rb") as f:
            b = f.read()
    except OSError:
        return None
    if len(b) < ROLE_OFFSET + 4 * SLOTS:
        return None
    r = struct.unpack_from("<%dI" % SLOTS, b, ROLE_OFFSET)
    return r if any(r) else None


def ev_node(d, role):
    r = roles(d)
    if r is None:
        return LF2000.get(role, -1)
    for i, v in enumerate(r):
        if v == role:
            return i
    return -1
