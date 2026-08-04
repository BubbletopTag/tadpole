#!/usr/bin/env python3
"""gl-demand.py — rank unimplemented GL entry points by how many titles need them.

    ./tools/gl-demand.py                  the work queue, most-needed first
    ./tools/gl-demand.py --title Prix     which entry points one title imports
    ./tools/gl-demand.py --unused         entry points NO installed title imports

WHY THIS EXISTS
---------------
"108 entry points are stubbed" was the number this project planned around for
months, and it is the wrong number. It ranks work by how much of the GLES
surface is missing, when what decides whether a title runs is which entry points
THAT TITLE calls. Measured: one Clam Prix race touches four stubs. Pet Pals 2
touches eight, and only one of those overlaps.

Runtime measurement (the stub hit counts in gl-warnings.log) gives depth — real
call counts, for the one title you just ran. This gives BREADTH: every installed
title, in about a second, with no emulator and no route file. A native Brio
title links libopengles_lite.so directly, so the gl* symbols it imports are
sitting in its own dynamic symbol table.

The two answer different questions and you want both:

    this script      "if I implement glLightx, how many titles does it unblock?"
    gl-warnings.log  "how badly does THIS title need it, and with what values?"

WHAT AN IMPORT DOES AND DOES NOT MEAN. An import is an upper bound: the title
links the symbol, so it may call it. It does not prove a call happens on any
particular screen — Clam Prix imports far more than a race exercises. Treat a
high import count as "worth implementing" and the runtime counter as "confirmed
in use".
"""
import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
CORE = os.path.join(PROJ, "tadpole", "shim", "tadpole_gles_core.c")
EGL = os.path.join(PROJ, "tadpole", "shim", "tadpole_egl.c")
STUBS = os.path.join(PROJ, "tadpole", "shim", "tadpole_gles_stubs.c")

# Where titles are installed. The project tree carries the stock widgets; the
# AppImage's data directory is where the real games land, so look in both and
# say which was used rather than silently reporting on twelve widgets.
ROOTS = [
    os.path.join(PROJ, "runtime", "sysroot", "LF", "Bulk"),
    os.path.expanduser("~/.local/share/tadpole/runtime/sysroot/LF/Bulk"),
]


def strip_noncode(src):
    """Comments and string literals out — see tools/gen-gl-stubs.py for why.

    Short version: the definition regex lets an argument list span newlines and
    prose contains no ';' or '{', so a comment mentioning a call can open a
    match that runs across the next real definition and hides it.
    """
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            i = n if j < 0 else j + 2
            out.append(" ")
        elif c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            out.append(" ")
        elif c in "\"'":
            q, i = c, i + 1
            while i < n and src[i] != q:
                i += 2 if src[i] == "\\" else 1
            i += 1
            out.append(" ")
        else:
            out.append(c)
            i += 1
    return "".join(out)


def defined_in(path):
    try:
        src = open(path, errors="replace").read()
    except OSError:
        return set()
    return set(re.findall(r"\b((?:gl|egl)[A-Za-z0-9_]*)\s*\([^;{]*\)\s*\{",
                          strip_noncode(src)))


def imports_of(path):
    """gl*/egl* symbols this ELF expects someone else to provide."""
    out = subprocess.run(["readelf", "--dyn-syms", "-W", path],
                         capture_output=True, text=True).stdout
    syms = set()
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 8 and f[6] == "UND":
            n = f[7].split("@")[0]
            if n.startswith(("gl", "egl")):
                syms.add(n)
    return syms


def find_titles():
    """(display name, path) for every native title object we can find."""
    titles, root_used = [], None
    for root in ROOTS:
        if not os.path.isdir(root):
            continue
        found = []
        for sub in ("ProgramFiles", "Downloads"):
            d = os.path.join(root, sub)
            if not os.path.isdir(d):
                continue
            for pkg in sorted(os.listdir(d)):
                pkgdir = os.path.join(d, pkg)
                meta = os.path.join(pkgdir, "meta.inf")
                name = pkg
                if os.path.isfile(meta):
                    txt = open(meta, errors="replace").read()
                    m = re.search(r'Name="([^"]*)"', txt)
                    if m:
                        name = m.group(1)
                for so in sorted(os.listdir(pkgdir)):
                    if so.endswith(".so"):
                        found.append((name, os.path.join(pkgdir, so)))
        if len(found) > len(titles):
            titles, root_used = found, root
    return titles, root_used


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", help="only titles whose name matches this substring")
    ap.add_argument("--unused", action="store_true",
                    help="list entry points no installed title imports")
    args = ap.parse_args()

    real = defined_in(CORE) | defined_in(EGL)
    stubs = defined_in(STUBS)

    titles, root = find_titles()
    if not titles:
        sys.exit("no titles found — looked in:\n  " + "\n  ".join(ROOTS))
    print(f"scanning {len(titles)} native objects under {root}\n")

    if args.title:
        want = args.title.lower()
        titles = [t for t in titles if want in t[0].lower()
                  or want in os.path.basename(os.path.dirname(t[1])).lower()]
        if not titles:
            sys.exit(f"no title matching {args.title!r}")

    # entry point -> set of title names importing it
    demand = {}
    for name, path in titles:
        for sym in imports_of(path):
            demand.setdefault(sym, set()).add(name)

    if args.title:
        for name, path in titles:
            syms = imports_of(path)
            missing = sorted(syms & stubs)
            print(f"{name}  ({os.path.basename(path)})")
            print(f"  imports {len(syms)} gl*/egl* entry points,"
                  f" {len(missing)} of them still stubs")
            for s in missing:
                print(f"    {s}")
            print()
        return 0

    if args.unused:
        never = sorted(s for s in stubs if s not in demand)
        print(f"=== {len(never)} stubs NO installed title imports ===")
        print("Implementing these buys nothing measurable. They exist only so the")
        print("symbol table is complete, which is a hard requirement on its own.\n")
        for i in range(0, len(never), 3):
            print("   " + "  ".join("%-30s" % s for s in never[i:i + 3]))
        return 0

    ranked = sorted(((len(v), k) for k, v in demand.items() if k in stubs),
                    reverse=True)
    print("=== STUBBED, AND IMPORTED BY INSTALLED TITLES ===")
    print("The work queue. Count is how many titles link the symbol, which is an")
    print("upper bound on need — confirm with a run and gl-warnings.log.\n")
    for count, sym in ranked:
        bar = "#" * min(count, 40)
        print(f"  {sym:<32} {count:>3}  {bar}")

    unimported = sum(1 for s in stubs if s not in demand)
    print(f"\n{len(ranked)} of {len(stubs)} stubs are imported by at least one"
          f" title; {unimported} by none (see --unused).")
    print(f"{len(real & set(demand))} entry points are implemented and in demand.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
