#!/usr/bin/env python3
"""Tadpole — turn LFConnect's encrypted .lf3 downloads into installable tars.

    lf3.py <file.lf3|dir> [...] [-o OUTDIR]
    lf3.py --list <file.lf3|dir> [...]        just say what they are
    lf3.py --optional ...                     no key configured is not an error

WHAT AN .lf3 IS. A digital purchase from LFConnect, as opposed to a .lf2 or
.lfp, which are not encrypted. Measured on a real file rather than assumed:

    bytes 0..15     the AES-CTR initialisation vector, in the clear
    bytes 16..      AES-128-CTR ciphertext of a bzip2 stream, which
                    decompresses to an ordinary tar with meta.inf at the top

THE KEY IS NOT SHIPPED, and this tool is not wired into the installer or the
AppImage. Everything above is standard cryptography; the key is the one part
that came from LeapFrog, and a key whose job is to unlock protected content is
not the same kind of thing as a file format — shipping it in a program other
people download is a question worth not answering carelessly.

It is not on the device to be recovered, either: searched byte for byte, the
extracted 4.6.0.784 firmware does not contain it. So there is no "dump it from
your own hardware" path the way there is for the firmware itself. Supply it
yourself and it works; see KEY_HELP below.

WHY THIS EXISTS AT ALL: tools/install-content.sh handles .lf2 and .lfp and
skips .lf3 entirely, so any title delivered as a digital purchase — including
the ones bundled free with the device — was invisible to the installer. That
looked like "the emulator does not support those games" and was really "the
installer could not read the envelope".

THE DEVICE ASSET. Most titles ship as a pair: the game itself, and a small
`DeviceAsset` package holding its home-screen icon and button-map metadata.
The two are merged into one tar here, which is what LeapPad Manager would have
installed.

  meta.inf gets `DeviceAccess=1` appended — without it the home picker filters
  the title out, the same way ProfileAccess does in tools/install-game.sh.

  ONLY THE TOP-LEVEL meta.inf IS REPLACED. The DeviceAsset's manifest is called
  `DAmeta.inf`, which also ends in "meta.inf" — matching on the suffix
  overwrites it with the GAME's manifest, so the asset package ends up
  claiming to be Type="Application" instead of Type="DeviceAsset". Whether
  anything downstream reads it is not obvious, and a package that lies about
  its own type is not a thing to ship on purpose.
"""

import bz2
import os
import re
import sys
import tarfile

CHUNK = 1 << 20

KEY_HELP = """No decryption key configured, so .lf3 packages were skipped.

NOTHING IS BROKEN. Tadpole does not need this: firmware installs, cartridge
backups install, and the emulator runs exactly as before. .lf3 is one extra
source of titles — digital purchases from LFConnect — and without a key those
files are simply left alone.

Tadpole does not ship the key. It is the one LeapFrog-derived ingredient in an
otherwise standard AES-CTR-over-bzip2 format, and it is not recoverable from
the device: searched byte for byte, the extracted firmware does not contain it.
It comes from LFConnect, the PC software.

Supply it once and this works:

    tools/lf3.py --key <32 hex characters> ...
    TADPOLE_LF3_KEY=<32 hex characters> tools/lf3.py ...
    or put it in ~/.config/tadpole/lf3.key

The files it decrypts are already yours; this only reads the envelope."""


def load_key(explicit=None):
    """The key, from the argument, the environment, or the config file."""
    if explicit:
        src = explicit
    elif os.environ.get("TADPOLE_LF3_KEY"):
        src = os.environ["TADPOLE_LF3_KEY"]
    else:
        cfg = os.path.join(
            os.environ.get("XDG_CONFIG_HOME") or
            os.path.expanduser("~/.config"), "tadpole", "lf3.key")
        try:
            with open(cfg) as f:
                src = f.read()
        except OSError:
            return None
    src = "".join(c for c in src if c in "0123456789abcdefABCDEF")
    if len(src) != 32:
        return None
    return bytes.fromhex(src)


def _cipher(key, iv):
    from cryptography.hazmat.backends import default_backend
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    return Cipher(algorithms.AES(key), modes.CTR(iv),
                  backend=default_backend()).decryptor()


def decrypt(path, key, progress=True):
    """-> the plain tar bytes. One streaming pass: read, decrypt, inflate."""
    # Only when someone is watching: \r does nothing useful in a log
    # file or a pipe, where it turns one line into seventy.
    progress = progress and sys.stdout.isatty()
    total = os.path.getsize(path)
    out, done = [], 16
    with open(path, "rb") as f:
        dec = _cipher(key, f.read(16))
        bz = bz2.BZ2Decompressor()
        while True:
            chunk = f.read(CHUNK)
            if not chunk:
                break
            # Past the bzip2 end-of-stream marker the decompressor raises if
            # given anything more, including b"".
            if not bz.eof:
                out.append(bz.decompress(dec.update(chunk)))
            done += len(chunk)
            if progress and total:
                pct = 100 * done // total
                sys.stdout.write("\r  decrypting %3d%%" % min(pct, 100))
                sys.stdout.flush()
    if progress:
        sys.stdout.write("\r                    \r")
        sys.stdout.flush()
    return b"".join(out)


def meta_of(blob, names=("meta.inf",)):
    """A manifest out of a tar held in memory, as text.

    `names` because a DeviceAsset package does not contain a "meta.inf" at
    all — its manifest is called "DAmeta.inf". Looking only for the former
    returns nothing for every asset package, so their Type and ProductID were
    never read and no icon was ever matched to a title.
    """
    with tarfile.open(fileobj=_BytesIO(blob)) as t:
        members = [m for m in t.getmembers() if m.isfile()]
        for want in names:                      # exact, top level first
            for m in members:
                if m.name.lstrip("./") == want:
                    return t.extractfile(m).read().decode("utf-8", "replace")
        for want in names:                      # then nested
            for m in members:
                if os.path.basename(m.name) == want:
                    return t.extractfile(m).read().decode("utf-8", "replace")
    return ""


def fields(text):
    """meta.inf as a dict. QUOTES ARE OPTIONAL — that is not a style choice,
    it is what the files contain:

        Name="Alphabet Stew"
        ProductID=0x00180007
        DeviceAccess=1

    Reading only the quoted form silently loses ProductID, which is the key
    the DeviceAsset package is matched on — so every title came out with no
    icon and the home screen drew the crossed-out "MISS?" placeholder instead.
    Nothing errored; the field simply was not there.
    """
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or "=" not in line:
            continue
        k, _, v = line.partition("=")
        out[k.strip()] = v.strip().strip('"')
    return out


def safe(name):
    out = "".join(c for c in name if c not in '<>:"/\\|?*' and 32 <= ord(c) < 127)
    return out.strip() or "Untitled"


class _BytesIO:
    """io.BytesIO, imported lazily to keep the header of this file readable."""
    def __new__(cls, b):
        import io
        return io.BytesIO(b)


def find_device_asset(lf3_path, product_id):
    """The .lf2 holding this title's icon, if it is sitting alongside.

    Two ways, because the download cache is hash-named and a hand-sorted
    folder is not: the naming convention when the filename carries the
    PackageID, and otherwise every .lf2 in the directory read for a matching
    ProductID.
    """
    d = os.path.dirname(os.path.abspath(lf3_path))
    base = os.path.basename(lf3_path)

    guess = base.replace("-000000.lf3", "-DA0000.lf2").replace("MULT", "PADS")
    cand = os.path.join(d, guess)
    if guess != base and os.path.exists(cand):
        return cand

    if not product_id:
        return None
    for f in sorted(os.listdir(d)):
        if not f.lower().endswith(".lf2"):
            continue
        p = os.path.join(d, f)
        try:
            with open(p, "rb") as fh:
                blob = bz2.decompress(fh.read())
            fl = fields(meta_of(blob, ("DAmeta.inf", "meta.inf")))
        except Exception:
            continue
        if fl.get("Type") == "DeviceAsset" and fl.get("ProductID") == product_id:
            return p
    return None


def build(lf3_path, outdir, key, quiet=False):
    """-> (output path, name) or (None, reason)."""
    blob = decrypt(lf3_path, key, progress=not quiet)
    if not blob:
        return None, "produced nothing — wrong key, or not an .lf3"

    meta = meta_of(blob)
    fl = fields(meta)
    name = safe(fl.get("Name") or fl.get("PackageID") or "Untitled")
    pid = fl.get("PackageID", "")
    system = pid.split("-")[0] if "-" in pid else "LF"

    if "DeviceAccess" not in fl:
        meta = meta.rstrip("\n") + '\nDeviceAccess=1\n'
    meta_bytes = meta.encode("utf-8")

    da = find_device_asset(lf3_path, fl.get("ProductID"))
    da_blob = None
    if da:
        try:
            with open(da, "rb") as fh:
                da_blob = bz2.decompress(fh.read())
        except Exception as e:
            print("  device asset %s unreadable: %s" % (os.path.basename(da), e))

    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, "%s_%s.tar" % (system, name))
    n = 1
    while os.path.exists(out):
        out = os.path.join(outdir, "%s_%s_%d.tar" % (system, name, n))
        n += 1

    with tarfile.open(out, "w") as new:
        with tarfile.open(fileobj=_BytesIO(blob)) as old:
            for m in old.getmembers():
                if m.isfile() and m.name.lstrip("./") == "meta.inf":
                    m.size = len(meta_bytes)
                    new.addfile(m, _BytesIO(meta_bytes))
                elif m.isfile():
                    new.addfile(m, old.extractfile(m))
                else:
                    new.addfile(m)
        if da_blob:
            with tarfile.open(fileobj=_BytesIO(da_blob)) as dat:
                for m in dat.getmembers():
                    if m.isfile():
                        new.addfile(m, dat.extractfile(m))
                    else:
                        new.addfile(m)
    return out, name


def walk(paths):
    for p in paths:
        if os.path.isdir(p):
            for f in sorted(os.listdir(p)):
                if f.lower().endswith(".lf3"):
                    yield os.path.join(p, f)
        elif p.lower().endswith(".lf3"):
            yield p


def main(argv):
    args, outdir, listonly, keyarg, optional = [], None, False, None, False
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == "-o":
            outdir = argv[i + 1]; i += 2
        elif a == "--list":
            listonly = True; i += 1
        elif a == "--key":
            keyarg = argv[i + 1]; i += 2
        elif a == "--optional":
            optional = True; i += 1
        else:
            args.append(a); i += 1
    if not args:
        sys.stderr.write(__doc__)
        return 2

    key = load_key(keyarg)
    if not key:
        # OPTIONAL BY DESIGN. Anything that calls this as one step of a larger
        # install must be able to carry on without it — the firmware installed
        # perfectly well before .lf3 was readable at all, and a missing key is
        # a smaller library, not a failure. --optional says the caller knows
        # that; a direct invocation still reports it as not having done what
        # was asked.
        sys.stderr.write(KEY_HELP + "\n")
        return 0 if optional else 2

    files = list(walk(args))
    if not files:
        print("no .lf3 files found")
        return 1
    outdir = outdir or os.path.join(os.getcwd(), "installables")

    print("%d encrypted package(s)" % len(files))
    ok = 0
    for n, f in enumerate(files, 1):
        print("[%d/%d] %s" % (n, len(files), os.path.basename(f)))
        try:
            if listonly:
                fl = fields(meta_of(decrypt(f, key)))
                print("    %-34s %-12s %s"
                      % (fl.get("Name", "?"), fl.get("Type", "?"),
                         fl.get("PackageID", "?")))
                ok += 1
                continue
            out, name = build(f, outdir, key)
            if out:
                print("    %-34s -> %s" % (name, os.path.basename(out)))
                ok += 1
            else:
                print("    failed: %s" % name)
        except Exception as e:
            print("    failed: %s" % e)
    print("\n%d of %d" % (ok, len(files)))
    if not listonly and ok:
        print("install with:  ./tools/install-game.sh %s/*.tar" % outdir)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
