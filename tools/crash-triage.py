#!/usr/bin/env python3
"""Group the shim's crash reports by root cause, with function names.

    ./tools/crash-triage.py                    everything kept so far
    ./tools/crash-triage.py --last             only the most recent run
    ./tools/crash-triage.py crash.log ...      specific files
    ./tools/crash-triage.py --raw              skip symbolisation (fast)

Reads the reports written by tadpole_crash.c and clusters them by faulting
library and offset, naming the titles affected by each and resolving the
offsets to functions.

WHY GROUPING IS THE POINT. Of the 112 installed packages, 79 are native Brio
apps built on the same engine, so a single fault in a shared library reproduces
across dozens of unrelated games. "Twelve games crash" and "one bug crashes
twelve games" call for completely different work, and only the grouping tells
you which one you have.

A cluster keyed on library+offset is the strong signal — same code, same place.
The per-library totals underneath catch the looser case where one library is
faulting in several spots.

WHERE THE REPORTS ARE. Under ~/.local/state/tadpole/crashes/<date>/, one
directory per run, written there by the shim because tadpole.sh puts the path
in TADPOLE_CRASHDIR. They used to go to $TADPOLE_DIR/crash.log, which is under
/tmp and which every probe script deletes before it starts — so a crash was
observable only if you looked before the next boot, and two could never be
compared. That path is still read, for a guest launched by hand.
"""
import os
import re
import sys
from collections import OrderedDict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from elfsyms import Symbols, demangle          # noqa: E402

SPLIT_CRASH = "=== tadpole: guest crashed ==="
SPLIT_ANY = re.compile(r"=== tadpole: guest (crashed|stack dump) ===")

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def state_root():
    base = os.environ.get("XDG_STATE_HOME") or os.path.expanduser("~/.local/state")
    return os.path.join(base, "tadpole", "crashes")


def default_inputs(last_only):
    """Every crash.log we have kept, oldest run first."""
    root = state_root()
    runs = []
    if os.path.isdir(root):
        for d in sorted(os.listdir(root)):
            p = os.path.join(root, d, "crash.log")
            if os.path.isfile(p):
                runs.append(p)
    if last_only:
        runs = runs[-1:]
    # The pre-TADPOLE_CRASHDIR location, for a guest started by hand.
    legacy = os.path.join(os.environ.get("TADPOLE_DIR", "/tmp/tadpole"), "crash.log")
    if not last_only and os.path.isfile(legacy):
        runs.append(legacy)
    return runs


def sysroot_dirs():
    """Where guest objects live, most specific first."""
    out = []
    for c in (os.environ.get("TADPOLE_PROJECT"),
              os.path.expanduser("~/.local/share/tadpole"), PROJ):
        if not c:
            continue
        for sub in (("runtime", "sysroot", "LF", "Bulk", "ProgramFiles"),
                    ("runtime", "libs")):
            p = os.path.join(c, *sub)
            if os.path.isdir(p):
                out.append(p)
    return out


def package_names(roots):
    """PackageID -> human name, read from each package's meta.inf."""
    names = {}
    for root in roots:
        if not root.endswith("ProgramFiles"):
            continue
        for pid in os.listdir(root):
            meta = os.path.join(root, pid, "meta.inf")
            try:
                with open(meta, "r", errors="replace") as fh:
                    m = re.search(r'Name="([^"]*)"', fh.read())
                if m:
                    names.setdefault(pid, m.group(1))
            except OSError:
                pass
    return names


class Resolver(object):
    """library basename + the crashing title -> Symbols, cached.

    The title matters: every native game's binary is called App.so, so an
    offset only means anything once you know WHICH App.so. The report's cwd
    line carries that, because AppManager chdir()s into the package directory
    before handing control over.
    """

    def __init__(self, roots):
        self.roots = roots
        self._cache = {}

    def _find(self, lib, cwd):
        pid = os.path.basename(cwd.rstrip("/")) if cwd else ""
        cands = []
        for r in self.roots:
            if pid:
                cands.append(os.path.join(r, pid, lib))
            cands.append(os.path.join(r, lib))
        for r in self.roots:
            for dirpath, _d, files in os.walk(r):
                if lib in files:
                    cands.append(os.path.join(dirpath, lib))
                    break
        for c in cands:
            if os.path.isfile(c):
                return c
        return None

    def lookup(self, lib, off, cwd):
        key = (lib, os.path.basename((cwd or "").rstrip("/")))
        if key not in self._cache:
            path = self._find(lib, cwd)
            self._cache[key] = Symbols(path) if path else None
        syms = self._cache[key]
        if not syms:
            return None
        hit = syms.lookup(off)
        if not hit:
            return None
        name, delta = hit
        return "%s+0x%x" % (demangle(name), delta) if delta else demangle(name)


def parse(text, source):
    """Yield one dict per crash report."""
    parts = SPLIT_ANY.split(text)
    # split() with one group gives [pre, kind, body, kind, body, ...]
    for i in range(1, len(parts) - 1, 2):
        kind, chunk = parts[i], parts[i + 1]
        rec = {"stack": [], "kind": kind, "source": source}
        for line in chunk.splitlines():
            line = line.strip()
            if line.startswith("=== end"):
                break
            m = re.match(r"signal\s+(\S+)", line)
            if m:
                rec["signal"] = m.group(1); continue
            m = re.match(r"cwd\s+(\S+)", line)
            if m:
                rec["cwd"] = m.group(1); continue
            m = re.match(r"alive\s+(\d+)s", line)
            if m:
                rec["alive"] = int(m.group(1)); continue
            m = re.match(r"(fault|pc|lr)\s+(0x[0-9a-f]+)\s*(\S+)?", line)
            if m:
                rec[m.group(1)] = m.group(3) or m.group(2); continue
            m = re.match(r"\[sp\+\d+\]\s+0x[0-9a-f]+\s+(\S+)", line)
            if m:
                rec["stack"].append(m.group(1))
        if rec.get("pc"):
            yield rec


def split_site(site):
    """'App.so+0x1234' -> ('App.so', 0x1234); anything else -> (None, None)."""
    if not site or "+0x" not in site:
        return None, None
    lib, _, off = site.rpartition("+0x")
    try:
        return lib, int(off, 16)
    except ValueError:
        return None, None


def main(argv):
    last_only = "--last" in argv
    raw = "--raw" in argv
    paths = [a for a in argv if not a.startswith("-")] or default_inputs(last_only)
    if not paths:
        print("No crash reports yet — %s is empty." % state_root())
        print("Crashes are kept automatically from now on; nothing to do until one happens.")
        return 0

    recs = []
    for p in paths:
        try:
            with open(p, "r", errors="replace") as fh:
                recs.extend(parse(fh.read(), p))
        except OSError as e:
            sys.stderr.write("crash-triage: %s\n" % e)
    if not recs:
        print("No crash reports found in %d file(s)." % len(paths))
        return 0

    roots = sysroot_dirs()
    names = package_names(roots)
    resolver = None if raw else Resolver(roots)

    def title(rec):
        pid = os.path.basename((rec.get("cwd") or "").rstrip("/"))
        return names.get(pid, pid or "<unknown>")

    def sym(site, cwd):
        if not resolver:
            return None
        lib, off = split_site(site)
        return resolver.lookup(lib, off, cwd) if lib else None

    crashes = [r for r in recs if r["kind"] == "crashed"]
    dumps = [r for r in recs if r["kind"] != "crashed"]
    print("%d crash report(s)%s across %d file(s)\n"
          % (len(crashes),
             ", %d on-demand dump(s)" % len(dumps) if dumps else "",
             len(paths)))

    clusters = OrderedDict()
    for r in crashes:
        clusters.setdefault(r["pc"], []).append(r)

    for site, group in sorted(clusters.items(), key=lambda kv: -len(kv[1])):
        titles = sorted({title(r) for r in group})
        sigs = sorted({r.get("signal", "?") for r in group})
        alive = [r["alive"] for r in group if "alive" in r]
        when = ""
        if alive:
            when = ("  instantly" if max(alive) <= 2
                    else "  after %d-%ds" % (min(alive), max(alive)))
        print("%-40s %d crash(es)  %s%s" % (site, len(group), ",".join(sigs), when))
        s = sym(site, group[0].get("cwd"))
        if s:
            print("      in  %s" % s)
        for t in titles:
            print("      %s" % t)
        # The immediate caller distinguishes "the same bug" from "the same
        # crash-handling code reached two different ways".
        callers = sorted({r.get("lr", "?") for r in group})
        for c in callers[:3]:
            cs = sym(c, group[0].get("cwd"))
            print("      via %s%s" % (c, "   %s" % cs if cs else ""))
        # One symbolised stack, so the cluster has a call path attached and not
        # only a location.
        deep = max(group, key=lambda r: len(r["stack"]))
        if deep["stack"]:
            print("      stack:")
            for fr in deep["stack"][:8]:
                fs = sym(fr, deep.get("cwd"))
                print("        %-32s %s" % (fr, fs or ""))
        print()

    libs = OrderedDict()
    for r in crashes:
        libs.setdefault(r["pc"].split("+")[0], set()).add(title(r))
    if len(libs) < len(clusters):
        print("By library:")
        for lib, ts in sorted(libs.items(), key=lambda kv: -len(kv[1])):
            print("  %-32s %d title(s)" % (lib, len(ts)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
