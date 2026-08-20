#!/usr/bin/env python3
"""Does everything that reads state.bin still agree on its layout?

    ./tools/tests/state_layout_test.py

THE BUG THIS EXISTS TO PREVENT is the one that keeps coming back: every
Leapster title suddenly renders at the full 480x272 panel instead of the
320x240 window its ViewFrame gives it. It has never once been a rendering
fault. state.bin carries that window, several separately-built binaries read
it, and when one of them disagrees about the layout it quietly falls back to
"use the whole panel".

The C side is down to ONE definition — tadpole/shim/tadpole_state.h, included
by the shim, the guest's GL library and the viewer — so the compiler keeps
those three honest, and tadpole-view --selftest-state proves the reader accepts
a file written by a different build.

What the compiler cannot see is the two Python tools, which decode the same
bytes with a hand-written field list. That is what this checks: the fields, in
order, and the sizes that follow from them. Anything added to the header shows
up here as a failure naming the file to update, rather than as a screenshot
that quietly comes out of the wrong page.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(os.path.dirname(HERE))
HDR = os.path.join(PROJ, "tadpole", "shim", "tadpole_state.h")

fails = []


def check(name, got, want):
    if got != want:
        fails.append("%s\n     got:  %r\n     want: %r" % (name, got, want))


def header_fields(text, struct):
    """The field names of one struct, in declaration order, comments stripped."""
    m = re.search(r"struct\s+%s\s*\{(.*?)\n\};" % struct, text, re.S)
    if not m:
        sys.exit("state_layout_test: no struct %s in %s" % (struct, HDR))
    body = re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S)
    names = []
    for line in body.split(";"):
        line = " ".join(line.split())
        if not line:
            continue
        # Strip the type, however it is spelled, and keep the declarator list.
        m2 = re.match(r"(?:struct\s+\w+|unsigned\s+int|unsigned|int|char)\s+(.*)$",
                      line)
        if not m2:
            continue
        for part in m2.group(1).split(","):
            part = part.strip().lstrip("*")
            if part:
                names.append(re.sub(r"\[.*\]$", "", part).strip())
    return names


def const(text, name):
    m = re.search(r"#define\s+%s\s+(\w+)" % name, text)
    return int(m.group(1), 0) if m else sys.exit("no %s in the header" % name)


def tool(path):
    """Import a tool by path — they are scripts, not modules on sys.path."""
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        os.path.basename(path)[:-3], os.path.join(PROJ, "tools", path))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


hdr = open(HDR).read()
layer = header_fields(hdr, "layer_state")
state = header_fields(hdr, "tadpole_state")
num_fb = const(hdr, "NUM_FB")
pkgid = const(hdr, "PKGID_MAX")

# The frozen part of the file. Growth is only ever allowed AFTER the layers —
# every reader locates layer[N] by arithmetic over what comes before it.
check("the header before the layers", state[:5],
      ["magic", "version", "width", "height", "vsync_count"])
check("the layers come next", state[5], "layer")
check("NUM_FB", num_fb, 3)

hdr_size = 5 * 4
layer_size = len(layer) * 4
tail_size = 4 + 4 + pkgid

# fbshot.py and burst.py decode these bytes by hand.
fb = tool("fbshot.py")
check("fbshot.py LAYER_FIELDS", list(fb.LAYER_FIELDS), layer)
check("fbshot.py LAYER_SIZE", fb.LAYER_SIZE, layer_size)
check("fbshot.py HDR_SIZE", fb.HDR_SIZE, hdr_size)
check("fbshot.py TAIL_SIZE", fb.TAIL_SIZE, tail_size)

bu = tool("burst.py")
check("burst.py LAYER_FIELDS", list(bu.LAYER_FIELDS), layer)
check("burst.py LAYER", bu.LAYER, layer_size)
check("burst.py HDR", bu.HDR, hdr_size)
check("burst.py TAIL", bu.TAIL, tail_size)

# And nobody may keep a private copy of the layout. Four hand-kept mirrors is
# what made this a recurring bug; the header is the only one now.
for rel in ("tadpole/shim/tadpole_shim.c", "tadpole/shim/tadpole_gles_core.c",
            "tadpole/viewer/tadpole_view.c"):
    src = open(os.path.join(PROJ, rel)).read()
    for struct in ("layer_state", "tadpole_state", "tad_layer_state", "tad_state"):
        if re.search(r"^struct\s+%s\s*\{" % struct, src, re.M):
            fails.append("%s declares its own struct %s — it must include "
                         "shim/tadpole_state.h instead" % (rel, struct))

# A reader that demands an exact size is the bug itself: a shim that appended a
# field at the end writes a LONGER file, which is legal and readable.
if not re.search(r"bytes\s*<\s*\(long\)sizeof\(struct tadpole_state\)", hdr):
    fails.append("tad_state_fault no longer accepts a longer state.bin — "
                 "only a SHORTER file is unreadable")

if fails:
    print("FAILED\n")
    for f in fails:
        print("  " + f)
    sys.exit(1)
print("PASS — %d layer fields, %d-byte layers, %d-byte state.bin; "
      "the header, fbshot.py and burst.py agree"
      % (len(layer), layer_size, hdr_size + num_fb * layer_size + tail_size))
