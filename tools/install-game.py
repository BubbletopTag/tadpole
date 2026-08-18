#!/usr/bin/env python3
"""Tadpole — install a LeapFrog game backup (LFManager-style .tar) into /LF/Bulk.

    ./tools/install-game.py games/FOF.tar [more.tar ...]
    ./tools/install-game.py --from-list list.txt
    ./tools/install-game.py --fix-saves
    ./tools/install-game.py --fix-meta

A faithful port of tools/install-game.sh, which stays the Linux entry point.
This exists because Windows has no bash: the viewer there runs Python for its
tools, the same way the firmware installer already does. Behaviour, output
format and destination rules are deliberately identical — the shell script's
comments carry the reasoning and are not repeated here, only summarised:

  * THREE tar shapes are real: flat (meta.inf on top), self-wrapped
    (<NAME>/meta.inf) and multi-package (several meta.inf). Every meta.inf in
    the archive is a package; installing only the top-level one silently
    installs nothing for two of the three shapes.
  * Destination by Type, from lfpkg: Application -> Bulk/ProgramFiles/<id>,
    System -> Base/<id>, Download|MicroDownload -> Bulk/Downloads/<id>.
  * ProfileAccess is appended to Applications that lack it, or the home picker
    filters the title out.
  * The save area Bulk/Data/Local/<profile>/<PackageID>/ must exist before
    first launch: nothing creates it on demand, and its absence hangs Cooking!
    Recipes on the Road on a white screen with no message.
"""
import os
import re
import shutil
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
BULK = os.environ.get("TADPOLE_BULK") or os.path.join(PROJ, "runtime", "sysroot", "LF", "Bulk")
BASE = os.environ.get("TADPOLE_BASE") or os.path.join(PROJ, "runtime", "sysroot", "LF", "Base")

# BY PATH, because the file is called install-didj.py and a hyphen is not a
# module name. Loading it this way keeps the tools' filenames consistent with
# every other one here rather than renaming one of them to suit an import.
def _load_didj():
    import importlib.util
    spec = importlib.util.spec_from_file_location(
        "install_didj", os.path.join(HERE, "install-didj.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


install_didj = _load_didj()


def field(meta, name):
    """Name="value" out of a meta.inf, first match, empty when absent.

    ANCHORED TO A LINE START — see install-game.sh. Unanchored, asking for Icon
    matched inside LargeIcon="preview.png", which meta.inf lists first, so the
    icon lookup returned a store banner."""
    m = re.search(r'^\s*%s="([^"]*)"' % re.escape(name), meta, re.M)
    return m.group(1) if m else ""


def json_str(text, name):
    """"Key" : "value" out of a JSON object, first match."""
    m = re.search(r'"%s"\s*:\s*"([^"]*)"' % re.escape(name), text)
    return m.group(1) if m else ""


def product_of(pid):
    """`LST3-0x00180002-000000` -> `0x00180002` — see install-game.sh."""
    parts = (pid or "").split("-")
    return parts[1].lower() if len(parts) > 1 else ""


def purge_same_product(pid):
    """Remove any OTHER Application for the same title. See install-game.sh:
    the same game ships under several PackageID prefixes, and installing two of
    them puts the one game on the home screen twice and makes the two entries
    share one save directory."""
    prod = product_of(pid)
    if not prod:
        return
    pf = os.path.join(BULK, "ProgramFiles")
    for entry in sorted(os.listdir(pf)) if os.path.isdir(pf) else []:
        mi = os.path.join(pf, entry, "meta.inf")
        if not os.path.isfile(mi):
            continue
        try:
            with open(mi, "r", encoding="utf-8", errors="replace") as f:
                meta = f.read()
        except OSError:
            continue
        opid = field(meta, "PackageID")
        if field(meta, "Type") != "Application" or not opid or opid == pid:
            continue
        if product_of(opid) != prod:
            continue
        shutil.rmtree(os.path.join(pf, entry), ignore_errors=True)
        print("  %-12s %-26s %s" % ("replaced", opid, field(meta, "Name")),
              flush=True)


ICON_KEYS = ('"IconPADS"', '"IconPHRS"', '"IconTHDS"', '"IconTHD1"')
ICON_NAMES = ("BaseImage.png", "BaseIcon.png", "icon.png", "icon64.png",
              "82x88.png", "PopUpIcon.png")


def pick_icon(dest):
    """A PNG in the package the home screen can draw. See install-game.sh."""
    try:
        with open(os.path.join(dest, "meta.inf"), "r", encoding="utf-8",
                  errors="replace") as f:
            meta = f.read()
    except OSError:
        meta = ""
    try:
        with open(os.path.join(dest, "GameInfo.json"), "r", encoding="utf-8",
                  errors="replace") as f:
            gi = f.read()
    except OSError:
        gi = ""
    # NOT meta.inf's LargeIcon= — see install-game.sh: it names a store banner,
    # and in the Ni Hao backup that banner is Tinker Bell artwork.
    cands = [field(meta, "Icon"), json_str(gi, "LargeIcon")] + list(ICON_NAMES)
    for c in cands:
        if c and c.lower().endswith(".png") and \
           os.path.isfile(os.path.join(dest, c)):
            return c
    try:
        for c in sorted(os.listdir(dest)):
            if "icon" in c.lower() and c.lower().endswith(".png"):
                return c
    except OSError:
        pass
    return ""


def add_icon_key(path, png):
    """Insert "IconPADS": "<png>" as GameInfo.json's first member.

    A TEXT INSERT, not a reserialise — see install-game.sh for why."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace", newline="") as f:
            text = f.read()
    except OSError:
        return
    i = text.find("{")
    if i < 0:
        return
    sep = "," if re.search(r'"[^"]*"\s*:', text) else ""
    # NO NEWLINE IS INTRODUCED — see install-game.sh. Byte-identical output to
    # the shell version is the point of this port.
    new = '%s{"IconPADS": "%s"%s%s' % (text[:i], png, sep, text[i + 1:])
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(new)


def safe_members(tar, prefix):
    """Members under `prefix`, with the wrapper stripped and nothing escaping.

    A backup is somebody else's archive: a member named ../../etc/passwd is
    not a hypothetical, it is the reason this function exists rather than a
    bare extractall().
    """
    for info in tar.getmembers():
        name = info.name
        if prefix != ".":
            if name == prefix:
                continue
            if not name.startswith(prefix + "/"):
                continue
            name = name[len(prefix) + 1:]
        name = name.lstrip("/")
        if not name or name.startswith("../") or "/../" in name:
            continue
        if not (info.isfile() or info.isdir()):
            continue          # devices and links have no business in a package
        yield info, name


def extract(tar, prefix, dest):
    for info, name in safe_members(tar, prefix):
        out = os.path.join(dest, name.replace("/", os.sep))
        if info.isdir():
            os.makedirs(out, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        src = tar.extractfile(info)
        if src is None:
            continue
        with src, open(out, "wb") as f:
            shutil.copyfileobj(src, f)


def profiles():
    """The profiles that EXIST, not an invented 0..3 — see the shell script."""
    local = os.path.join(BULK, "Data", "Local")
    try:
        return [os.path.join(local, d) for d in os.listdir(local)
                if os.path.isdir(os.path.join(local, d))]
    except OSError:
        return []


def install_one(tar, path, metapath):
    prefix = os.path.dirname(metapath) or "."
    try:
        f = tar.extractfile(metapath)
        meta = f.read().decode("utf-8", "replace") if f else ""
    except (KeyError, OSError):
        return
    typ = field(meta, "Type")
    pid = field(meta, "PackageID")
    name = field(meta, "Name")
    if not pid:
        return

    if typ == "Application":
        dest = os.path.join(BULK, "ProgramFiles", pid)
    elif typ == "System":
        dest = os.path.join(BASE, pid)
    elif typ in ("Download", "MicroDownload"):
        dest = os.path.join(BULK, "Downloads", pid)
    else:
        print("  %-12s %-26s %s (skipped)" % (typ, pid, name), flush=True)
        return

    if typ == "Application":
        purge_same_product(pid)

    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest, exist_ok=True)
    extract(tar, prefix, dest)

    mi = os.path.join(dest, "meta.inf")
    if typ == "Application":
        try:
            with open(mi, "r", encoding="utf-8", errors="replace") as f:
                have = any(l.startswith("ProfileAccess=") for l in f)
        except OSError:
            have = True
        if not have:
            # THE NEWLINE IS NOT OPTIONAL — see the note in install-game.sh.
            # Plenty of these files end without one, and appending straight to
            # the end welds two fields into `DeviceAccess=1ProfileAccess=...`,
            # losing both: the title installs and then never appears on the
            # home screen.
            nl = ""
            try:
                with open(mi, "rb") as f:
                    f.seek(0, os.SEEK_END)
                    if f.tell():
                        f.seek(-1, os.SEEK_END)
                        if f.read(1) not in (b"\n", b"\r"):
                            nl = "\n"
            except OSError:
                nl = "\n"
            with open(mi, "a", encoding="utf-8") as f:
                f.write(nl + 'ProfileAccess=-1,0,1,2,3\n')
        for prof in profiles():
            os.makedirs(os.path.join(prof, pid), exist_ok=True)
        # THE MANIFEST IS INPUT, NOT STATE — see install-game.sh at length. The
        # Qt devices' package-manager daemon rewrites every meta.inf at boot
        # from a model with no room for Device=, and a Leapster title that
        # loses its device type loses its on-screen A/B/Home ViewFrame with it.
        # Read-only makes the rewrite fail harmlessly. Only packages that
        # declare a Device= have anything to protect.
        if field(meta, "Device"):
            try:
                os.chmod(mi, 0o444)
            except OSError:
                pass
        # See pick_icon in install-game.sh: a GameInfo.json naming none of the
        # icon keys the firmware reads leaves the tile showing a placeholder.
        gi = os.path.join(dest, "GameInfo.json")
        if os.path.isfile(gi):
            try:
                with open(gi, "r", encoding="utf-8", errors="replace") as f:
                    have = any(k in f.read() for k in ICON_KEYS)
            except OSError:
                have = True
            if not have:
                png = pick_icon(dest)
                if png:
                    add_icon_key(gi, png)
                    print("  %-12s %-26s %s" % ("icon", pid, png), flush=True)

    print("  %-12s %-26s %s" % (typ, pid, name), flush=True)
    dep = field(meta, "Depends")
    if dep:
        print("      needs: %s" % dep, flush=True)


def fix_saves():
    made = 0
    pf = os.path.join(BULK, "ProgramFiles")
    for d in sorted(os.listdir(pf)) if os.path.isdir(pf) else []:
        mi = os.path.join(pf, d, "meta.inf")
        if not os.path.isfile(mi):
            continue
        with open(mi, "r", encoding="utf-8", errors="replace") as f:
            if not re.search(r'^Type="?Application', f.read(), re.M):
                continue
        for prof in profiles():
            p = os.path.join(prof, d)
            if not os.path.isdir(p):
                os.makedirs(p, exist_ok=True)
                made += 1
    print("created %d missing save directories under %s"
          % (made, os.path.join(BULK, "Data", "Local")))
    return 0


def fix_meta():
    """Repair meta.inf files an earlier append welded together.

    The bug is described in install-game.sh: appending ProfileAccess to a file
    that did not end in a newline produced

        DeviceAccess=1ProfileAccess=-1,0,1,2,3

    which loses both fields, and the title stops appearing on the home screen.
    This splits them back apart wherever a field has been run onto the end of
    another one. It is idempotent, and it only touches lines that are actually
    malformed."""
    pf = os.path.join(BULK, "ProgramFiles")
    fixed = 0
    for d in sorted(os.listdir(pf)) if os.path.isdir(pf) else []:
        mi = os.path.join(pf, d, "meta.inf")
        if not os.path.isfile(mi):
            continue
        try:
            with open(mi, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        # ONLY THE FIELD WE APPEND, and only where it is not already at the
        # start of a line. Splitting on any `Word=` found mid-line would be
        # the more general repair and also a way to corrupt a value that
        # legitimately contains one — this fixes the damage this tool caused
        # and nothing else. Idempotent: once split, the lookbehind fails.
        new = re.sub(r'(?<=[^\n])(ProfileAccess=)', r'\n\1', text)
        if new == text:
            continue
        if not new.endswith("\n"):
            new += "\n"
        # install_one makes some manifests read-only (see there); this is the
        # one tool that is supposed to be able to write them anyway.
        mode = os.stat(mi).st_mode & 0o777
        if not mode & 0o200:
            os.chmod(mi, 0o644)
        with open(mi, "w", encoding="utf-8") as f:
            f.write(new)
        if not mode & 0o200:
            os.chmod(mi, mode)
        name = ""
        m = re.search(r'^Name="?([^"\n]*)', new, re.M)
        if m:
            name = m.group(1)
        print("  repaired %-30s %s" % (d, name))
        fixed += 1
    print("repaired %d meta.inf files" % fixed)
    return 0


def main(argv):
    args = argv[1:]
    if not args:
        sys.stderr.write("usage: %s <game.tar> [...]\n" % argv[0])
        return 2
    if args[0] == "--fix-saves":
        return fix_saves()
    if args[0] == "--fix-meta":
        return fix_meta()
    if args[0] == "--from-list":
        # One path per line: twenty ticked titles, each of whose names may hold
        # spaces and brackets, do not belong on a command line.
        if len(args) < 2 or not os.path.isfile(args[1]):
            sys.stderr.write("no such list: %s\n" % (args[1] if len(args) > 1 else ""))
            return 2
        with open(args[1], "r", encoding="utf-8") as f:
            args = [l.strip() for l in f if l.strip()]
        if not args:
            sys.stderr.write("nothing listed\n")
            return 2
        print("installing %d title(s)" % len(args), flush=True)

    total = len(args)
    for n, path in enumerate(args, 1):
        if not os.path.isfile(path):
            sys.stderr.write("no such file: %s\n" % path)
            continue
        # DIDJ PACKAGES GO SOMEWHERE ELSE. They arrive here because this is what
        # the viewer's "Install .tar directly" runs, and a user holding a Didj
        # dump has no reason to know it is a different kind of package. They need
        # a conversion this file does not do — see install_didj — so hand them
        # over rather than installing something that will not launch. The
        # delegate prints its own progress header, hence the early continue.
        if install_didj.is_didj_archive(path):
            try:
                install_didj.install_archives([path])
            except SystemExit:
                pass          # it reported the missing step; keep the batch going
            continue
        print("[%d/%d] %s:" % (n, total, os.path.basename(path)), flush=True)
        try:
            with tarfile.open(path, "r:*") as tar:
                metas = [m.name for m in tar.getmembers()
                         if m.name == "meta.inf" or m.name.endswith("/meta.inf")]
                for m in metas:
                    install_one(tar, path, m)
        except (tarfile.TarError, OSError) as e:
            sys.stderr.write("  cannot read %s: %s\n" % (os.path.basename(path), e))
    print()
    print("installed into %s" % BULK)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
