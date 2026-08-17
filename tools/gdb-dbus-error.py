"""Read the DBus::Error that libWireless throws, instead of inferring it.

Load into a gdb attached to a Tadpole guest (see tools/probe-wireless.sh). Every
theory about the CWirelessMPI crash so far has been reconstructed from the
outside -- who owns which bus name, what dbus-monitor did or did not show. But
dbus-c++ carries the failure in the thrown object itself: DBus::Error wraps
libdbus's DBusError, whose first two words are `const char *name` and
`const char *message`. At __cxa_throw, r0 points at that object. So the answer
is two pointer hops away and has been the whole time.

ONE CAUTION ABOUT dbus-monitor, which is why this file exists: it does not show
the bus daemon's own replies to calls addressed to the daemon -- Hello,
AddMatch, GetNameOwner. An error reply to any of those is invisible there, and
BrioWrapper's client stops right after a run of AddMatch calls. Reading the
exception sidesteps that blind spot entirely.

The layout walk is deliberately defensive rather than a typed cast. dbus-c++'s
Error holds `RefPtrI<InternalError> _int` after the vptr, and InternalError's
own layout differs across the 0.5/0.9 packagings -- the firmware's copy is
stripped, so gdb cannot tell us which. Rather than guess one and print garbage,
scan the first few words for anything that dereferences to a plausible C string.
"""
import gdb
import string

PRINTABLE = set(bytes(string.printable[:-5], "ascii"))
MAXSTR = 200


def _read(addr, n):
    try:
        return bytes(gdb.selected_inferior().read_memory(addr, n))
    except gdb.MemoryError:
        return None


def _word(addr):
    """One little-endian 32-bit word, or None if unmapped."""
    b = _read(addr, 4)
    return None if b is None else int.from_bytes(b, "little")


def _cstring(addr):
    """A NUL-terminated printable string at addr, or None.

    Deliberately strict: a run of bytes that merely happens to be printable is
    not evidence. Requires a terminator inside MAXSTR and at least four
    characters, which is enough to reject pointer noise while still catching
    short names like "Timeout".
    """
    if not addr or addr < 0x1000:
        return None
    out = bytearray()
    while len(out) < MAXSTR:
        b = _read(addr + len(out), 1)
        if b is None:
            return None
        if b[0] == 0:
            break
        if b[0] not in PRINTABLE:
            return None
        out += b
    else:
        return None
    return out.decode("ascii") if len(out) >= 4 else None


def _scan(addr, words, depth, seen, indent=""):
    """Print any string reachable from `addr` within `depth` pointer hops."""
    if depth < 0 or addr in seen:
        return
    seen.add(addr)
    for i in range(words):
        w = _word(addr + i * 4)
        if w is None:
            continue
        s = _cstring(w)
        if s:
            print("%s  +%-3d -> %#010x  %r" % (indent, i * 4, w, s))
        elif w > 0x1000 and depth > 0:
            # Not a string itself; it may be the struct that holds them.
            before = len(seen)
            _scan(w, 4, depth - 1, seen, indent + "    ")
            if len(seen) == before:
                pass


def dump_exception():
    """Print everything identifying about the exception now being thrown."""
    frame = gdb.selected_frame()
    try:
        r0 = int(frame.read_register("r0")) & 0xFFFFFFFF
        r1 = int(frame.read_register("r1")) & 0xFFFFFFFF
    except gdb.error as e:
        print("cannot read registers: %s" % e)
        return

    print("=" * 70)
    print("__cxa_throw  object=%#010x  type_info=%#010x" % (r0, r1))

    # type_info's name is at +4 in the std::type_info layout ARM uses.
    tname = _cstring(_word(r1 + 4) or 0)
    print("thrown type : %s" % (tname or "<unreadable>"))

    print("-" * 70)
    print("strings reachable from the exception object:")
    _scan(r0, 8, 2, set())

    print("-" * 70)
    print("backtrace:")
    try:
        print(gdb.execute("backtrace 25", to_string=True))
    except gdb.error as e:
        print("  (backtrace failed: %s)" % e)
    print("=" * 70)


class ThrowBreak(gdb.Breakpoint):
    """Stop on the throw and dump it, then let the program continue.

    Returning True from stop() keeps gdb stopped, which is what we want for the
    FIRST throw -- but Brio throws DBus::Error more than once on some paths, and
    only the one whose backtrace runs through CWirelessMPI matters. So dump
    every throw and stay stopped; the caller decides when it has seen enough.
    """

    def __init__(self):
        super(ThrowBreak, self).__init__("__cxa_throw", internal=False)
        self.hits = 0

    def stop(self):
        self.hits += 1
        print("\n### throw #%d" % self.hits)
        dump_exception()
        return True


ThrowBreak()
print("[gdb-dbus-error] armed on __cxa_throw")
