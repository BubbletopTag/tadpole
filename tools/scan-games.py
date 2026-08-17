#!/usr/bin/env python3
"""Tadpole — turn a folder of game backups into a list of icons and names.

    scan-games.py <folder> [--cache DIR] [--force]

WHY. Installing a game meant picking a .tar out of a file list, and a file list
is the worst possible view of a game library: the names are whatever the person
who dumped the cartridge typed, several titles differ only by a version number
in brackets, and nothing tells you which ones you already have. The cartridge
itself carries a name and an icon — the same ones the LeapPad shows on its home
screen — so this reads those out and the viewer draws them.

WHAT IT PRODUCES, in <cache> (default ~/.cache/tadpole/games):

    index.tsv     one line per title: name, package, version, size, icon, path
    i/<key>.tpi   the icon, decoded and scaled, in the trivial format below
    e/<key>.rec   the per-archive cache entry, keyed on path+mtime+size

Rescanning a folder that has not changed costs one stat() per archive. That
matters more than it sounds: these backups are 20-120 MB each and a library of
forty is seven gigabytes, so a full scan is tens of seconds and a user who
opens the picker twice should pay it once.

WHICH meta.inf. A backup can hold several packages — the game, a shared
library pack, a microphone widget — each with its own manifest. The one that
describes the TITLE is the shallowest Type="Application" that names an Icon;
the widgets name none, and the library packs are Type="Download" or "System".

ICONS ARE NORMALISED HERE, NOT IN THE VIEWER. They arrive as 8-bit RGBA PNG,
8-bit RGB PNG, and — for two Disney titles — a Flash movie called icon64.swf.
Decoding that zoo in C, in a viewer whose one PNG decoder exists to load a
single known file, is a poor trade. Python has zlib and this file has a
decoder; what reaches the viewer is:

    'TPI1' | u16 width | u16 height | width*height*4 bytes of RGBA

which is twenty lines to read and impossible to get wrong.
"""

import hashlib
import os
import re
import struct
import sys
import tarfile
import zlib

ICON_MAX = 96                     # tile art is drawn at 40-64px; 96 leaves room
INDEX_VERSION = 1


def log(msg):
    # Line-buffered: the viewer reads this pipe and shows it as progress, so a
    # line that sits in a buffer is a progress panel that looks stuck.
    sys.stdout.write(msg + "\n")
    sys.stdout.flush()


# ---- PNG -----------------------------------------------------------------

def png_decode(data):
    """-> (w, h, bytearray RGBA) or None if this is not a PNG we can read."""
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, pal, trns = 8, [], None, None
    w = h = depth = ctype = interlace = 0
    while pos + 8 <= len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + n]
        if typ == b"IHDR":
            w, h = struct.unpack(">II", chunk[:8])
            depth, ctype, interlace = chunk[8], chunk[9], chunk[12]
        elif typ == b"PLTE":
            pal = chunk
        elif typ == b"tRNS":
            trns = chunk
        elif typ == b"IDAT":
            idat.append(chunk)
        elif typ == b"IEND":
            break
        pos += 12 + n
    if not w or not h or not idat:
        return None
    if interlace:
        return None               # Adam7: not worth it for an icon
    if depth not in (1, 2, 4, 8, 16):
        return None

    chans = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(ctype)
    if chans is None:
        return None

    raw = zlib.decompress(b"".join(idat))
    # Bytes per pixel for the filter's "left" reference, minimum 1.
    bpp = max(1, (chans * depth) // 8)
    stride = (w * chans * depth + 7) // 8

    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        ft = raw[p]; p += 1
        line = bytearray(raw[p:p + stride]); p += stride
        if ft == 1:
            for i in range(bpp, stride):
                line[i] = (line[i] + line[i - bpp]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - bpp] if i >= bpp else 0
                c = prev[i - bpp] if i >= bpp else 0
                b = prev[i]
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            return None
        out[y * stride:(y + 1) * stride] = line
        prev = line

    # -> RGBA
    rgba = bytearray(w * h * 4)
    for y in range(h):
        row = out[y * stride:(y + 1) * stride]
        for x in range(w):
            if depth == 8:
                o = x * chans
                s = row[o:o + chans]
            elif depth == 16:
                o = x * chans * 2
                s = row[o:o + chans * 2:2]        # high byte is close enough
            else:                                  # 1/2/4, palette or grey
                per = 8 // depth
                byte = row[x // per]
                shift = 8 - depth * (x % per + 1)
                s = [(byte >> shift) & ((1 << depth) - 1)]
            d = (y * w + x) * 4
            if ctype == 0:
                v = s[0] if depth == 8 else s[0] * 255 // ((1 << depth) - 1)
                rgba[d:d + 4] = bytes((v, v, v, 255))
            elif ctype == 2:
                rgba[d:d + 4] = bytes((s[0], s[1], s[2], 255))
            elif ctype == 3:
                i = s[0]
                if not pal or i * 3 + 2 >= len(pal):
                    return None
                a = trns[i] if (trns and i < len(trns)) else 255
                rgba[d:d + 4] = bytes((pal[i*3], pal[i*3+1], pal[i*3+2], a))
            elif ctype == 4:
                rgba[d:d + 4] = bytes((s[0], s[0], s[0], s[1]))
            else:
                rgba[d:d + 4] = bytes((s[0], s[1], s[2], s[3]))
    return w, h, rgba


def scale_rgba(w, h, px, maxdim):
    """Box-average down to fit maxdim. Icons are photographic, not pixel art:
    dropping every other pixel makes them grainy in a way that reads as a bad
    decode."""
    if w <= maxdim and h <= maxdim:
        return w, h, px
    scale = max(w / maxdim, h / maxdim)
    nw, nh = max(1, int(w / scale)), max(1, int(h / scale))
    out = bytearray(nw * nh * 4)
    for y in range(nh):
        y0, y1 = int(y * h / nh), max(int(y * h / nh) + 1, int((y + 1) * h / nh))
        for x in range(nw):
            x0, x1 = int(x * w / nw), max(int(x * w / nw) + 1, int((x + 1) * w / nw))
            r = g = b = a = n = 0
            for yy in range(y0, min(y1, h)):
                base = yy * w * 4
                for xx in range(x0, min(x1, w)):
                    o = base + xx * 4
                    r += px[o]; g += px[o+1]; b += px[o+2]; a += px[o+3]; n += 1
            o = (y * nw + x) * 4
            if n:
                out[o:o+4] = bytes((r // n, g // n, b // n, a // n))
    return nw, nh, out


def write_tpi(path, w, h, px):
    with open(path, "wb") as f:
        f.write(b"TPI1" + struct.pack("<HH", w, h))
        f.write(bytes(px))


# ---- archives ------------------------------------------------------------



def parse_meta(blob):
    # These manifests are UTF-8 — "Disney Princesses - Histoires et aventures
    # en livre animé" is in the library — but a few are not valid UTF-8, so
    # fall back rather than lose the whole record to one bad byte.
    try:
        text = blob.decode("utf-8")
    except UnicodeDecodeError:
        text = blob.decode("latin1")
    return dict(re.findall(r'(\w+)="([^"]*)"', text))


# The names an icon goes by, in the order we would rather have them. The Flash
# era titles (Letter Factory, Pet Pals, Up!, the Star Wars readers) name a .swf
# as their Icon and ship the artwork alongside it as PopUpIcon.png or
# BaseIcon.png — fourteen of the eighty-seven here — so "the manifest says swf"
# has to mean "look next to it", not "no icon".
ICON_NAMES = ("icon.png", "game_icon.png", "iconnormal.png", "baseicon.png",
              "baseimage.png", "popupicon.png", "previewimage.png",
              "icon64.png", "th.png")


def pick_icon_member(names, metadir, icon):
    """Where the icon really is, as a name the ARCHIVE will accept.

    meta.inf names its icon relative to its own directory; when that entry is
    missing or is a Flash movie, look for the artwork that sits beside it.

    `names` maps a tidied path to the member's REAL name. Some backups list
    everything as "./LPAD/Icon.png" and some as "LPAD/Icon.png", so matching
    has to happen on the tidied form — but extracting has to use the original,
    which is a distinction that cost two titles their icons the first time
    round: they matched, then raised KeyError on the way out.
    """
    def find(cand):
        c = os.path.normpath(cand).lstrip("./")
        if not c.lower().endswith(".png"):
            return None
        return names.get(c) or names.get(c.lower())

    if icon:
        hit = find(os.path.join(metadir, icon))
        if hit:
            return hit
        # Same name, PNG extension: BaseIcon.swf -> BaseIcon.png, which is
        # exactly how Letter Factory ships it.
        hit = find(os.path.join(metadir, os.path.splitext(icon)[0] + ".png"))
        if hit:
            return hit

    # Then, in order: a file named like an icon, a file whose name merely
    # CONTAINS "icon" (EPR_LargeIcon.png, large_icon.png), and finally a
    # top-level preview. Depth-limited on purpose — several titles carry a
    # coloring book full of preview_page_NN.png, and a random page of content
    # dressed up as the game's icon is worse than no icon at all.
    best = None
    for tidy, real in names.items():
        base = os.path.basename(tidy).lower()
        if not base.endswith(".png"):
            continue
        if base in ICON_NAMES:
            pri = (0, ICON_NAMES.index(base))
        elif "icon" in base:
            pri = (1, 0)
        elif base in ("preview.png", "previewimage.png") and tidy.count("/") <= 1:
            pri = (2, 0)
        else:
            continue
        near = 0 if (metadir and tidy.lower().startswith(metadir.lower())) else 1
        rank = (pri[0], near, pri[1], tidy.count("/"), tidy.lower())
        if best is None or rank < best[0]:
            best = (rank, real)
    return best[1] if best else None


def scan_archive(path):
    """-> dict describing the title, or None if this .tar holds no game."""
    try:
        tf = tarfile.open(path)
    except Exception as e:
        log("  ! %s: %s" % (os.path.basename(path), e))
        return None
    with tf:
        members = tf.getmembers()
        # tidied path -> the name the archive actually knows it by
        names = {m.name.lstrip("./"): m.name for m in members if m.isfile()}
        best = None
        for m in members:
            if not m.isfile() or os.path.basename(m.name) != "meta.inf":
                continue
            try:
                fld = parse_meta(tf.extractfile(m).read())
            except Exception:
                continue
            if fld.get("Type") != "Application":
                continue
            depth = m.name.count("/")
            # An Application WITHOUT an icon is a widget (the microphone one
            # ships inside several titles); prefer anything that has one.
            rank = (0 if fld.get("Icon") else 1, depth)
            if best is None or rank < best[0]:
                best = (rank, m.name, fld)
        if best is None:
            return None
        _, metapath, fld = best
        metadir = os.path.dirname(metapath).lstrip("./")

        rec = {
            "name": fld.get("Name") or os.path.splitext(os.path.basename(path))[0],
            "pid": fld.get("PackageID", ""),
            "version": fld.get("Version", ""),
            "path": os.path.abspath(path),
            "size": os.path.getsize(path),
            "icon": None,
        }
        member = pick_icon_member(names, metadir, fld.get("Icon"))
        if member:
            try:
                img = png_decode(tf.extractfile(member).read())
            except Exception:
                img = None
            if img:
                rec["icon"] = scale_rgba(img[0], img[1], img[2], ICON_MAX)
        return rec


# ---- cache ---------------------------------------------------------------

def key_for(path):
    return hashlib.sha1(os.path.abspath(path).encode("utf-8")).hexdigest()[:16]


def cache_dir():
    """The SAME directory the viewer reads, which is not always ~/.cache.

    tadpole_ui.c's games_cache_dir() walks XDG_CACHE_HOME, then Windows'
    %LOCALAPPDATA%, then ~/.cache. This has to walk it identically or the
    scanner writes an index nothing reads and the library stays empty after a
    scan that reported success. Environment-driven rather than
    platform-driven: LOCALAPPDATA is set on every Windows and on no Linux, so
    the added link changes nothing there.
    """
    xdg = os.environ.get("XDG_CACHE_HOME")
    if xdg:
        return os.path.join(xdg, "tadpole", "games")
    local = os.environ.get("LOCALAPPDATA")
    if local:
        return os.path.join(local, "Tadpole", "cache", "games")
    return os.path.join(os.path.expanduser("~/.cache"), "tadpole", "games")


def load_rec(cache, key, st):
    p = os.path.join(cache, "e", key + ".rec")
    try:
        with open(p, "r", encoding="utf-8") as f:
            d = dict(l.rstrip("\n").split("\t", 1) for l in f if "\t" in l)
    except OSError:
        return None
    if d.get("mtime") != str(int(st.st_mtime)) or d.get("size") != str(st.st_size):
        return None
    return d


def save_rec(cache, key, rec, st):
    os.makedirs(os.path.join(cache, "e"), exist_ok=True)
    with open(os.path.join(cache, "e", key + ".rec"), "w", encoding="utf-8") as f:
        for k in ("name", "pid", "version", "path"):
            f.write("%s\t%s\n" % (k, rec[k]))
        f.write("size\t%d\n" % st.st_size)
        f.write("mtime\t%d\n" % int(st.st_mtime))
        f.write("bytes\t%d\n" % rec["size"])
        f.write("hasicon\t%d\n" % (1 if rec["icon"] else 0))


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    force = "--force" in argv
    cache = cache_dir()
    if "--cache" in argv:
        cache = argv[argv.index("--cache") + 1]
    if not args:
        sys.stderr.write(__doc__)
        return 2
    folder = args[0]
    if not os.path.isdir(folder):
        log("not a folder: %s" % folder)
        return 1

    os.makedirs(os.path.join(cache, "i"), exist_ok=True)
    os.makedirs(os.path.join(cache, "e"), exist_ok=True)

    tars = sorted(f for f in os.listdir(folder) if f.lower().endswith(".tar"))
    if not tars:
        log("no .tar backups in %s" % folder)
        # Still write an empty index: the viewer must be able to tell "scanned,
        # found nothing" from "never scanned".
        open(os.path.join(cache, "index.tsv"), "w").write(
            "#tadpole-game-index %d\n" % INDEX_VERSION)
        return 0

    log("==> %d archive(s) in %s" % (len(tars), folder))
    rows, done = [], 0
    for f in tars:
        done += 1
        full = os.path.join(folder, f)
        try:
            st = os.stat(full)
        except OSError:
            continue
        key = key_for(full)
        rec = None if force else load_rec(cache, key, st)
        if rec:
            rows.append((rec["name"], rec["pid"], rec.get("version", ""),
                         rec.get("bytes", str(st.st_size)),
                         key if rec.get("hasicon") == "1" else "",
                         rec["path"]))
            continue
        log("  [%d/%d] %s" % (done, len(tars), f))
        got = scan_archive(full)
        if not got:
            log("        no game package inside - skipped")
            continue
        if got["icon"]:
            w, h, px = got["icon"]
            write_tpi(os.path.join(cache, "i", key + ".tpi"), w, h, px)
        save_rec(cache, key, got, st)
        rows.append((got["name"], got["pid"], got["version"], str(got["size"]),
                     key if got["icon"] else "", got["path"]))

    rows.sort(key=lambda r: r[0].lower())
    tmp = os.path.join(cache, "index.tsv.tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write("#tadpole-game-index %d\n" % INDEX_VERSION)
        for r in rows:
            # Names come out of a manifest and could in principle contain a
            # tab; the reader splits on tabs, so strip them here.
            f.write("\t".join(x.replace("\t", " ").replace("\n", " ") for x in r))
            f.write("\n")
    os.replace(tmp, os.path.join(cache, "index.tsv"))
    log("==> %d title(s) ready" % len(rows))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
