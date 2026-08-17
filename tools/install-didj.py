#!/usr/bin/env python3
"""Tadpole — Didj support: the compatibility files, and Didj game installs.

    ./tools/install-didj.py --setup DIDJ.zip [ControlOverlay.zip]
    ./tools/install-didj.py --status
    ./tools/install-didj.py <game.zip|game.tar> [more...]

A faithful port of tools/install-didj.sh, which stays the Linux entry point.
This exists because Windows has no bash: the viewer there runs Python for its
tools, the same way the game and firmware installers already do. Behaviour,
output format and destination rules are deliberately identical — the shell
script's comments carry the reasoning and are not repeated here, only
summarised:

  * A Didj game is a 2008 LeapFrog handheld's package. The LeapPad2 firmware
    already loads Didj compatibility data at startup ("End Load DidjPatches"
    appears on every stock boot); what a fresh install lacks is the DATA, which
    is LeapFrog's and cannot ship with Tadpole.
  * ONCE: the compatibility files (DidjAvatars, DidjMDLs, DidjPatches) go in
    LF/Base. PER GAME: install, then edit meta.inf and add a controller overlay.
  * meta.inf edits: Device="Didj" -> "LeapsterExplorer" or it will not launch;
    the guide's blanket delete of every .png line, which also takes Icon= and
    PreviewImage= with it; append ProfileAccess or the picker filters the title
    out of every profile; then put the artwork lines back naming the title's OWN
    icon, falling back to the overlay's generic art only when it has none.
    Without them the home tile is a red X reading "MISS?".
  * The tile itself is IconPADS in GameInfo.json, drawn at native size, so the
    title's 64x64 icon is resampled to the 82x88 every stock package ships and
    written over BaseIcon.png. --refresh redoes just that step for titles
    installed before this tool learned to.
  * The folder is named by the 3LD, not the PackageID, because DidjPatches is
    indexed by that three-letter code.
  * The save area Bulk/Data/Local/<profile>/<PackageID>/ must exist before first
    launch, for the reason install-game.py spells out.
"""
import json
import os
import re
import shutil
import struct
import zlib
import sys
import tarfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import netssl  # noqa: E402  — a sibling tool, not a package

PROJ = os.path.dirname(HERE)

# WHERE THE TWO PIECES COME FROM, and why they are two different questions.
#
# The controller overlay was drawn by a member of the Tadpole Discord for this
# purpose. It is their own work, carries no LeapFrog artwork and no characters,
# and is published for anyone to use — so Tadpole can fetch it outright.
#
# DIDJ.zip is LeapFrog's: the Didj compatibility data that shipped on the
# Leapster Explorer. The URL is the community upload the written guide points
# at, and the item it sits in ("leappad3capturesetup") is what it sounds like —
# a rig somebody built to run Didj games on a LeapPad 3, which does not support
# them either, and the same files turned out to be what Tadpole needs. It is
# here because that is what people are following today.
#
# The cleaner answer is to pull the Leapster Explorer firmware from LeapFrog's
# own digitalcontent server, exactly as tools/online-update.py already does for
# the LeapPad2, and extract the Didj files locally — no redistribution at all.
# That is worth doing and is not done yet.
#
# Both were checked live before being written down: HEAD 200, 11550867 and 54739
# bytes, byte-identical in size to the copies the guide's own steps produce.
DIDJ_URL = "https://archive.org/download/leappad3capturesetup/DIDJ.zip"
OVERLAY_URL = "https://archive.org/download/control-overlay/ControlOverlay.zip"
# Beside the firmware downloads, and gitignored for the same reason.
CACHE = os.path.join(PROJ, "sources", "didj")
BULK = os.environ.get("TADPOLE_BULK") or os.path.join(PROJ, "runtime", "sysroot", "LF", "Bulk")
BASE = os.environ.get("TADPOLE_BASE") or os.path.join(PROJ, "runtime", "sysroot", "LF", "Base")
DIDJ_DIR = os.environ.get("TADPOLE_DIDJ") or os.path.join(PROJ, "runtime", "didj")
OVERLAY = os.path.join(DIDJ_DIR, "overlay")


def die(msg):
    print("install-didj: " + msg, file=sys.stderr)
    sys.exit(1)


def field(meta, name):
    m = re.search(r'%s="([^"]*)"' % re.escape(name), meta)
    return m.group(1) if m else ""


def didj_ready():
    return os.path.isdir(os.path.join(BASE, "DidjPatches"))


def overlay_ready():
    return os.path.isfile(os.path.join(OVERLAY, "GameInfo.json"))


# ---- archives --------------------------------------------------------------
#
# Chosen by what the file IS, not by its extension: the community's Didj dumps
# circulate as .zip while the rest of Tadpole's library is LFManager .tar, and a
# .zip named .tar should not become a support question.
def is_zip(path):
    try:
        with open(path, "rb") as f:
            return f.read(2) == b"PK"
    except OSError:
        return False


def arc_open(path):
    return zipfile.ZipFile(path) if is_zip(path) else tarfile.open(path)


def arc_list(arc):
    if isinstance(arc, zipfile.ZipFile):
        return arc.namelist()
    return arc.getnames()


def arc_read(arc, member):
    if isinstance(arc, zipfile.ZipFile):
        return arc.read(member).decode("utf-8", "replace")
    f = arc.extractfile(member)
    return f.read().decode("utf-8", "replace") if f else ""


def safe_names(arc, prefix):
    """(member, relative path) pairs under `prefix`, nothing escaping `dest`.

    A dump is somebody else's archive: a member named ../../etc/passwd is not a
    hypothetical, it is why this exists rather than a bare extractall().
    """
    if isinstance(arc, zipfile.ZipFile):
        entries = [(n, n, n.endswith("/")) for n in arc.namelist()]
    else:
        entries = [(i, i.name, i.isdir()) for i in arc.getmembers()
                   if i.isfile() or i.isdir()]
    for member, name, isdir in entries:
        if prefix != ".":
            if name == prefix:
                continue
            if not name.startswith(prefix + "/"):
                continue
            name = name[len(prefix) + 1:]
        name = name.lstrip("/")
        if not name or name.startswith("../") or "/../" in name:
            continue
        yield member, name, isdir


def arc_extract(arc, prefix, dest):
    for member, name, isdir in safe_names(arc, prefix):
        out = os.path.join(dest, name.replace("/", os.sep))
        if isdir:
            os.makedirs(out, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(out), exist_ok=True)
        if isinstance(arc, zipfile.ZipFile):
            with arc.open(member) as src, open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)
        else:
            src = arc.extractfile(member)
            if not src:
                continue
            with open(out, "wb") as dst:
                shutil.copyfileobj(src, dst)


def metas(arc):
    """Every package manifest, never the overlay's own DAmeta.inf."""
    return [n for n in arc_list(arc)
            if re.search(r"(^|/)meta\.inf$", n) and not n.endswith("/DAmeta.inf")]


# ---- the home-screen tile --------------------------------------------------
#
# WHY THIS IS HERE AND NOT A ONE-LINER AGAINST PILLOW.
#
# The home picker draws a package's tile from the IconPADS file named in its
# GameInfo.json, and it draws it AT NATIVE SIZE — it does not scale. Every stock
# package therefore ships an 82x88 BaseIcon.png (the Camera's is 83x88), and a
# Didj title's own 64x64 IconNormal.png, dropped in unchanged, renders as a small
# badge floating in the middle of a big empty card. Measured, not guessed: that
# is exactly what it looked like.
#
# So the icon has to be resampled at install time, and it has to happen with no
# dependency beyond the standard library. Pillow is present on this development
# machine and absent from the Python the AppImage and the Windows build carry, so
# reaching for it would make the tile work here and quietly fail for everyone who
# installs Tadpole the normal way. zlib is enough.
TILE_W, TILE_H = 82, 88


def _png_read(path):
    """-> (w, h, RGBA bytes). 8-bit PNGs, the five colour types that occur."""
    d = open(path, "rb").read()
    if d[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, pal, trns = 8, [], None, None
    w = h = depth = ctype = 0
    while pos + 8 <= len(d):
        ln = struct.unpack(">I", d[pos:pos + 4])[0]
        typ = d[pos + 4:pos + 8]
        body = d[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype, _comp, _filt, interlace = struct.unpack(">IIBBBBB", body[:13])
            if depth != 8 or interlace:
                raise ValueError("only 8-bit non-interlaced PNGs")
        elif typ == b"PLTE":
            pal = body
        elif typ == b"tRNS":
            trns = body
        elif typ == b"IDAT":
            idat.append(body)
        elif typ == b"IEND":
            break
        pos += 12 + ln
    raw = zlib.decompress(b"".join(idat))
    nch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    stride = w * nch
    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    o = 0
    for y in range(h):
        f = raw[o]; o += 1
        line = bytearray(raw[o:o + stride]); o += stride
        # PNG per-scanline filters (RFC 2083 §6). Undone in place, exactly as
        # the spec defines them; a wrong Paeth here shows up as a smeared icon.
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            b = prev[i]
            c = prev[i - nch] if i >= nch else 0
            if f == 1:   line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 255
        for x in range(w):
            s = x * nch
            j = (y * w + x) * 4
            if ctype == 6:
                out[j:j+4] = line[s:s+4]
            elif ctype == 2:
                out[j:j+3] = line[s:s+3]; out[j+3] = 255
            elif ctype == 0:
                v = line[s]; out[j] = out[j+1] = out[j+2] = v; out[j+3] = 255
            elif ctype == 4:
                v = line[s]; out[j] = out[j+1] = out[j+2] = v; out[j+3] = line[s+1]
            else:                                    # 3: palette
                idx = line[s]
                if pal and idx * 3 + 2 < len(pal):
                    out[j:j+3] = pal[idx*3:idx*3+3]
                out[j+3] = trns[idx] if (trns and idx < len(trns)) else 255
        prev = line
    return w, h, bytes(out)


def _png_write(path, w, h, px):
    rows = bytearray()
    for y in range(h):
        rows.append(0)                               # filter 0: none
        rows += px[y * w * 4:(y + 1) * w * 4]

    def chunk(t, data):
        c = t + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)

    open(path, "wb").write(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b""))


# WHERE THE ART GOES INSIDE THE TILE, measured off the stock packages rather
# than chosen. Seven of them, every one shipped by LeapFrog:
#
#   MULT-0x00180007  82x88  content 60x55  margins L11 R11 T11 B22
#   MULT-0x00230003  83x88          53x52          L16 R14 T11 B25
#   PAD2-0x001F0005  83x88          58x52          L13 R12 T9  B27
#   PADS-0x001B0036  83x88          55x57          L12 R16 T9  B22
#   PADS-0x001F0006  83x88          63x52          L11 R9  T11 B25
#   PADS-0x001F0007  82x88          66x62          L9  R7  T4  B22
#   PADS-0x001F0008  83x88          61x57          L11 R11 T8  B23
#
# So the convention is a ~60x55 box, centred horizontally, sitting high: the
# bottom margin is consistently double the top. That asymmetry is not an
# accident of the artwork — it is the same on all seven — so the tile is copied
# rather than centred naively.
CONTENT_W, CONTENT_H, CONTENT_TOP = 60, 55, 9


def _opaque_bbox(w, h, px):
    """Bounding box of visible pixels, or None when the image is empty."""
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        row = y * w
        for x in range(w):
            if px[(row + x) * 4 + 3] > 8:
                if x < minx: minx = x
                if x > maxx: maxx = x
                if y < miny: miny = y
                if y > maxy: maxy = y
    if maxx < 0:
        return None
    return minx, miny, maxx, maxy


def make_tile(src, dst, tw=TILE_W, th=TILE_H):
    """Resample `src` into a `tw`x`th` RGBA home-screen tile. -> True on success.

    CROPPED TO THE VISIBLE ART FIRST, and that is the whole trick. Sonic's
    IconNormal.png is a 64x64 canvas whose opaque pixels are a 40x40 badge
    anchored at (0,0) — a quarter of the image, hard against the top-left corner,
    with 24px of nothing to the right and below. Scaling the CANVAS and centring
    that leaves the badge visibly up and to the left in the finished card, which
    is exactly how it rendered before this function learned to crop.

    ASPECT PRESERVED. The art is square and the content box is not; stretching
    would turn a round badge into an oval, which reads as a bug rather than a
    design.

    Bilinear, not nearest: this is an upscale (40 -> 55, 1.4x), and nearest
    leaves ragged edges on exactly the circular logos Didj titles use.
    """
    try:
        sw, sh, sp = _png_read(src)
    except Exception:
        return False
    if not sw or not sh:
        return False

    box = _opaque_bbox(sw, sh, sp)
    if box is None:                                  # fully transparent
        return False
    bx0, by0, bx1, by1 = box
    cw, ch = bx1 - bx0 + 1, by1 - by0 + 1

    scale = min(float(CONTENT_W) / cw, float(CONTENT_H) / ch)
    dw = max(1, int(cw * scale + 0.5))
    dh = max(1, int(ch * scale + 0.5))
    ox = (tw - dw) // 2                              # centred across
    oy = CONTENT_TOP + (CONTENT_H - dh) // 2         # centred in the content box

    out = bytearray(tw * th * 4)                     # transparent
    for y in range(dh):
        fy = by0 + (y + 0.5) / scale - 0.5
        y0 = int(fy) if fy >= by0 else by0
        if y0 > by1: y0 = by1
        y1 = min(y0 + 1, by1)
        wy = fy - y0
        if wy < 0: wy = 0.0
        if wy > 1: wy = 1.0
        ty = y + oy
        if ty < 0 or ty >= th:
            continue
        for x in range(dw):
            fx = bx0 + (x + 0.5) / scale - 0.5
            x0 = int(fx) if fx >= bx0 else bx0
            if x0 > bx1: x0 = bx1
            x1 = min(x0 + 1, bx1)
            wx = fx - x0
            if wx < 0: wx = 0.0
            if wx > 1: wx = 1.0
            tx = x + ox
            if tx < 0 or tx >= tw:
                continue
            i00 = (y0 * sw + x0) * 4; i01 = (y0 * sw + x1) * 4
            i10 = (y1 * sw + x0) * 4; i11 = (y1 * sw + x1) * 4
            j = (ty * tw + tx) * 4
            for c in range(4):
                top = sp[i00 + c] * (1 - wx) + sp[i01 + c] * wx
                bot = sp[i10 + c] * (1 - wx) + sp[i11 + c] * wx
                out[j + c] = int(top * (1 - wy) + bot * wy + 0.5)
    _png_write(dst, tw, th, bytes(out))
    return True


def download(url, dest):
    """Fetch `url` to `dest`, reporting progress. -> the path.

    PROGRESS IN LINES, not a bar: the viewer shows this tool's stdout in a
    scrolling panel, so a carriage-return bar would arrive as one enormous line.
    Every 2 MB is frequent enough to look alive and rare enough to stay short.
    """
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    tmp = dest + ".part"
    print("  fetching %s" % url, flush=True)
    try:
        with netssl.urlopen(url, timeout=120) as r, open(tmp, "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            got = next_mark = 0
            while True:
                chunk = r.read(65536)
                if not chunk:
                    break
                f.write(chunk)
                got += len(chunk)
                if got >= next_mark:
                    if total:
                        print("  %d%% (%.1f of %.1f MB)"
                              % (got * 100 // total, got / 1e6, total / 1e6),
                              flush=True)
                    else:
                        print("  %.1f MB" % (got / 1e6), flush=True)
                    next_mark = got + 2_000_000
    except Exception as e:
        # NAME THE TRANSPORT FAILURE. "Could not download" sends people checking
        # a network that is fine; netssl.explain knows the difference between no
        # route and a certificate this machine cannot verify.
        if os.path.exists(tmp):
            os.remove(tmp)
        die("download failed: %s" % netssl.explain(e))
    os.replace(tmp, dest)
    print("  saved %s (%.1f MB)" % (dest, os.path.getsize(dest) / 1e6))
    return dest


def fetch_compat():
    print("Downloading the Didj compatibility files")
    z = download(DIDJ_URL, os.path.join(CACHE, "DIDJ.zip"))
    setup([z])
    return 0


def fetch_overlay():
    print("Downloading the Didj controller overlay")
    z = download(OVERLAY_URL, os.path.join(CACHE, "ControlOverlay.zip"))
    install_overlay(z)
    print()
    print("Controller overlay ready.")
    return 0


# SEPARATELY SETTABLE, because the two pieces are two different downloads and
# the wizard picks one file at a time. Someone who already has the compatibility
# files from following the guide by hand needs only this half.
def install_overlay(overlayzip):
    if not os.path.isfile(overlayzip):
        die("no such file: " + overlayzip)
    with arc_open(overlayzip) as arc:
        if not any("GameInfo.json" in x for x in arc_list(arc)):
            die("%s has no GameInfo.json — that is not the controller "
                "overlay" % os.path.basename(overlayzip))
        shutil.rmtree(OVERLAY, ignore_errors=True)
        os.makedirs(OVERLAY, exist_ok=True)
        arc_extract(arc, ".", OVERLAY)
    print("  controller overlay staged in %s" % OVERLAY)


# ---- one-time setup --------------------------------------------------------
def setup(argv):
    if not argv:
        die("usage: install-didj.py --setup DIDJ.zip [ControlOverlay.zip]")
    didjzip = argv[0]
    overlayzip = argv[1] if len(argv) > 1 else None
    if not os.path.isfile(didjzip):
        die("no such file: " + didjzip)
    if not os.path.isdir(BASE):
        die("no LF/Base at %s — install the system firmware first" % BASE)

    # Validate before writing: the wrong zip scatters junk through the firmware
    # tree and the failure would not surface until a game refused to start.
    with arc_open(didjzip) as arc:
        if not any(n.startswith("DidjPatches/") for n in arc_list(arc)):
            die("%s has no DidjPatches/ — that is not the Didj compatibility "
                "package" % os.path.basename(didjzip))
        print("Installing Didj compatibility files into %s" % BASE)
        arc_extract(arc, ".", BASE)
    patches = os.path.join(BASE, "DidjPatches")
    n = len([d for d in os.listdir(patches)
             if os.path.isdir(os.path.join(patches, d))]) if os.path.isdir(patches) else 0
    print("  DidjAvatars, DidjMDLs, DidjPatches (%d title patch(es))" % n)

    if overlayzip:
        install_overlay(overlayzip)

    print()
    if overlay_ready():
        print("Didj support is ready. Install games with:")
        print("  ./tools/install-didj.py <game.zip>")
    else:
        print("Compatibility files installed, but NO controller overlay was given.")
        print("Didj games need one — they use shoulder buttons the LeapPad2 lacks.")
        print("Re-run with:  install-didj.py --setup %s ControlOverlay.zip" % didjzip)


# ---- per-game install ------------------------------------------------------
def install_one(arc, metapath):
    prefix = os.path.dirname(metapath) or "."
    meta = arc_read(arc, metapath)
    if field(meta, "Device") != "Didj":
        return 0                       # not ours; install-game.py has it

    pid = field(meta, "PackageID")
    name = field(meta, "Name")
    tld = field(meta, "3LD") or field(meta, "ShortName") or pid
    # Read these BEFORE the .png delete wipes them.
    own_icon = field(meta, "Icon")
    own_preview = field(meta, "PreviewImage")
    if not tld:
        return 0
    dest = os.path.join(BULK, "ProgramFiles", tld)

    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest, exist_ok=True)
    arc_extract(arc, prefix, dest)

    # ---- the meta.inf edits, in the order the guide gives them -------------
    mpath = os.path.join(dest, "meta.inf")
    with open(mpath, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().splitlines()
    out = []
    for ln in lines:
        if re.match(r'\s*Device\s*=\s*"Didj"', ln):
            ln = re.sub(r'"Didj"', '"LeapsterExplorer"', ln)
        if ".png" in ln:
            continue
        out.append(ln)
    if not any(ln.startswith("ProfileAccess=") for ln in out):
        out.append("ProfileAccess=-1,0,1,2,3")

    # ---- the overlay -------------------------------------------------------
    if overlay_ready():
        for root, _dirs, files in os.walk(OVERLAY):
            rel = os.path.relpath(root, OVERLAY)
            tgt = dest if rel == "." else os.path.join(dest, rel)
            os.makedirs(tgt, exist_ok=True)
            for fn in files:
                shutil.copy2(os.path.join(root, fn), os.path.join(tgt, fn))

    # ---- the artwork, the title's own first --------------------------------
    # See install-didj.sh for why the deleted lines have to come back at all.
    def pick(own, fallback):
        if own and os.path.isfile(os.path.join(dest, own)):
            return own
        return fallback if os.path.isfile(os.path.join(dest, fallback)) else ""

    art = pick(own_icon, "iconLPAD.png")
    if art:
        out.append('Icon="%s"' % art)
    art = pick(own_preview, "previewimage.png")
    if art:
        out.append('PreviewImage="%s"' % art)

    # ---- THE HOME-SCREEN TILE ----------------------------------------------
    #
    # IconPADS in GameInfo.json is what the picker actually draws, at native
    # size, so the title's own 64x64 icon has to be resampled up to the 82x88
    # every stock package ships. Overwriting BaseIcon.png rather than repointing
    # IconPADS keeps the file name the one every other package on the device
    # uses, so nothing downstream has to know a Didj title is unusual.
    #
    # Falls back silently to the overlay's generic tile: a dump whose icon is
    # missing or in a format the reader does not take still gets a card rather
    # than a red X, and that is worth more than insisting on the real artwork.
    if own_icon:
        src = os.path.join(dest, own_icon)
        if os.path.isfile(src):
            make_tile(src, os.path.join(dest, "BaseIcon.png"))
    # LABEL THE ROW "Didj". The picker prints IconLabel down the side of the
    # tile — "Game", "eBook", "Creativity" on stock titles — so this is a free,
    # honest place to say what the title actually is.
    gpath = os.path.join(dest, "GameInfo.json")
    if os.path.isfile(gpath):
        try:
            with open(gpath, "r", encoding="utf-8", errors="replace") as f:
                info = json.load(f)
            info["IconLabel"] = "Didj"
            with open(gpath, "w", encoding="utf-8") as f:
                json.dump(info, f, indent=1)
        except (ValueError, OSError):
            pass          # a GameInfo we cannot parse is not worth failing over

    with open(mpath, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

    # The save area, keyed by PackageID, which the meta.inf keeps even though
    # the folder does not.
    if pid:
        local = os.path.join(BULK, "Data", "Local")
        if os.path.isdir(local):
            for prof in os.listdir(local):
                p = os.path.join(local, prof)
                if os.path.isdir(p):
                    os.makedirs(os.path.join(p, pid), exist_ok=True)

    print("  %-12s %-26s %s" % ("Didj", tld, name))
    return 1


def refresh_icons():
    """Re-do the artwork step on Didj titles already installed. -> exit code.

    THE UPGRADE PATH. Earlier builds of this tool pointed every Didj title at the
    overlay's generic "GAME ICON" card, because the artwork question had not been
    worked out yet. Those installs are still sitting on people's home screens and
    reinstalling them means finding the original dump again — which for most
    people means finding a Didj and dumping it again.
    Nothing here needs the dump: the package already contains its own icon.

    A Didj install is recognised by the overlay we put there — iconLPAD.png beside
    a PADS/buttonMap.json — so this cannot mistake a stock LeapFrog title for one
    of ours and repaint it.
    """
    pf = os.path.join(BULK, "ProgramFiles")
    if not os.path.isdir(pf):
        die("no ProgramFiles at %s" % pf)
    done = 0
    for name in sorted(os.listdir(pf)):
        d = os.path.join(pf, name)
        if not (os.path.isfile(os.path.join(d, "iconLPAD.png"))
                and os.path.isfile(os.path.join(d, "PADS", "buttonMap.json"))):
            continue
        # IconNormal.png is the Didj packaging convention — Sonic and Super
        # Chicks both use it — and it is what the original meta.inf named before
        # the .png lines were deleted.
        own = os.path.join(d, "IconNormal.png")
        if os.path.isfile(own) and make_tile(own, os.path.join(d, "BaseIcon.png")):
            mp = os.path.join(d, "meta.inf")
            try:
                with open(mp, "r", encoding="utf-8", errors="replace") as f:
                    lines = f.read().splitlines()
                lines = [l for l in lines if not l.startswith("Icon=")]
                lines.append('Icon="IconNormal.png"')
                with open(mp, "w", encoding="utf-8") as f:
                    f.write("\n".join(lines) + "\n")
            except OSError:
                pass
            gp = os.path.join(d, "GameInfo.json")
            try:
                with open(gp, "r", encoding="utf-8", errors="replace") as f:
                    info = json.load(f)
                info["IconLabel"] = "Didj"
                with open(gp, "w", encoding="utf-8") as f:
                    json.dump(info, f, indent=1)
            except (ValueError, OSError):
                pass
            print("  %-12s %s" % ("Didj", name))
            done += 1
    print()
    print("refreshed %d Didj tile(s)" % done)
    return 0


def is_didj_archive(path):
    """Does this archive hold a Didj package? Cheap enough to ask of every file.

    Imported by install-game.py, which routes them here rather than installing a
    package that would sit on the home screen and refuse to start.
    """
    try:
        with arc_open(path) as arc:
            for m in metas(arc):
                if field(arc_read(arc, m), "Device") == "Didj":
                    return True
    except (tarfile.TarError, zipfile.BadZipFile, OSError):
        return False
    return False


def install_archives(paths):
    """Install Didj games. Raises SystemExit (via die) when setup is missing."""
    if not didj_ready():
        die('Didj compatibility files are not installed.\n'
            '  Run:  install-didj.py --setup DIDJ.zip ControlOverlay.zip\n'
            '  (or use the setup wizard\'s "Didj games" page)')

    installed = 0
    for i, path in enumerate(paths, 1):
        if not os.path.isfile(path):
            print("no such file: " + path, file=sys.stderr)
            continue
        print("[%d/%d] %s:" % (i, len(paths), os.path.basename(path)), flush=True)
        try:
            with arc_open(path) as arc:
                for m in metas(arc):
                    installed += install_one(arc, m)
        except (tarfile.TarError, zipfile.BadZipFile, OSError) as e:
            print("  cannot read %s: %s" % (os.path.basename(path), e),
                  file=sys.stderr)

    if not installed:
        print()
        print('No Didj packages found. A Didj game\'s meta.inf says Device="Didj";')
        print("for anything else use ./tools/install-game.py")
        return 1
    print()
    print("installed %d Didj title(s) into %s"
          % (installed, os.path.join(BULK, "ProgramFiles")))
    return 0


def main():
    argv = sys.argv[1:]
    if not argv:
        die("usage: install-didj.py --setup DIDJ.zip [ControlOverlay.zip] "
            "| --status | <game.zip> [...]")
    if argv[0] == "--setup":
        setup(argv[1:])
        return 0
    if argv[0] == "--overlay":
        if len(argv) < 2:
            die("usage: install-didj.py --overlay ControlOverlay.zip")
        print("Staging the Didj controller overlay")
        install_overlay(argv[1])
        return 0
    # The shell entry point calls this for the one step it cannot do itself.
    if argv[0] == "--make-tile":
        if len(argv) < 3:
            die("usage: install-didj.py --make-tile <src.png> <dst.png>")
        return 0 if make_tile(argv[1], argv[2]) else 1
    if argv[0] == "--refresh":
        print("Refreshing Didj home-screen tiles")
        return refresh_icons()
    if argv[0] == "--fetch-compat":
        return fetch_compat()
    if argv[0] == "--fetch-overlay":
        return fetch_overlay()
    if argv[0] == "--status":
        patches = os.path.join(BASE, "DidjPatches")
        n = len([d for d in os.listdir(patches)
                 if os.path.isdir(os.path.join(patches, d))]) if os.path.isdir(patches) else 0
        print("compat=%s patches=%d" % ("yes" if didj_ready() else "no", n))
        print("overlay=%s" % ("yes" if overlay_ready() else "no"))
        return 0

    return install_archives(argv)


if __name__ == "__main__":
    sys.exit(main())
