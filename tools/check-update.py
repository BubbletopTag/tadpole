#!/usr/bin/env python3
"""Is there a newer Tadpole release, and what changed since this one?

    ./tools/check-update.py                     report, machine-readable
    ./tools/check-update.py --current 08082026-0001
    ./tools/check-update.py --download DEST     fetch the newest AppImage
    ./tools/check-update.py --human             readable, for a terminal

Releases are tagged `tadpole-DDMMYYYY-NNNN`, one canonical asset per release
called Tadpole-x86_64.AppImage, and the release body is the commit message. So
"what changed" is answerable without cloning anything: it is the bodies of every
release newer than the one running.

OUTPUT IS PARSED BY THE VIEWER, so it is line-oriented and boring:

    status  current|behind|unknown
    current 08082026-0001
    latest  09082026-0002
    count   3
    asset   https://github.com/…/Tadpole-x86_64.AppImage
    ver     09082026-0002
    note    fmv: video renders — fb2 is YUV420
    note    …
    ver     08082026-0003
    note    …

WHY NOT A LIBRARY. urllib is in the standard library and this runs on a user's
machine, where `pip install requests` is not a thing we get to ask for. The
GitHub API needs no key for public releases; unauthenticated callers get 60
requests an hour, which is far more than a launch-time check will ever use.

NETWORK FAILURE IS NOT AN ERROR HERE. Someone offline is not doing anything
wrong, and an update check that interrupts them with a dialog is a bug. Any
failure prints `status unknown` with a reason and exits 0; only a genuine
usage mistake exits non-zero.
"""
import json
import os
import re
import sys
import urllib.error
import urllib.request

# Read from the git remote where possible, so a fork checks its own releases
# rather than reporting somebody else's as an update.
def _repo_from_git():
    try:
        import subprocess
        r = subprocess.run(["git", "-C", os.path.dirname(os.path.dirname(
            os.path.abspath(__file__))), "remote", "get-url", "origin"],
            capture_output=True, text=True, timeout=4)
        m = re.search(r"github\.com[:/]([^/]+/[^/.]+)", r.stdout.strip())
        return m.group(1) if m else None
    except Exception:
        return None


REPO = os.environ.get("TADPOLE_REPO") or _repo_from_git() or "bubbletoptag/tadpole"
ASSET = "Tadpole-x86_64.AppImage"
API = "https://api.github.com/repos/%s/releases?per_page=30" % REPO
UA = "Tadpole-update-check"
TIMEOUT = 8


def version_of(tag_or_name):
    """`tadpole-08082026-0001` or `Tadpole 08082026-0001` -> (2026,8,8,1).

    Sorting on the raw string would be wrong the moment the year rolls over:
    DDMMYYYY puts 01012027 before 02012026. Parse it into fields and compare
    those.
    """
    m = re.search(r"(\d{2})(\d{2})(\d{4})-(\d{4})", tag_or_name or "")
    if not m:
        return None
    d, mo, y, n = (int(x) for x in m.groups())
    return (y, mo, d, n)


def fetch():
    req = urllib.request.Request(API, headers={"User-Agent": UA,
                                               "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as fh:
        return json.load(fh)


def current_version(argv):
    """What is running: --current wins, then the environment, then 'dev'."""
    if "--current" in argv:
        return argv[argv.index("--current") + 1]
    return os.environ.get("TADPOLE_VERSION", "")


def main(argv):
    human = "--human" in argv
    dest = None
    if "--download" in argv:
        dest = argv[argv.index("--download") + 1]

    cur_s = current_version(argv)
    cur = version_of(cur_s)

    def unknown(why):
        if human:
            print("Could not check for updates: %s" % why)
        else:
            print("status unknown")
            print("reason %s" % why)
        return 0

    try:
        rels = fetch()
    except urllib.error.HTTPError as e:
        return unknown("GitHub returned %s" % e.code)
    except urllib.error.URLError as e:
        return unknown("no connection (%s)" % (e.reason,))
    except Exception as e:                       # noqa: BLE001 - never interrupt
        return unknown(str(e)[:80])

    rels = [r for r in rels if not r.get("draft")]
    seen = []
    for r in rels:
        v = version_of(r.get("tag_name") or r.get("name"))
        if v:
            seen.append((v, r))
    seen.sort(key=lambda t: t[0], reverse=True)
    if not seen:
        return unknown("no published releases yet")

    latest_v, latest = seen[0]

    def asset_url(rel):
        for a in rel.get("assets", []):
            if a.get("name") == ASSET:
                return a.get("browser_download_url")
        return None

    url = asset_url(latest)

    if dest:
        # DOWNLOAD TO A TEMPORARY NAME AND RENAME. A half-written AppImage that
        # looks complete is the one failure mode worth engineering against here:
        # the user would run it and get something inexplicable.
        if not url:
            sys.stderr.write("the newest release has no %s attached\n" % ASSET)
            return 1
        tmp = dest + ".part"
        req = urllib.request.Request(url, headers={"User-Agent": UA})
        with urllib.request.urlopen(req, timeout=60) as r, open(tmp, "wb") as out:
            total = int(r.headers.get("Content-Length") or 0)
            got = 0
            while True:
                chunk = r.read(262144)
                if not chunk:
                    break
                out.write(chunk)
                got += len(chunk)
                if total:
                    print("pct %d" % (100 * got // total), flush=True)
        os.replace(tmp, dest)
        os.chmod(dest, 0o755)
        print("saved %s" % dest)
        return 0

    newer = [(v, r) for v, r in seen if cur is None or v > cur]
    behind = bool(newer) and cur is not None

    if human:
        if cur is None:
            print("Running an unreleased build; newest published is %s."
                  % (latest.get("name") or latest.get("tag_name")))
        elif not newer:
            print("Up to date (%s)." % cur_s)
            return 0
        else:
            print("%d newer release(s). You have %s, newest is %s.\n"
                  % (len(newer), cur_s, latest.get("tag_name")))
        for v, r in newer[:10]:
            print("== %s" % (r.get("name") or r.get("tag_name")))
            for line in (r.get("body") or "").strip().splitlines()[:12]:
                print("   %s" % line)
            print()
        return 0

    print("status %s" % ("behind" if behind else
                         ("current" if cur is not None else "unknown")))
    print("current %s" % (cur_s or "-"))
    print("latest %s" % (latest.get("tag_name") or "-"))
    print("title %s" % (latest.get("name") or "-"))
    print("count %d" % len(newer))
    if url:
        print("asset %s" % url)
    for v, r in newer[:10]:
        print("ver %s" % (r.get("name") or r.get("tag_name")))
        body = (r.get("body") or "").strip()
        for line in body.splitlines()[:14]:
            line = line.rstrip()
            if line:
                print("note %s" % line[:120])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
