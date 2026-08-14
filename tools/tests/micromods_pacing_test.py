#!/usr/bin/env python3
"""Does the micromod scanner stay polite to LeapFrog's servers?

    ./tools/tests/micromods_pacing_test.py

One script, no framework, no network — the same shape as link_resolve_test.py
and for a better reason than most tests have: everything checked here is a
promise made to somebody ELSE'S servers, and the only way to find out that it
regressed in the field would be to have already broken it in the field.

urlopen is replaced with a stub and time.sleep with a recorder, so the whole
thing runs in milliseconds and sends nothing. What it pins down:

  * a scan that reaches the network waits SCAN_INTERVAL after the last one,
    and SLEEPS to do it rather than refusing — clicking Scan twice should get
    you a scan, just a late one
  * requests inside a scan are CDN_DELAY apart, until the scan has made
    SCAN_DEEP_AFTER of them, and SCAN_DEEP_DELAY apart after that
  * every request sent is written to the log with its URL and what came back,
    including the 404s, which are the ordinary way a walk finds the end
  * a run that sends nothing — an --install off the cache — never waits
"""
import importlib.util
import os
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)

_spec = importlib.util.spec_from_file_location(
    "micromods", os.path.join(TOOLS, "micromods.py"))
mm = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(mm)

# Everything it writes goes to a temporary directory: a test that appends to
# the real request log would be falsifying the record it is here to protect.
_tmp = tempfile.mkdtemp(prefix="mm-pacing-")
mm.CACHE = os.path.join(_tmp, "cache")
mm.REQUEST_LOG = os.path.join(_tmp, "requests.log")
mm.SCAN_STAMP = os.path.join(mm.CACHE, ".last-scan")

slept = []
mm.time.sleep = lambda s: slept.append(s)


class FakeResponse:
    """Enough of an HTTPResponse for cdn_fetch, including 3.7's getcode()."""
    status = 200

    def __init__(self, body):
        self.body = body

    def read(self):
        return self.body

    def getcode(self):
        return self.status

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


def serve(_req, timeout=None):
    return FakeResponse(b"x" * 400)


def not_found(_req, timeout=None):
    raise mm.urllib.error.HTTPError("url", 404, "Not Found", {}, None)


mm.urllib.request.urlopen = serve

_fails = 0


def check(label, got, want):
    global _fails
    ok = got == want
    if not ok:
        _fails += 1
    print("  %-42s %-10s %s" % (label, "ok" if ok else "WRONG",
                                "" if ok else "got %r, expected %r" % (got, want)))


print("a scan that follows one too closely waits its turn")
os.makedirs(mm.CACHE, exist_ok=True)
open(mm.SCAN_STAMP, "w").close()                  # the last scan was just now
mm.cdn_fetch("MULT-0x00180002-000001")
check("slept the interval out", round(slept[0]), round(mm.SCAN_INTERVAL))
check("and did not refuse", True, True)

print("\ninside one scan")
slept.clear()
for slot in range(2, 22):                         # requests 2 through 21
    mm.cdn_fetch("MULT-0x00180002-%06d" % slot)
# One gap per request and not one more. Counting is the only way to say "the
# gate did not fire again" here, because SCAN_INTERVAL and SCAN_DEEP_DELAY are
# both five seconds and a sleep of 5.0 could honestly be either.
check("one gap per request, no second gate", len(slept), 20)
check("ordinary gaps", slept[:19], [mm.CDN_DELAY] * 19)
check("the 21st request waits", slept[19], mm.SCAN_DEEP_DELAY)

print("\nand it stays slow once it is deep")
slept.clear()
mm.cdn_fetch("MULT-0x00180002-000022")
check("still the long gap", slept, [mm.SCAN_DEEP_DELAY])

print("\nthe request log")
lines = open(mm.REQUEST_LOG).read().splitlines()
check("one line per request sent", len(lines), 22)
check("names the URL", "digitalcontent.leapfrog.com" in lines[0], True)
check("records what came back", lines[0].endswith("200 400 bytes"), True)

print("\na 404 is an answer, not a failure")
mm.urllib.request.urlopen = not_found
check("returns None", mm.cdn_fetch("MULT-0x00180002-000099"), None)
check("and is logged", open(mm.REQUEST_LOG).read().splitlines()[-1].endswith("404"),
      True)

print("\na run that sends nothing never waits")
mm._gate_passed = False                           # a fresh process would be
slept.clear()
old = time.time() - (mm.SCAN_INTERVAL + 1)
os.utime(mm.SCAN_STAMP, (old, old))
mm.scan_gate()
check("no sleep once the window has passed", slept, [])

print("\nPASS — the scanner paces itself and writes down what it sent"
      if not _fails else "\n%d WRONG" % _fails)
sys.exit(1 if _fails else 0)
