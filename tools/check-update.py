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

HTTPS GOES THROUGH netssl, AND THAT IS THE DIFFERENCE BETWEEN WORKING AND NOT
ON WINDOWS 7. This was the last tool here still calling urllib.request.urlopen
directly, and it is the one tool whose failure is invisible: fetch-firmware.py
and online-update.py were both moved onto the bundled CA bundle when LeapFrog's
DigiCert-rooted certificate turned out to be unverifiable on an unpatched 7,
and this was left behind.

GitHub is worse than LeapFrog on that machine, not better. api.github.com
chains to Sectigo Public Server Authentication Root E46 and release assets are
served from objects.githubusercontent.com, which chains to an ISRG (Let's
Encrypt) root — both NEWER than the DigiCert G2 that already failed, so a
Windows that cannot verify LeapFrog certainly cannot verify these. The result
was an update check that reported `no connection` for ever on a machine with a
perfectly good network, and a Download button that could not have worked if
anyone had pressed it. Verified: the bundle tools/build-windows.sh already
ships beside the interpreter validates both hosts.
"""
import json
import os
import re
import sys
import urllib.error
import urllib.request

# A sibling tool, not a package — the same import dance install-firmware.py and
# online-update.py do, and it must not be fatal: check-update.py is also run
# straight out of a source checkout where the surrounding tree is whatever the
# developer has. Falling back to plain urllib keeps a Linux checkout working
# exactly as before, which is where the system certificate store is current
# anyway and none of this matters.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    import netssl                                  # noqa: E402
except Exception:                                  # noqa: BLE001
    netssl = None

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


# The fallback matters more than it looks: _repo_from_git() shells out to git,
# and an installed Windows tree has neither git nor a .git directory, so EVERY
# Windows user takes this branch. Spelled exactly as the repository is spelled.
# (The API is case-insensitive about owner/repo — the lowercase spelling this
# replaces was checked and does answer — but a constant nobody can test from
# the outside is a poor place to rely on a server's leniency.)
REPO = os.environ.get("TADPOLE_REPO") or _repo_from_git() or "BubbletopTag/tadpole"

# ONE RELEASE, TWO ASSETS, AND EACH PLATFORM ASKS FOR ITS OWN. Handing a
# Windows user an AppImage is worse than offering no update at all: the
# download succeeds and the file cannot run. So the asset follows the platform
# doing the asking. TADPOLE_ASSET overrides, for testing and for anyone
# checking the other platform's release from this one.
ASSET_LINUX = "Tadpole-x86_64.AppImage"
ASSET_WINDOWS = "Glasspole-Setup.exe"
ASSET = os.environ.get("TADPOLE_ASSET") or (
    ASSET_WINDOWS if sys.platform == "win32" else ASSET_LINUX)
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


def _open(req, timeout):
    """One place that opens a URL, so the CA bundle cannot be forgotten again."""
    if netssl is not None:
        return netssl.urlopen(req, timeout=timeout)
    return urllib.request.urlopen(req, timeout=timeout)


def _why(e):
    """A reason worth showing. netssl.explain names a certificate failure as a
    certificate failure; without it the old text blamed the network, which is
    how this stayed hidden on Windows 7 for as long as it did."""
    if netssl is not None:
        try:
            return netssl.explain(e)
        except Exception:                          # noqa: BLE001
            pass
    return str(e)[:80]


def fetch():
    req = urllib.request.Request(API, headers={"User-Agent": UA,
                                               "Accept": "application/vnd.github+json"})
    with _open(req, TIMEOUT) as fh:
        return json.load(fh)


def current_version(argv):
    """What is running: --current wins, then the environment, then the stamp.

    THE STAMP FILE IS THE THIRD ANSWER, and it exists because only one binary
    in the product has the version compiled into it. tools/build-windows.sh
    writes .tadpole-version at the root of the installed tree; the viewer
    passes its own baked-in string with --current and never needs it, but
    anyone running this tool by hand — or from a launcher, or out of an
    installed tree where the compiled-in value is not to hand — used to get
    `status dev` and a list of every release ever made. It was being written
    and read by nothing at all.
    """
    if "--current" in argv:
        return argv[argv.index("--current") + 1]
    env = os.environ.get("TADPOLE_VERSION", "")
    if env:
        return env
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        with open(os.path.join(root, ".tadpole-version")) as fh:
            return fh.read().strip()
    except Exception:                              # noqa: BLE001
        return ""


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
        return unknown(_why(e))
    except Exception as e:                       # noqa: BLE001 - never interrupt
        return unknown(_why(e))

    rels = [r for r in rels if not r.get("draft")]
    seen = []
    for r in rels:
        v = version_of(r.get("tag_name") or r.get("name"))
        if v:
            seen.append((v, r))
    seen.sort(key=lambda t: t[0], reverse=True)
    if not seen:
        # Releases exist but none use the tadpole-DDMMYYYY-NNNN scheme — say
        # that, rather than "no releases", which sends someone looking at the
        # wrong thing. The repo's first release predates the scheme.
        return unknown("no releases use the tadpole-DDMMYYYY-NNNN tag scheme"
                       if rels else "no published releases yet")

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
        try:
            with _open(req, 60) as r, open(tmp, "wb") as out:
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
        except Exception as e:                     # noqa: BLE001
            # A FAILED DOWNLOAD MUST NOT LOOK LIKE A FINISHED ONE. This used to
            # let the exception out, which left the .part file behind and — on
            # Windows, where the viewer then runs whatever is at `dest` — could
            # leave the PREVIOUS download in place for the installer step to
            # launch. Say why, clean up, and fail.
            try:
                os.remove(tmp)
            except OSError:
                pass
            sys.stderr.write("could not download %s: %s\n" % (ASSET, _why(e)))
            return 1
        # A TRUNCATED FILE IS THE WHOLE RISK HERE, and it is worth one
        # comparison: on Windows `dest` is an installer that is about to be
        # EXECUTED, and half an NSIS installer is not a file anyone should be
        # invited to run. Content-Length is advisory, so this only refuses when
        # the server said a length and we did not get it.
        if total and got != total:
            try:
                os.remove(tmp)
            except OSError:
                pass
            sys.stderr.write("download truncated: got %d of %d bytes\n"
                             % (got, total))
            return 1
        os.replace(tmp, dest)
        # Harmless and meaningless on Windows — os.chmod there honours only the
        # read-only bit — but this is also how the Linux AppImage becomes
        # runnable, so it stays.
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

    # A WORKING COPY IS NOT A FAILED CHECK. `dev` used to fall through to
    # "unknown", so the front end told a developer "Could not check — no answer
    # from GitHub", blaming the network for the fact that they are not running
    # a release. It is a third state and deserves its own name.
    print("status %s" % ("behind" if behind else
                         ("current" if cur is not None else "dev")))
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
