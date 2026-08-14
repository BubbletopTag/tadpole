#!/usr/bin/env python3
"""micromods.py — get the bonus content the device was never connected to earn.

    ./tools/micromods.py                       what is installed
    ./tools/micromods.py --scan 0x00180002     what LeapFrog still serves for it
    ./tools/micromods.py --scan 0x00180002 --install
    ./tools/micromods.py --scan 0x00180002 --install --only 000009,000011
    ./tools/micromods.py --ingest DIR          install packages you already have
    ./tools/micromods.py --fix-access          repair ones installed inert
    ./tools/micromods.py --uninstall           undo exactly what this tool made

Micromods are LeapFrog's free extras — alternate music, expansion tracks,
dress-up clothes, seasonal themes. A child earned badges, a parent connected
the device to LFConnect, and points bought them. A device that was never
connected has none, and Tadpole cannot talk to LFConnect.

BUT THE PACKAGES ARE STILL SERVED, and that is the whole tool:

    https://digitalcontent.leapfrog.com/packages/<ProductID>/<PackageID>.lf2

--scan asks for exactly the slots a title could have, stops at the first gap,
and caches what comes back. Nothing is enumerated, nothing is crawled, and
every request names one package belonging to a title already installed here.

WHY DOWNLOADING BEATS SYNTHESISING, which is what this tool used to do. A
micromod carries no content — the game already ships the material — so it
looked like a flag that could simply be written. It is a flag. It is not the
same flag twice:

  * SpongeBob's Clam Prix (DjMicroMod::readPackageInfo) compares the meta.inf
    `Name` against a table of eight strings compiled into its own binary:
    BACKGROUND_MUSIC_1..3, KART_TRACK_EXPANSION, then KART_TRACK four times.
    A name outside that table means nothing to it.
  * Ni Hao Kai-lan (CheckMDLFiles) never reads the name. It looks INSIDE the
    package for clothes.txt, collectable.txt or theme.txt, each holding one
    digit, and builds a bitmask from them — clothes 1..7, collectables 1..3,
    themes 1..3.
  * 125 installed ProductIDs call LTM::CMicroDownloads::get. Only five use an
    engine anybody here has read.

So there is no universal flag to write, and a slot number is not one either.
The real package always carries whatever its own title looks for, which is why
this fetches instead of guessing.

WHAT THE FIRMWARE DEMANDS FIRST. Before a title sees anything,
LTM::CMicroDownloads::get filters LF/Bulk/Downloads by: no `doom` file, a
meta.inf, Type="MicroDownload", a ProductID equal to the running title's, and
then DeviceAccess=1 or a ProfileAccess naming the signed-in player. A package
as SERVED has neither of those last two — LFConnect adds one on install — so
everything installed here gets one written. See ensure_access.

Turning a micromod on is still done in the game's own menu, per profile, so
nothing here touches a profile.

NOTHING IS OVERWRITTEN and everything is logged. A package this tool did not
create is never touched, and --uninstall works from the manifest it wrote, so
it cannot remove a micromod that came off the real device.
"""
import argparse
import hashlib
import io
import json
import os
import re
import shutil
import sys
import tarfile
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
SYSROOT = os.environ.get("TADPOLE_SYSROOT") or os.path.join(PROJ, "runtime", "sysroot")
BULK = os.environ.get("TADPOLE_BULK") or os.path.join(SYSROOT, "LF", "Bulk")
DOWNLOADS = os.path.join(BULK, "Downloads")
PROGRAMFILES = os.path.join(BULK, "ProgramFiles")
MANIFEST = os.path.join(PROJ, "runtime", "micromods-made.json")
CACHE = os.path.join(PROJ, "runtime", "micromods-cache")

# ---- LeapFrog's content server -------------------------------------------
#
# The layout is the one tools/fetch-firmware.py already uses for firmware:
#
#     https://digitalcontent.leapfrog.com/packages/<ProductID>/<PackageID>.lf2
#
# ASK ONLY FOR WHAT WE HAVE A REASON TO WANT. The bucket answers a bare GET
# with an S3 listing, and it is tempting to read the catalogue straight out of
# it — but it returns only the first 1000 keys and ignores `prefix` and
# `marker` (CloudFront drops the query string), so that is both a dead end and
# somebody's boundary. Every request this tool makes names one exact package,
# derived from the ProductID of a title the user already owns and has
# installed. Nothing is enumerated and nothing is crawled.
#
# ONLY THE MULT FAMILY IS SERVED. LPAD and LST3 micromods are not on the CDN
# at all. That is not a problem: LTM::CMicroDownloads::get filters on the
# ProductID and never looks at the family, so a MULT package is read by an
# LPAD or LST3 build of the same title.
CDN = "https://digitalcontent.leapfrog.com/packages"
CDN_FAMILY = "MULT"
UA = "tadpole-micromods/1 (+https://github.com/BubbletopTag/leappad-emu)"

# ---- how hard this leans on somebody else's server ------------------------
#
# Three limits, for three different worries, all enforced inside cdn_fetch()
# so no future caller can route around them by accident.
#
#   CDN_DELAY        between requests within one scan. A scan is a dozen of
#                    them; there is no reason to be quick about it.
#   SCAN_INTERVAL    between one scan REACHING THE NETWORK and the next. Held
#                    across processes — each scan is a separate run of this
#                    tool, so a stamp file is the only thing that can remember
#                    — and it SLEEPS rather than refusing: someone clicking
#                    Scan twice wants the scan, not an error message.
#   SCAN_DEEP_*      a scan that keeps going gets slower. Past the 20th
#                    request the gap becomes five seconds, so the deep walk a
#                    title with unusual amounts of bonus content provokes
#                    costs the server the same as several ordinary ones.
#                    With RUNS as it stands that is slots 21..24 of the first
#                    run and anything the later runs add on top.
CDN_DELAY = 0.3
SCAN_INTERVAL = 5.0
SCAN_DEEP_AFTER = 20
SCAN_DEEP_DELAY = 5.0

# EVERY REQUEST, WRITTEN DOWN. This tool talks to a company's servers about
# packages for titles the user owns, and until now the only record of what it
# asked for was whatever scrolled past in the progress panel. One line per
# request, appended, so "what did this actually send" has an answer tomorrow
# as well as today. Cache hits are not requests and are not logged.
REQUEST_LOG = os.path.join(PROJ, "runtime", "micromods-requests.log")
# Touched when a scan is allowed through the gate; its mtime IS the record of
# when the last one started.
SCAN_STAMP = os.path.join(CACHE, ".last-scan")

# ---- the catalogue --------------------------------------------------------
#
# ProductID -> list of (slot, Name, MDLType, depends_on_cartridge)
#
# ONLY ENTRIES WITH A PROVENANCE, and only for the offline path. --scan gets
# the real packages from LeapFrog and needs none of this; the catalogue is what
# --enable falls back on when there is no network, and it holds nothing that
# was not read out of a real package.
#
# It is also NOT a description of a title's micromods. Clam Prix's binary
# accepts a fifth name, KART_TRACK, which no LFConnect package here carries.
#
# `depends` marks the ones whose real package carries
# Depends="<SYS>-<ProductID>-999999" — "prove you own the cartridge". The
# CartridgeData stub it names is written alongside when it is missing, because
# a dependency on an absent package is exactly the kind of thing a picker
# quietly filters out.
CATALOGUE = {
    # SpongeBob SquarePants: The Clam Prix (Virtuos). Read from the LFConnect
    # packages in Projects/0x00180025/installables/.
    "0x00180025": [
        ("000001", "BACKGROUND_MUSIC_1", "background_music_1", True),
        ("000002", "BACKGROUND_MUSIC_2", "background_music_2", True),
        ("000003", "BACKGROUND_MUSIC_3", "background_music_3", True),
        ("100000", "KART_TRACK_EXPANSION", "kart_track_expansion", False),
        # In the binary's name table (entries 4-7) but in no package here.
        ("000004", "KART_TRACK", "kart_track", True),
    ],
}

# The families a LeapPad-generation title is packaged for. A micromod has to
# match the family of the build that actually runs, which is why the same
# unlock ships as both LPAD- and MULT-: installing only one leaves the other
# build seeing nothing.
FAMILIES = ("LPAD", "MULT", "MHRS", "PADS", "PAD2", "LST3", "PHRS")

META_TEMPLATE = """MetaVersion="1.0"
Device="{device}"
Type="MicroDownload"
ProductID={product}
PackageID="{pkgid}"
Version="1.0.0.0"
Locale="en-us"
Name="{name}"
Publisher="LeapFrog Enterprises, Inc."
Hidden=0
MDLType="{mdltype}"
{depends}DeviceAccess=1
"""

DEVICE_FOR = {
    "LPAD": "LeapPadExplorer",
    "MULT": "LeapPad2",
    "MHRS": "LeapPad2",
    "PADS": "LeapPad2",
    "PAD2": "LeapPad2",
}


def load_manifest():
    try:
        with open(MANIFEST) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {"made": []}


def save_manifest(m):
    os.makedirs(os.path.dirname(MANIFEST), exist_ok=True)
    with open(MANIFEST, "w") as f:
        json.dump(m, f, indent=2, sort_keys=True)
        f.write("\n")


def meta_fields(path):
    """-> dict of a meta.inf's fields, quotes stripped. {} if unreadable."""
    out = {}
    try:
        with open(path, "r", errors="replace") as f:
            for line in f:
                k, _, v = line.partition("=")
                k = k.strip()
                if k:
                    out[k] = v.strip().strip('"')
    except OSError:
        pass
    return out


def installed_micromods():
    """-> {PackageID: fields} for everything already in Downloads."""
    out = {}
    if not os.path.isdir(DOWNLOADS):
        return out
    for name in sorted(os.listdir(DOWNLOADS)):
        meta = os.path.join(DOWNLOADS, name, "meta.inf")
        if not os.path.isfile(meta):
            continue
        f = meta_fields(meta)
        if f.get("Type") == "MicroDownload":
            out[name] = f
    return out


def installed_titles():
    """-> {ProductID: [PackageID, ...]} for what is in ProgramFiles."""
    out = {}
    if not os.path.isdir(PROGRAMFILES):
        return out
    for name in sorted(os.listdir(PROGRAMFILES)):
        parts = name.split("-")
        if len(parts) == 3 and parts[1].startswith("0x"):
            out.setdefault(parts[1], []).append(name)
    return out


def has_micromod_code(pkgdir):
    """Does this title's binary contain the micromod subsystem?

    Read as bytes and searched for the plain markers rather than parsed: the
    point is a yes/no on 460 files, and a title that mentions any of these is
    one whose micromods are worth chasing."""
    so = os.path.join(pkgdir, "App.so")
    try:
        with open(so, "rb") as f:
            blob = f.read()
    except OSError:
        return False
    return (b"MicroDownload" in blob or b"MicroMod" in blob
            or b"kPackageTypeMDL" in blob)


def caps_tokens(pkgdir):
    """Candidate micromod names: ALL_CAPS_WITH_UNDERSCORES in the binary.

    A wide net on purpose. Clam Prix returns 123 of these for 4 real names, so
    this is a list to read, never a list to install."""
    so = os.path.join(pkgdir, "App.so")
    try:
        with open(so, "rb") as f:
            blob = f.read()
    except OSError:
        return []
    pat = re.compile(rb"[A-Z][A-Z0-9]*(?:_[A-Z0-9]+)+")
    seen = set()
    for m in pat.finditer(blob):
        tok = m.group(0)
        # bounded by non-identifier bytes, so a fragment of a longer symbol
        # does not come back as a name of its own
        s, e = m.start(), m.end()
        before = blob[s - 1:s] if s else b"\0"
        after = blob[e:e + 1] or b"\0"
        if before.isalnum() or before == b"_" or after.isalnum() or after == b"_":
            continue
        if 6 <= len(tok) <= 40:
            seen.add(tok.decode("latin1"))
    return sorted(seen)


def md5_file(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            h.update(chunk)
    return h.hexdigest()


def pkg_files(pkgdir):
    """Every file in a package, as ./-relative paths, sorted. NOT just the top.

    os.listdir() was what this used, and for the flat packages it was written
    against — a meta.inf and a marker file — the difference never showed."""
    out = []
    for root, _dirs, files in os.walk(pkgdir):
        for fn in files:
            if fn == "packagefiles.md5":
                continue
            p = os.path.join(root, fn)
            rel = os.path.relpath(p, pkgdir).replace(os.sep, "/")
            out.append(("./" + rel, p))
    return sorted(out)


def write_checksums(pkgdir, sep="  "):
    """packagefiles.md5 for a package built here, listing every file in it.

    THIS USED TO TRUNCATE THE MANIFEST TO THE TOP LEVEL, and quietly. Mr.
    Pencil's Leaplets are 249 files in nine subdirectories, and installing one
    rewrote its 248-line manifest as two lines: download.json and meta.inf.
    Every other checksum — every coloring book page, every scene, every ogg —
    was deleted from the record of what the package is supposed to contain.
    Kai-lan and Bubble Guppies never showed it because their packages have no
    subdirectories at all.

    `sep` is the gap between hash and path, because the packages do not agree:
    LeapFrog's own Bubble Guppies and Kai-lan manifests use two spaces and Mr.
    Pencil's use one. When rewriting somebody else's file, keep theirs."""
    with open(os.path.join(pkgdir, "packagefiles.md5"), "w") as f:
        for rel, path in pkg_files(pkgdir):
            f.write("%s%s%s\n" % (md5_file(path), sep, rel))


def update_checksum(pkgdir, name):
    """Re-hash ONE file in an existing manifest, leaving every other line as
    the vendor wrote it — order, spacing and all.

    Rewriting the whole file was the mistake: this package came from LeapFrog
    with a complete manifest, we changed exactly one byte range in exactly one
    file, and the honest edit is one line. Regenerating it can only lose
    information — and did."""
    man = os.path.join(pkgdir, "packagefiles.md5")
    target = "./" + name
    try:
        with open(man, "r", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        write_checksums(pkgdir)          # no manifest: make a complete one
        return
    digest = md5_file(os.path.join(pkgdir, name))
    out, done = [], False
    for line in lines:
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[1].strip() == target:
            sep = line[len(parts[0]):len(line) - len(parts[1])]
            out.append("%s%s%s" % (digest, sep, parts[1]))
            done = True
        else:
            out.append(line)
    if not done:
        out.append("%s  %s" % (digest, target))
    with open(man, "w") as f:
        f.write("\n".join(out) + "\n")


# ---- talking to the CDN ---------------------------------------------------

def _ssl_ready():
    """LeapFrog's chain is rooted at DigiCert Global Root G2, which an
    un-updated Windows 7 does not carry. netssl is the sibling tool that
    installs a bundle for it; importing it is the whole setup."""
    try:
        sys.path.insert(0, HERE)
        import netssl  # noqa: F401 — imported for its import-time side effect
    except Exception:
        pass


def cdn_url(pkgid, ext="lf2"):
    return "%s/%s/%s.%s" % (CDN, pkgid.split("-")[1], pkgid, ext)


def log_request(url, outcome):
    """One line per request actually sent. Never raises.

    A log that cannot be written is not a reason to abandon a scan, so every
    failure here is swallowed — the worst case is a missing line, and the
    alternative is a tool that dies because a directory is read-only."""
    line = "%s  GET %s  %s\n" % (
        time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), url, outcome)
    try:
        os.makedirs(os.path.dirname(REQUEST_LOG), exist_ok=True)
        with open(REQUEST_LOG, "a", encoding="utf-8") as f:
            f.write(line)
    except OSError:
        pass


_gate_passed = False       # this process has already waited its turn
_requests = 0              # requests SENT, not slots considered


def scan_gate():
    """Sleep until this scan is allowed to touch the network. Once per run.

    Deliberately gated on the first REQUEST rather than on the tool starting.
    A scan whose packages are all cached — which is exactly what --install
    does, since it re-scans to find what it is installing — sends nothing to
    LeapFrog, and making the user watch a five second stall for a run that
    never opens a socket would be a limit on them rather than on us."""
    global _gate_passed
    if _gate_passed:
        return
    _gate_passed = True
    try:
        waited = time.time() - os.path.getmtime(SCAN_STAMP)
    except OSError:
        waited = SCAN_INTERVAL          # never scanned here before
    if waited < SCAN_INTERVAL:
        left = SCAN_INTERVAL - waited
        print("  waiting %.1fs — one scan every %gs" % (left, SCAN_INTERVAL))
        sys.stdout.flush()              # the progress panel reads this live
        time.sleep(left)
    try:
        os.makedirs(os.path.dirname(SCAN_STAMP), exist_ok=True)
        with open(SCAN_STAMP, "w") as f:
            f.write(time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()) + "\n")
    except OSError:
        pass


def cdn_fetch(pkgid, timeout=30):
    """-> package bytes, or None when the CDN does not have it.

    None means 404 and nothing else. A refused connection or a timeout raises,
    because "the server is unreachable" and "this micromod does not exist" are
    the two answers a scan must never confuse: treating the first as the second
    would quietly report that a title has no bonus content.

    ALL THE PACING LIVES HERE, before the request rather than after it, so it
    applies to the last request of a scan as well as the ones in the middle —
    a delay that only runs afterwards is not a delay before the next caller's
    first request."""
    global _requests
    url = cdn_url(pkgid)
    scan_gate()
    if _requests:
        deep = _requests >= SCAN_DEEP_AFTER
        if deep:
            print("  %d requests in — pausing %gs" % (_requests, SCAN_DEEP_DELAY))
            sys.stdout.flush()
        time.sleep(SCAN_DEEP_DELAY if deep else CDN_DELAY)
    _requests += 1
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            blob = r.read()
            # .status is 3.9 and later on this object; .getcode() is what 3.7
            # has, and the Windows 7 build is pinned to 3.7. See docs/.
            code = getattr(r, "status", None) or r.getcode()
            log_request(url, "%s %d bytes" % (code, len(blob)))
            return blob
    except urllib.error.HTTPError as e:
        log_request(url, str(e.code))
        if e.code in (403, 404):
            return None
        raise
    except Exception as e:
        log_request(url, "FAILED %s" % str(e)[:80])
        raise


def tar_meta(blob):
    """-> (fields, members) read from a package's meta.inf, or (None, None)."""
    try:
        t = tarfile.open(fileobj=io.BytesIO(blob))
        names = t.getnames()
    except (tarfile.TarError, OSError):
        return None, None
    metan = next((n for n in names if os.path.basename(n) == "meta.inf"), None)
    if not metan:
        return None, None
    f = t.extractfile(metan)
    fields = {}
    if f:
        for line in f.read().decode("utf-8", "replace").splitlines():
            k, _, v = line.partition("=")
            if k.strip():
                fields[k.strip()] = v.strip().strip('"')
    return fields, names


def cache_dir(product):
    return os.path.join(CACHE, product)


def cached(pkgid):
    p = os.path.join(cache_dir(pkgid.split("-")[1]), pkgid + ".lf2")
    return p if os.path.isfile(p) else None


def write_index(product, found):
    """One line per package the server has, for the viewer to read.

    TSV rather than JSON because the reader is 40 lines of C in the Micromods
    screen, and a scan is the only thing that writes it."""
    d = cache_dir(product)
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, "index.tsv.new")
    with open(tmp, "w", encoding="utf-8") as f:
        for pkgid, fields, names in found:
            f.write("%s\t%s\t%s\t%s\n" % (
                pkgid.rsplit("-", 1)[1],
                "mod" if fields.get("Type") == "MicroDownload" else "dep",
                (fields.get("Name", "") or "").replace("\t", " "),
                "1" if any(n.lower().endswith(".png") for n in names) else "0"))
    os.replace(tmp, os.path.join(d, "index.tsv"))


def cache_put(pkgid, blob):
    d = cache_dir(pkgid.split("-")[1])
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, pkgid + ".lf2")
    with open(p, "wb") as f:
        f.write(blob)
    return p


# WHERE A TITLE'S SLOTS LIVE. Slots are consecutive within a run and the runs
# are far apart, so a scan walks each run and gives up when it stops finding
# things. The spans are backstops against a title with more bonus content than
# anyone here has seen, not numbers anything depends on.
#
# THE STARTS ARE EVIDENCE, NOT GUESSES, and the first three were not enough.
# This began as 1, 100000 and 999999 — everything Ni Hao Kai-lan, Clam Prix and
# Bubble Guppies between them use — and reported "no micromods on the server"
# for Mr. Pencil Saves Doodleburg, which has two. They are at 003001 and
# 003002, nowhere near anything being asked for, and LeapFrog serves 003001 to
# this day: 5.2 MB of coloring book, 404 on every slot the scan actually
# walked. A negative result from a search that never looked is worse than no
# search, because it is believed.
#
# Every start below is a slot some real package on a real device uses:
#
#   000001   Kai-lan 1..11, Bubble Guppies 1..5, Clam Prix 1..3
#   003001   Mr. Pencil 0x00180015 and 0x00180022 — MDLType "Leaplet",
#            confirmed served by the CDN as MULT-0x00180015-003001
#   004001   0x00180024, also a Leaplet
#   100000   Clam Prix's kart track
#   200000   0x00210008, and 0x1F1E0002 at 200001..200003
#   300000   0x1F1E0002 at 300000..300016
#   900000   0x001E0010
#   999999   not a micromod but the CartridgeData stub the packages name in
#            Depends — "prove you own the cartridge". One request, and it
#            saves synthesising one.
#
# WHAT IS STILL NOT FOUND, said plainly rather than left to be discovered: a
# run that starts late. 0x001E0010's slots begin at 000007, so a walk from
# 000001 misses on its first request and stops. Finding that would mean asking
# for slots there is no reason to think exist, one after another, which is the
# crawl this tool is written not to be. A title in that position is still
# reachable through --ingest with the package in hand.
RUNS = ((1, 24), (3001, 24), (4001, 24), (100000, 8), (200000, 8),
        (300000, 24), (900000, 4), (999999, 1))

# HOW MANY HOLES A RUN SURVIVES, once it has produced something. 0x1F1E0002
# has 300008 and 300011 with nothing between them, so stopping at the first
# miss loses six packages that are there. The tolerance is deliberately only
# extended to runs that have ALREADY found something: an empty run still costs
# exactly one request, which is what keeps a scan of a title with no bonus
# content at eight requests rather than twenty-four.
RUN_GAP = 2


def scan_online(product, refresh=False, quiet=False):
    """Ask the CDN which micromods this title has. -> [(pkgid, fields, names)]

    One request per slot. A run that finds nothing stops at its first miss, so
    a title with no bonus content at all costs one request per run — eight —
    and a title that has some pays only for the runs it actually uses."""
    _ssl_ready()
    out = []
    for start, span in RUNS:
        got, gap = 0, 0
        for i in range(span):
            pkgid = "%s-%s-%06d" % (CDN_FAMILY, product, start + i)
            hit = cached(pkgid)
            if hit and not refresh:
                blob = open(hit, "rb").read()
            else:
                try:
                    blob = cdn_fetch(pkgid)      # paces itself; see cdn_fetch
                except Exception as e:
                    print("  ! %s: %s" % (pkgid, str(e)[:70]), file=sys.stderr)
                    return out, False
                if blob is None:
                    # A run that has found nothing is not this title's run, and
                    # stops dead. One that has found something is allowed a
                    # hole or two, because real runs have them. See RUN_GAP.
                    gap += 1
                    if not got or gap > RUN_GAP:
                        break
                    continue
                gap = 0
                cache_put(pkgid, blob)
            fields, names = tar_meta(blob)
            if fields is None:
                continue
            got += 1              # this run is real; it may now survive a hole
            out.append((pkgid, fields, names))
            if not quiet:
                print("    %-28s %-26s %s"
                      % (pkgid, fields.get("Name", "")[:26],
                         "preview" if any(n.endswith(".png") for n in names) else ""))
    if out:
        write_index(product, out)
    return out, True


PROFILE_ACCESS = "ProfileAccess=0,1,2,3"


def ensure_access(pkgdir):
    """Give a package an access field if it has neither. -> True if written.

    THE PACKAGE AS LEAPFROG SHIPS IT IS NOT YET INSTALLABLE. A micromod
    downloaded from digitalcontent.leapfrog.com carries no DeviceAccess and no
    ProfileAccess — LFConnect adds one when it assigns the content to a child,
    the same way it adds ProfileAccess to an Application. Compare the raw
    download with a copy that went through LFConnect:

        MULT-0x00180002-000009.lf2   (CDN)   ...MDLType="background theme"
        LPAD-0x00180002-000009       (real)  ...ProfileAccess=0,1,2  Size=28672

    Without one of those two fields the package is DISCARDED BEFORE THE TITLE
    EVER SEES IT. LTM::CMicroDownloads::get walks the Downloads folder and its
    filter ends:

        if (meta.GetDeviceAccess() == 1)          -> accept
        n = meta.GetProfileAccess(&ids)
        if (n == 0)                               -> reject
        accept only if the current player is in ids

    so "no access field" is the reject case. It is the same fault as the one
    install-game.py fixes for the home screen — a package that installs
    cleanly, reports success, and is then filtered out for want of the one
    field nobody wrote.

    0,1,2,3 is every profile slot the device has. LeapFrog's own micromods say
    0,1,2; a fourth costs nothing and matches whichever profile is signed in.
    """
    meta = os.path.join(pkgdir, "meta.inf")
    try:
        with open(meta, "r", errors="replace") as f:
            text = f.read()
    except OSError:
        return False
    for line in text.splitlines():
        if line.startswith("ProfileAccess=") or line.strip() == "DeviceAccess=1":
            return False
    # THE NEWLINE IS NOT OPTIONAL — see install-game.py. These files often end
    # without one, and appending straight to the end welds two fields into
    # `Size=20480ProfileAccess=0,1,2,3`, losing both.
    with open(meta, "a", encoding="utf-8") as f:
        f.write(("" if text.endswith(("\n", "\r")) else "\n") + PROFILE_ACCESS + "\n")
    # ONE LINE, not the whole manifest — see update_checksum.
    update_checksum(pkgdir, "meta.inf")
    return True


def make_package(pkgid, product, name, mdltype, depends, dry):
    """Write one micromod. -> 'made' | 'exists' | 'would'."""
    dest = os.path.join(DOWNLOADS, pkgid)
    if os.path.exists(dest):
        return "exists"
    if dry:
        return "would"
    family = pkgid.split("-")[0]
    dep = ""
    if depends:
        dep = 'Depends="%s-%s-999999","1.0.0.0"\n' % (family, product)
    os.makedirs(dest)
    with open(os.path.join(dest, "meta.inf"), "w") as f:
        f.write(META_TEMPLATE.format(
            device=DEVICE_FOR.get(family, "LeapPad2"),
            product=product, pkgid=pkgid, name=name,
            mdltype=mdltype, depends=dep))
    write_checksums(dest)
    return "made"


def make_cartridge_stub(family, product, dry):
    """The CartridgeData stub a music unlock declares Depends on.

    Written only when it is missing. Its whole job is to exist: the real one
    is metadata identifying ownership of the physical cartridge, and the
    unlocks that name it are the ones LeapFrog gated on owning the game."""
    pkgid = "%s-%s-999999" % (family, product)
    dest = os.path.join(DOWNLOADS, pkgid)
    if os.path.exists(dest):
        return "exists"
    if dry:
        return "would"
    os.makedirs(dest)
    with open(os.path.join(dest, "meta.inf"), "w") as f:
        f.write('MetaVersion="1.0"\n'
                'Device="%s"\n'
                'Type="CartridgeData"\n'
                'ProductID=%s\n'
                'PackageID="%s"\n'
                'Version="1.0.0.0"\n'
                'Locale="en-us"\n'
                'Name="CARTRIDGE_DATA"\n'
                'Publisher="LeapFrog Enterprises, Inc."\n'
                'Hidden=1\n'
                'DeviceAccess=1\n'
                % (DEVICE_FOR.get(family, "LeapPad2"), product, pkgid))
    write_checksums(dest)
    return "made"


# ---- commands -------------------------------------------------------------

def cmd_scan():
    inst = installed_micromods()
    titles = installed_titles()
    print("micromods installed: %d" % len(inst))
    by_product = {}
    for pkgid, f in inst.items():
        by_product.setdefault(f.get("ProductID", "?"), []).append((pkgid, f))
    for product in sorted(by_product):
        print("  %s" % product)
        for pkgid, f in sorted(by_product[product]):
            print("    %-28s %s" % (pkgid, f.get("Name", "")))

    # COUNTED BY TITLE, NOT BY PACKAGE. The same game ships once per device
    # family, so counting packages says 265 where counting titles says 135,
    # and only the second is the number anyone means by "how many of my games
    # have bonus content".
    capable = []
    for product, pkgs in sorted(titles.items()):
        for p in pkgs:
            if has_micromod_code(os.path.join(PROGRAMFILES, p)):
                capable.append(product)
                break
    print("\ntitles with a micromod code path: %d of %d"
          % (len(capable), len(titles)))

    print("\ncatalogued and not yet installed:")
    missing = 0
    for product, entries in sorted(CATALOGUE.items()):
        fams = [p.split("-")[0] for p in titles.get(product, [])]
        fams = [f for f in fams if f in FAMILIES]
        for slot, name, _mdl, _dep in entries:
            for fam in sorted(set(fams)):
                pkgid = "%s-%s-%s" % (fam, product, slot)
                if pkgid not in inst:
                    print("    %-28s %s" % (pkgid, name))
                    missing += 1
    if not missing:
        print("    (none)")
    print("\n--scan <ProductID> asks LeapFrog what that title actually has, "
          "which beats\nanything catalogued here. --enable writes the "
          "catalogued ones with no network.")


def cmd_enable(which, dry):
    titles = installed_titles()
    inst = installed_micromods()
    man = load_manifest()
    products = sorted(CATALOGUE) if which == "all" else [which]
    total = {"made": 0, "exists": 0, "would": 0, "skipped": 0}

    for product in products:
        entries = CATALOGUE.get(product)
        if not entries:
            print("%s: not in the catalogue — try --discover %s"
                  % (product, product))
            continue
        pkgs = titles.get(product, [])
        fams = sorted({p.split("-")[0] for p in pkgs if p.split("-")[0] in FAMILIES})
        if not fams:
            print("%s: not installed here, skipping" % product)
            total["skipped"] += 1
            continue
        print("%s  (families installed: %s)" % (product, ", ".join(fams)))
        for slot, name, mdltype, depends in entries:
            for fam in fams:
                pkgid = "%s-%s-%s" % (fam, product, slot)
                if depends:
                    r = make_cartridge_stub(fam, product, dry)
                    if r == "made":
                        man["made"].append("%s-%s-999999" % (fam, product))
                        print("    + %-28s (cartridge stub)"
                              % ("%s-%s-999999" % (fam, product)))
                r = make_package(pkgid, product, name, mdltype, depends, dry)
                total[r] = total.get(r, 0) + 1
                mark = {"made": "+", "exists": "=", "would": "?"}[r]
                print("    %s %-28s %s" % (mark, pkgid, name))
                if r == "made":
                    man["made"].append(pkgid)
        if pkgid in inst:
            pass
    if not dry:
        save_manifest(man)
    print("\nmade %d, already there %d%s"
          % (total["made"], total["exists"],
             ", would make %d" % total["would"] if dry else ""))
    if total["made"]:
        print("Turn them on inside the game's own Micromods menu — that "
              "choice is per profile\nand this tool does not touch profiles.")




def cmd_ingest(src, dry):
    """Install real micromod packages: .tar archives, or unpacked directories.

    This is the trustworthy path — the meta.inf comes from LeapFrog rather
    than from this file's template — so it is what to use whenever the actual
    package is available."""
    man = load_manifest()
    made = 0
    cands = []
    for root, _dirs, files in os.walk(src):
        for fn in files:
            if fn.endswith((".tar", ".lf2", ".lf3")):
                cands.append(os.path.join(root, fn))
        if "meta.inf" in files:
            cands.append(root)
    for c in sorted(set(cands)):
        fields, members = {}, None
        if os.path.isdir(c):
            fields = meta_fields(os.path.join(c, "meta.inf"))
        else:
            try:
                with tarfile.open(c) as t:
                    names = t.getnames()
                    metan = next((n for n in names
                                  if os.path.basename(n) == "meta.inf"), None)
                    if not metan:
                        continue
                    f = t.extractfile(metan)
                    text = f.read().decode("utf-8", "replace") if f else ""
                    for line in text.splitlines():
                        k, _, v = line.partition("=")
                        if k.strip():
                            fields[k.strip()] = v.strip().strip('"')
                    members = (c, names, os.path.dirname(metan))
            except (tarfile.TarError, OSError) as e:
                # .lf3 IS ENCRYPTED, and an Application usually. Reporting a
                # five-line tarfile traceback for each one buried the actual
                # results; a micromod that ships as .lf3 (they exist — Clam
                # Prix's expansion is one) needs tools/lf3.py run over it
                # first, so say that once and move on.
                if c.endswith(".lf3"):
                    print("  . %s: encrypted, run tools/lf3.py on it first"
                          % os.path.basename(c))
                else:
                    print("  ! %s: %s" % (os.path.basename(c), str(e).split("\n")[0]))
                continue
        if fields.get("Type") != "MicroDownload":
            continue
        pkgid = fields.get("PackageID")
        if not pkgid:
            continue
        dest = os.path.join(DOWNLOADS, pkgid)
        if os.path.exists(dest):
            print("  = %-28s already installed" % pkgid)
            continue
        if dry:
            print("  ? %-28s %s" % (pkgid, fields.get("Name", "")))
            continue
        os.makedirs(dest)
        if members:
            path, names, prefix = members
            with tarfile.open(path) as t:
                for n in names:
                    m = t.getmember(n)
                    if not m.isfile():
                        continue
                    rel = os.path.relpath(n, prefix) if prefix else n
                    if rel.startswith(".."):
                        continue
                    out = os.path.join(dest, rel)
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    ex = t.extractfile(m)
                    if ex:
                        with open(out, "wb") as w:
                            shutil.copyfileobj(ex, w)
        else:
            for fn in os.listdir(c):
                s = os.path.join(c, fn)
                if os.path.isfile(s):
                    shutil.copy2(s, os.path.join(dest, fn))
        man["made"].append(pkgid)
        made += 1
        access = " +access" if ensure_access(dest) else ""
        print("  + %-28s %s%s" % (pkgid, fields.get("Name", ""), access))
    if not dry:
        save_manifest(man)
    print("installed %d" % made)


def install_blob(pkgid, blob, dry):
    """Unpack one downloaded package into Downloads. -> 'made'|'exists'|'would'"""
    dest = os.path.join(DOWNLOADS, pkgid)
    if os.path.exists(dest):
        return "exists"
    if dry:
        return "would"
    t = tarfile.open(fileobj=io.BytesIO(blob))
    names = t.getnames()
    metan = next(n for n in names if os.path.basename(n) == "meta.inf")
    prefix = os.path.dirname(metan)
    os.makedirs(dest)
    for n in names:
        m = t.getmember(n)
        if not m.isfile():
            continue
        rel = os.path.relpath(n, prefix) if prefix else n
        if rel.startswith(".."):
            continue
        out = os.path.join(dest, rel)
        os.makedirs(os.path.dirname(out), exist_ok=True)
        ex = t.extractfile(m)
        if ex:
            with open(out, "wb") as w:
                shutil.copyfileobj(ex, w)
    # WITHOUT THIS THE DOWNLOAD IS INERT. See ensure_access — the CDN copy
    # carries no access field at all, and the firmware drops such a package
    # before the title is asked about it.
    ensure_access(dest)
    return "made"


def cmd_scan_online(product, refresh, install, only, dry):
    """Scan a title's micromods on LeapFrog's server, and optionally take them.

    --scan on its own is read-only: it downloads each package into the cache
    and prints what is there, so the choosing can be done with the real names
    in front of you rather than slot numbers."""
    titles = installed_titles()
    products = sorted(titles) if product == "all" else [product]
    if product != "all" and product not in titles:
        print("%s: not installed here — micromods are only useful for a title "
              "you own" % product)
        return
    man = load_manifest()
    total = 0
    for prod in products:
        pkgs = titles.get(prod, [])
        name = ""
        for p in pkgs:
            name = meta_fields(os.path.join(PROGRAMFILES, p, "meta.inf")).get("Name", "")
            if name:
                break
        print("%s  %s" % (prod, name))
        found, ok = scan_online(prod, refresh=refresh)
        if not ok:
            print("  (server unreachable — nothing concluded about this title)")
            return
        mods = [(k, f, n) for k, f, n in found
                if f.get("Type") == "MicroDownload"]
        deps = [(k, f, n) for k, f, n in found
                if f.get("Type") != "MicroDownload"]
        if not mods:
            print("  no micromods on the server for this title")
            continue
        total += len(mods)
        if not install:
            print("  %d available%s — --scan %s --install to take them"
                  % (len(mods), ", plus a cartridge stub" if deps else "", prod))
            continue
        inst = installed_micromods()
        # The CartridgeData stub first: the micromods declare Depends on it,
        # and a dependency naming an absent package is exactly the kind of
        # thing a picker filters out quietly.
        # --only NAMES MICROMODS, NOT DEPENDENCIES. The stub is not a micromod:
        # the screen does not count it, does not let you tick it, and the
        # micromods are the reason it is wanted at all — so filtering the
        # picked slots through it meant an install from the UI could never
        # produce one, while `--install` without --only always did. Two
        # different results from the same tool for the same title.
        #
        # No package LeapFrog currently serves declares Depends, so this is
        # belt-and-braces rather than a fix for something observed failing;
        # the LFConnect copies that do declare it are the reason the stub is
        # fetched in the first place.
        for pkgid, fields, _names, is_dep in \
                [(k, f, n, True) for k, f, n in deps] + \
                [(k, f, n, False) for k, f, n in mods]:
            if only and not is_dep and pkgid.rsplit("-", 1)[1] not in only:
                continue
            blob = open(cached(pkgid), "rb").read()
            r = install_blob(pkgid, blob, dry)
            mark = {"made": "+", "exists": "=", "would": "?"}[r]
            print("  %s %-28s %s" % (mark, pkgid, fields.get("Name", "")))
            if r == "made":
                man["made"].append(pkgid)
        if not dry:
            save_manifest(man)
    if not install and total:
        print("\ncached under runtime/micromods-cache — re-scanning costs "
              "LeapFrog nothing.")
        print("every request sent is logged to runtime/micromods-requests.log")


def cmd_fix_access(dry):
    """Repair micromods already installed without an access field.

    Only ever ADDS the field, and only to a package that has neither — a
    package that already names its profiles is left exactly as it is."""
    fixed, ok = 0, 0
    for pkgid, fields in sorted(installed_micromods().items()):
        dest = os.path.join(DOWNLOADS, pkgid)
        has = fields.get("ProfileAccess") or fields.get("DeviceAccess") == "1"
        if has:
            ok += 1
            continue
        print("  %s %-28s %s" % ("?" if dry else "+", pkgid, fields.get("Name", "")))
        if not dry:
            ensure_access(dest)
        fixed += 1
    print("%s %d, already had one %d"
          % ("would fix" if dry else "fixed", fixed, ok))
    if fixed and not dry:
        print("These were being discarded by the firmware before the title "
              "saw them.\nRun the game and look again.")


def cmd_uninstall(dry):
    man = load_manifest()
    if not man["made"]:
        print("nothing to undo — this tool has not made anything here")
        return
    gone = 0
    for pkgid in sorted(set(man["made"])):
        dest = os.path.join(DOWNLOADS, pkgid)
        if not os.path.isdir(dest):
            continue
        print("  - %s" % pkgid)
        if not dry:
            shutil.rmtree(dest)
        gone += 1
    if not dry:
        save_manifest({"made": []})
    print("removed %d" % gone)


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("--enable", metavar="PRODUCTID|all")
    ap.add_argument("--ingest", metavar="DIR")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("--fix-access", action="store_true",
                    help="add ProfileAccess to installed micromods missing it")
    ap.add_argument("--scan", metavar="PRODUCTID|all",
                    help="ask LeapFrog's server what this title's micromods are")
    ap.add_argument("--install", action="store_true",
                    help="with --scan: install what the scan found")
    ap.add_argument("--only", metavar="SLOT[,SLOT...]",
                    help="with --install: just these slots, e.g. 000009,000011")
    ap.add_argument("--refresh", action="store_true",
                    help="with --scan: re-download instead of using the cache")
    ap.add_argument("-n", "--dry-run", action="store_true")
    a = ap.parse_args()

    if not os.path.isdir(BULK):
        print("no system files at %s — install firmware first." % SYSROOT,
              file=sys.stderr)
        return 1
    os.makedirs(DOWNLOADS, exist_ok=True)

    if a.uninstall:
        cmd_uninstall(a.dry_run)
    elif a.fix_access:
        cmd_fix_access(a.dry_run)
    elif a.scan:
        only = set(s.strip() for s in a.only.split(",")) if a.only else None
        cmd_scan_online(a.scan, a.refresh, a.install, only, a.dry_run)
    elif a.ingest:
        cmd_ingest(a.ingest, a.dry_run)
    elif a.enable:
        cmd_enable(a.enable, a.dry_run)
    else:
        cmd_scan()
    return 0


if __name__ == "__main__":
    sys.exit(main())
