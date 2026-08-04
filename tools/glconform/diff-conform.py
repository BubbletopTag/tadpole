#!/usr/bin/env python3
"""diff-conform.py — hardware log vs emulator log, sorted by what to fix first.

    ./tools/glconform/diff-conform.py hw.log emu.log

The buckets, and why each is separate:

  DIVERGE     hardware OK, emulator not (or the reverse). THE WORKLIST. A
              reverse divergence — we pass something hardware fails — is just
              as interesting: it usually means the assertion is wrong, not that
              we are ahead of the device.
  BOTH_FAIL   both sides fail the same way. Usually means the TEST encodes the
              spec and the device does not implement it. Matching the device is
              what makes titles work, so these are normally "leave alone", not
              "fix to spec".
  BOTH_OK     agree. Nothing to do.
  VALUE_DIFF  both sides pass, but reported different numbers. This is the
              bucket that would be invisible in a plain pass/fail diff, and it
              is where the sharpest bugs hide: a limit we answer 1024 to and
              the device answers 4096 to is two passes and one real defect.
  MISSING     a test one log has and the other does not — a run that stopped
              early, never a silent pass.

Read the selfcheck.* lines in each raw log first. A side that fails those
cannot be trusted about any other err= code in that log, and the summary says
so rather than leaving it to be noticed.
"""
import re
import sys

LINE = re.compile(
    r'^RESULT (?P<name>\S+) (?P<status>OK|FAIL|SKIP) err=0x(?P<err>[0-9a-fA-F]+)'
    r' detail="(?P<detail>.*)"$')

# Numbers inside a detail string, so two passing runs can still be compared.
# Hex first: "0x812F" must not read as the two numbers 0 and 812.
NUMS = re.compile(r'0x[0-9a-fA-F]+|-?\d+')


def parse(path):
    tests, meta = {}, []
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            m = LINE.match(line)
            if m:
                tests[m.group("name")] = (m.group("status"), m.group("err"),
                                          m.group("detail"))
            elif line.startswith(("META ", "EGLINIT ")):
                meta.append(line)
    return tests, meta


def numbers(detail):
    return [n.lower() for n in NUMS.findall(detail)]


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} HW_LOG EMU_LOG")
    hw, hw_meta = parse(sys.argv[1])
    emu, emu_meta = parse(sys.argv[2])

    for line in hw_meta:
        print(f"hw:  {line}")
    for line in emu_meta:
        print(f"emu: {line}")
    print()

    for side, tests in (("hw", hw), ("emu", emu)):
        bad = [n for n in tests
               if n.startswith("selfcheck.") and tests[n][0] != "OK"]
        if bad:
            print(f"!! {side}: {', '.join(bad)} did not pass — every err= code in"
                  f" that log is unreliable.\n")

    buckets = {k: [] for k in
               ("DIVERGE", "VALUE_DIFF", "BOTH_FAIL", "BOTH_OK", "MISSING")}

    for name in sorted(set(hw) | set(emu)):
        h, e = hw.get(name), emu.get(name)
        if h is None or e is None:
            buckets["MISSING"].append((name, h, e))
        elif h[0] == "OK" and e[0] == "OK":
            if numbers(h[2]) != numbers(e[2]):
                buckets["VALUE_DIFF"].append((name, h, e))
            else:
                buckets["BOTH_OK"].append((name, h, e))
        elif h[0] != e[0]:
            buckets["DIVERGE"].append((name, h, e))
        else:
            buckets["BOTH_FAIL"].append((name, h, e))

    def dump(key, blurb, detail=True):
        rows = buckets[key]
        print(f"=== {key} ({len(rows)}) — {blurb} ===")
        for name, h, e in rows:
            if not detail:
                print(f"  {name}")
                continue
            print(f"  {name}")
            for tag, r in (("hw ", h), ("emu", e)):
                if r is None:
                    print(f"    {tag}: (test never ran)")
                else:
                    print(f"    {tag}: {r[0]} err=0x{r[1]} {r[2]}")
        print()

    dump("DIVERGE", "THE WORKLIST: hardware and the emulator disagree")
    dump("VALUE_DIFF", "both pass, different numbers — check these too")
    dump("MISSING", "one side never ran this; check that log's tail first")
    dump("BOTH_FAIL", "both sides fail; usually the device, not us")
    dump("BOTH_OK", "agree", detail=False)

    print(f"summary: {len(buckets['DIVERGE'])} diverge,"
          f" {len(buckets['VALUE_DIFF'])} value-diff,"
          f" {len(buckets['BOTH_FAIL'])} both-fail,"
          f" {len(buckets['BOTH_OK'])} agree,"
          f" {len(buckets['MISSING'])} missing")
    return 1 if buckets["DIVERGE"] or buckets["VALUE_DIFF"] else 0


if __name__ == "__main__":
    sys.exit(main())
