#!/usr/bin/env python3
"""Group the shim's crash reports by root cause.

    ./tools/crash-triage.py [crash.log ...]

Reads the reports written by tadpole_crash.c and clusters them by the faulting
library and offset, newest information first, naming the titles affected by
each.

WHY GROUPING IS THE POINT. Of the 112 installed packages, 79 are native Brio
apps built on the same engine, so a single fault in a shared library reproduces
across dozens of unrelated games. "Twelve games crash" and "one bug crashes
twelve games" call for completely different work, and only the grouping tells
you which one you have.

A cluster keyed on library+offset is the strong signal — same code, same place.
The per-library totals underneath catch the looser case where one library is
faulting in several spots.
"""
import os
import re
import sys
from collections import OrderedDict

SPLIT = "=== tadpole: guest crashed ==="


def find_sysroot():
    for c in (os.environ.get("TADPOLE_PROJECT"),
              os.path.expanduser("~/.local/share/tadpole"),
              os.path.dirname(os.path.dirname(os.path.abspath(__file__)))):
        if not c:
            continue
        p = os.path.join(c, "runtime", "sysroot", "LF", "Bulk", "ProgramFiles")
        if os.path.isdir(p):
            return p
    return None


def package_names(root):
    """PackageID -> human name, read from each package's meta.inf."""
    names = {}
    if not root:
        return names
    for pid in os.listdir(root):
        meta = os.path.join(root, pid, "meta.inf")
        try:
            with open(meta, "r", errors="replace") as fh:
                m = re.search(r'Name="([^"]*)"', fh.read())
            if m:
                names[pid] = m.group(1)
        except OSError:
            pass
    return names


def parse(text):
    """Yield one dict per crash report."""
    for chunk in text.split(SPLIT)[1:]:
        rec = {"stack": []}
        for line in chunk.splitlines():
            line = line.strip()
            if line.startswith("=== end"):
                break
            m = re.match(r"signal\s+(\S+)", line)
            if m:
                rec["signal"] = m.group(1)
                continue
            m = re.match(r"cwd\s+(\S+)", line)
            if m:
                rec["cwd"] = m.group(1)
                continue
            m = re.match(r"(fault|pc|lr)\s+(0x[0-9a-f]+)\s*(\S+)?", line)
            if m:
                rec[m.group(1)] = m.group(3) or m.group(2)
                continue
            m = re.match(r"\[sp\+\d+\]\s+0x[0-9a-f]+\s+(\S+)", line)
            if m:
                rec["stack"].append(m.group(1))
        if rec.get("pc"):
            yield rec


def main(argv):
    paths = argv or [
        os.path.join(os.environ.get("TADPOLE_DIR", "/tmp/tadpole"), "crash.log")]
    text = ""
    for p in paths:
        try:
            with open(p, "r", errors="replace") as fh:
                text += fh.read()
        except OSError as e:
            sys.stderr.write("crash-triage: %s\n" % e)
    if not text:
        return 1

    names = package_names(find_sysroot())
    recs = list(parse(text))
    if not recs:
        print("No crash reports found.")
        return 0

    def title(rec):
        pid = os.path.basename(rec.get("cwd", "") or "")
        return names.get(pid, pid or "<unknown>")

    clusters = OrderedDict()
    for r in recs:
        clusters.setdefault(r["pc"], []).append(r)

    print("%d crash report(s), %d distinct fault site(s)\n"
          % (len(recs), len(clusters)))

    for site, group in sorted(clusters.items(), key=lambda kv: -len(kv[1])):
        titles = sorted({title(r) for r in group})
        sigs = sorted({r.get("signal", "?") for r in group})
        print("%-44s %d crash(es)  %s" % (site, len(group), ",".join(sigs)))
        for t in titles:
            print("      %s" % t)
        # The immediate caller is what distinguishes "the same bug" from "the
        # same crash-handling code reached two different ways".
        callers = sorted({r.get("lr", "?") for r in group})
        if callers:
            print("      via %s" % ", ".join(callers[:4]))
        print()

    libs = OrderedDict()
    for r in recs:
        libs.setdefault(r["pc"].split("+")[0], set()).add(title(r))
    if len(libs) < len(clusters):
        print("By library:")
        for lib, ts in sorted(libs.items(), key=lambda kv: -len(kv[1])):
            print("  %-32s %d title(s)" % (lib, len(ts)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
