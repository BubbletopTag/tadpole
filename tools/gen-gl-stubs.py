#!/usr/bin/env python3
"""Regenerate tadpole_gles_stubs.c so no guest can hit an unresolved symbol.

    ./tools/gen-gl-stubs.py            report only
    ./tools/gen-gl-stubs.py --write    rewrite the stubs file

WHY THIS MATTERS MORE THAN IT USED TO. Our libGLESv1_CM.so is now also reached
under the name libopengles_lite.so, which is what native titles link. Before
that alias they fell through to the stock LeapFrog driver, which exports all 180
GL/EGL entry points — dead, but present, so they always LOADED. Against our
library a single missing entry point is fatal at load time:

    AppManager: can't resolve symbol 'glLightx'

Pet Pals 2 stopped showing its startup logo for exactly that reason. Completeness
of the symbol table is therefore a hard requirement, separate from whether any
given call does anything useful.

A stub is not a fix. It makes the title load and run; a call that needs real
behaviour still needs implementing in tadpole_gles_core.c. The report below
distinguishes the two so the difference stays visible instead of being quietly
papered over.
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
STUBS = os.path.join(PROJ, "tadpole", "shim", "tadpole_gles_stubs.c")
CORE = os.path.join(PROJ, "tadpole", "shim", "tadpole_gles_core.c")
EGL = os.path.join(PROJ, "tadpole", "shim", "tadpole_egl.c")

# Entry points that must do real work for 3D to be correct. Stubbing them keeps
# a title running but renders it wrong, so they are called out separately.
IMPORTANT = {
    "glFrustumf", "glFrustumx", "glFrustumfOES",
    "glTexEnvf", "glTexEnvi", "glTexEnviv",
    "glNormal3f", "glNormal3x",
    "glGetFloatv", "glGetBooleanv",
    "glLightx", "glLightfv", "glLightxv",
    "glMaterialf", "glMaterialfv",
    "glMultiTexCoord4f", "glMultiTexCoord4x",
    "glDepthRangex", "glClearDepthfOES",
    "glColor4f", "glCullFace", "glScissor", "glShadeModel",
    "glNormalPointer", "glReadPixels", "glTexEnvfv",
    "glPixelStorei", "glHint", "glFinish",
}


def device_symbols():
    """Every gl*/egl* entry point the real device's library exports."""
    for root in (os.path.expanduser("~/.local/share/tadpole/rootfs"),
                 os.path.join(PROJ, "rootfs")):
        for dirpath, _, files in os.walk(root):
            for f in files:
                if f.startswith("libGLESv1_CM.so"):
                    p = os.path.join(dirpath, f)
                    if os.path.islink(p):
                        continue
                    out = subprocess.run(
                        ["readelf", "--dyn-syms", "-W", p],
                        capture_output=True, text=True).stdout
                    syms = set()
                    for line in out.splitlines():
                        parts = line.split()
                        if len(parts) >= 8 and parts[3] == "FUNC" \
                                and parts[6] != "UND":
                            n = parts[7].split("@")[0]
                            if n.startswith(("gl", "egl")):
                                syms.add(n)
                    if syms:
                        return syms, p
    return set(), None


def defined_in(path):
    """Entry points with a real definition in a source file."""
    try:
        src = open(path, errors="replace").read()
    except OSError:
        return set()
    # A definition is a return type, the name, an argument list, then a brace.
    return set(re.findall(r"\b((?:gl|egl)[A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{", src))


def stub_names():
    return defined_in(STUBS)


def exported_by_us():
    """What our BUILT library actually exports — ground truth.

    The source-scanning regex above is only good enough to tell a real
    implementation from a stub; it misses definitions whose formatting it does
    not anticipate, and acting on that would emit a stub for something already
    defined and break the link with a duplicate symbol. The symbol table cannot
    be wrong about what exists.
    """
    lib = os.path.join(PROJ, "runtime", "shimlibs-gl", "libGLESv1_CM.so")
    if not os.path.exists(lib):
        return None
    out = subprocess.run(["readelf", "--dyn-syms", "-W", lib],
                         capture_output=True, text=True).stdout
    syms = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[3] == "FUNC" and parts[6] != "UND":
            n = parts[7].split("@")[0]
            if n.startswith(("gl", "egl")):
                syms.add(n)
    return syms


def main(argv):
    write = "--write" in argv
    dev, devpath = device_symbols()
    if not dev:
        sys.stderr.write("gen-gl-stubs: no device libGLESv1_CM.so found\n")
        return 1

    real = defined_in(CORE) | defined_in(EGL)
    stubs = stub_names()
    built = exported_by_us()
    if built is None:
        sys.stderr.write("gen-gl-stubs: build the GL shim first "
                         "(cd tadpole && make gl)\n")
        return 1
    # What EXISTS comes from the symbol table; the source scan only classifies
    # what exists into real-vs-stub.
    have = built | stubs
    real = real & built
    missing = sorted(dev - have)
    stubbed_important = sorted((stubs & IMPORTANT) - real)

    print("device exports        %d" % len(dev))
    print("  real implementations %d  (tadpole_gles_core.c, tadpole_egl.c)"
          % len(real & dev))
    print("  no-op stubs          %d" % len(stubs & dev))
    print("  MISSING ENTIRELY     %d  <- these fail to LOAD" % len(missing))
    print()
    if missing:
        print("missing:")
        for i in range(0, len(missing), 4):
            print("   " + "  ".join("%-28s" % m for m in missing[i:i + 4]))
        print()
    if stubbed_important:
        print("stubbed but NEEDED for correct rendering (a stub loads, then")
        print("draws the wrong thing — implement these in the core):")
        for i in range(0, len(stubbed_important), 3):
            print("   " + "  ".join("%-26s" % m
                                    for m in stubbed_important[i:i + 3]))
        print()

    if not write:
        print("(report only; pass --write to regenerate the stubs file)")
        return 0

    keep = sorted(stubs)
    body = keep + missing
    with open(STUBS, "w") as fh:
        fh.write(
            "/* Tadpole — GLES 1.x no-op stubs.\n"
            " *\n"
            " * GENERATED by tools/gen-gl-stubs.py — do not edit by hand.\n"
            " *\n"
            " * These exist so a guest can never hit an unresolved symbol. Since\n"
            " * our library is reached as libopengles_lite.so, which is the name\n"
            " * native titles link, a missing entry point is fatal at LOAD time:\n"
            " *     AppManager: can't resolve symbol 'glLightx'\n"
            " * Real implementations live in tadpole_gles_core.c and take\n"
            " * precedence; anything left here does nothing at all.\n"
            " */\n")
        for n in body:
            fh.write("void %s(void) { }\n" % n)
    print("wrote %s (%d entry points)" % (STUBS, len(body)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
