#!/usr/bin/env python3
"""micromods.py — turn on the bonus content the device was never connected to get.

    ./tools/micromods.py                      what is installed, what is missing
    ./tools/micromods.py --ingest DIR         install real micromod packages
    ./tools/micromods.py --enable 0x00180025  write the catalogued ones
    ./tools/micromods.py --enable all
    ./tools/micromods.py --discover 0x00180025    candidate names, for review
    ./tools/micromods.py --uninstall          undo exactly what this tool made

WHAT A MICROMOD IS, read off this firmware rather than guessed. LeapFrog's
bonus content — alternate music, expansion tracks, home-screen themes — is a
package with NO CONTENT IN IT:

    LF/Bulk/Downloads/<PackageID>/
        meta.inf              ~260 bytes, the whole thing
        packagefiles.md5      one md5 per file
        <preview>.png         optional, for the picker

The game already ships the material. SpongeBob's KART_TRACK_EXPANSION unpacks
to a meta.inf, a 70x69 icon and a checksum, while the four expansion tracks it
"delivers" are sitting in the base package's own Data/Sound/Material/voice/ the
whole time. The package is a flag saying the child earned it, not a delivery
mechanism — which is why writing the flag is enough.

HOW A TITLE FINDS THEM. sys::getMicroModsPath() builds the Downloads path,
DjMicroMod::readPackageInfo() reads what is sitting in it, and isExist() is
answered by presence. What the game matches on is the meta.inf `Name` — the
uppercase token, e.g. KART_TRACK_EXPANSION, which is in the title's own binary.
The lowercase `MDLType` beside it is NOT in the binary and appears to be for
LFConnect's benefit.

The player then switches a micromod on in the game's own Micromods menu, and
that choice is per profile: MicromodState::serialize() writes it into the
profile's save data. So this tool makes content AVAILABLE. Turning it on is
still done in the game, which is also why nothing here touches a profile.

WHY THERE IS A CATALOGUE INSTEAD OF A CLEVER SCAN. A title's micromod names
can only be guessed from outside it. Clam Prix's binary holds 123 strings
shaped like a micromod name and exactly 4 of them are one, so a scan that
installs what it finds would write 119 packages that mean nothing. --discover
prints candidates and installs none of them; the catalogue below holds only
entries taken from real packages or from what is already installed here.

NOTHING IS OVERWRITTEN and everything is logged. A package this tool did not
create is never touched, and --uninstall works from the manifest it wrote, so
it cannot remove a micromod that came off the real device.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
SYSROOT = os.environ.get("TADPOLE_SYSROOT") or os.path.join(PROJ, "runtime", "sysroot")
BULK = os.environ.get("TADPOLE_BULK") or os.path.join(SYSROOT, "LF", "Bulk")
DOWNLOADS = os.path.join(BULK, "Downloads")
PROGRAMFILES = os.path.join(BULK, "ProgramFiles")
MANIFEST = os.path.join(PROJ, "runtime", "micromods-made.json")

# ---- the catalogue --------------------------------------------------------
#
# ProductID -> list of (slot, Name, MDLType, depends_on_cartridge)
#
# ONLY ENTRIES WITH A PROVENANCE. Every one below was read out of a real
# LFConnect package or off a package already installed on this firmware. When
# a title's micromods are not known, it does not get a guess — it gets left
# out, and --discover tells you what to go and confirm.
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
    ],
}

# The families a LeapPad-generation title is packaged for. A micromod has to
# match the family of the build that actually runs, which is why the same
# unlock ships as both LPAD- and MULT-: installing only one leaves the other
# build seeing nothing.
FAMILIES = ("LPAD", "MULT", "MHRS", "PADS", "PAD2")

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


def write_checksums(pkgdir):
    """packagefiles.md5 in the device's own format: `<md5>  ./<name>`."""
    names = sorted(n for n in os.listdir(pkgdir) if n != "packagefiles.md5")
    with open(os.path.join(pkgdir, "packagefiles.md5"), "w") as f:
        for n in names:
            p = os.path.join(pkgdir, n)
            if os.path.isfile(p):
                f.write("%s  ./%s\n" % (md5_file(p), n))


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
    print("\n--enable <ProductID> writes them. --discover <ProductID> lists "
          "candidates for a title\nthat is not catalogued yet.")


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


def cmd_discover(product):
    titles = installed_titles()
    pkgs = titles.get(product, [])
    if not pkgs:
        print("%s: not installed here" % product)
        return
    for p in pkgs:
        d = os.path.join(PROGRAMFILES, p)
        if not has_micromod_code(d):
            print("%s: no micromod code in this build" % p)
            continue
        toks = caps_tokens(d)
        print("%s: %d candidate names — READ THESE, do not trust them" % (p, len(toks)))
        for t in toks:
            print("    %s" % t)
        print("  Most are not micromods. The real ones are the tokens the "
              "title's Micromods\n  menu offers; confirm against a real "
              "package before adding to CATALOGUE.")


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
                print("  ! %s: %s" % (os.path.basename(c), e))
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
        print("  + %-28s %s" % (pkgid, fields.get("Name", "")))
    if not dry:
        save_manifest(man)
    print("installed %d" % made)


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
    ap.add_argument("--discover", metavar="PRODUCTID")
    ap.add_argument("--ingest", metavar="DIR")
    ap.add_argument("--uninstall", action="store_true")
    ap.add_argument("-n", "--dry-run", action="store_true")
    a = ap.parse_args()

    if not os.path.isdir(BULK):
        print("no system files at %s — install firmware first." % SYSROOT,
              file=sys.stderr)
        return 1
    os.makedirs(DOWNLOADS, exist_ok=True)

    if a.uninstall:
        cmd_uninstall(a.dry_run)
    elif a.ingest:
        cmd_ingest(a.ingest, a.dry_run)
    elif a.discover:
        cmd_discover(a.discover)
    elif a.enable:
        cmd_enable(a.enable, a.dry_run)
    else:
        cmd_scan()
    return 0


if __name__ == "__main__":
    sys.exit(main())
