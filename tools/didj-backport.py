#!/usr/bin/env python3
"""Tadpole — put a LeapsterExplorer title on a Didj.

    ./tools/didj-backport.py install TITLE.tar               # into the emulator's Didj
    ./tools/didj-backport.py install TITLE.tar --out DIR     # a tree to copy onto a real Didj
    ./tools/didj-backport.py install TITLE.tar --as SCP      # the directory name, if meta.inf lacks 3LD=
    ./tools/didj-backport.py check TITLE.tar                 # what the title still lacks
    ./tools/didj-backport.py check --installed               # every title in ProgramFiles
    ./tools/didj-backport.py lib --out DIR                   # only the compatibility library

TITLE is a .tar or .zip of the title as it ships (App.so and meta.inf at the
top), or a directory holding the same. docs/DIDJ-BACKPORT.md is the tutorial;
docs/DIDJ.md has the measurement this is built on.

WHAT "INSTALL" DOES, and it is exactly three things:

  1. Puts the title at /Didj/ProgramFiles/<3LD>/, which is where the Didj's
     shell looks — the three-letter ID from its own meta.inf, the same way
     the stock titles are laid out. Nothing registers a title; the shell
     scans that directory.
  2. Fixes the two meta.inf lines the Didj's shell cannot survive. Explorer
     packages carry a Flash icon (Icon="icon.swf"); the Didj reads every
     title's icon as a PNG and ASSERTS on the first that is not one, before
     it has drawn a menu — the whole shell dies, on every boot, until the
     title is removed. So Icon= is pointed at a 64x64 PNG (the size the
     Didj's own icons are), taken from the package or made from its art,
     and PreviewImage= at a PNG. The original is kept as meta.inf.orig.
  3. Drops libCartridgeMPI.so into /Didj/Base/Brio/lib/. It is not a
     cartridge library: it is the sixteen symbols the Didj's 2009 Brio and
     Lightning framework lack that Explorer titles import, under the one
     library name their App.so asks for and takes nothing from. See
     tadpole/shim/tadpole_didj_backport.c. It is built for the Didj's own
     ARM926 core, so the same file serves the emulator and the hardware.

Nothing in the Didj's firmware is modified. Remove the title's directory
and the library and the device is stock again.

"CHECK" is the measurement behind all of this: every symbol the title's
App.so imports, minus every symbol the Didj's libraries export, minus what
the compatibility library adds. A title that comes out clean will get past
the loader. Whether it then runs is the experiment. One that does not come
out clean says exactly which symbols a bigger compatibility library needs,
which is how the library grew to sixteen in the first place.

Pure stdlib. Demangling uses c++filt when binutils is present and shows the
mangled names otherwise.
"""
import importlib.util
import os
import shutil
import struct
import sys
import tarfile
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from elfsyms import demangle  # noqa: E402

LIB_NAME = "libCartridgeMPI.so"
LIB_SRC = os.path.join(PROJ, "runtime", "didj-backport", LIB_NAME)
DIDJ_LIB_DIRS = [os.path.join("Didj", "Base", "Brio", "lib"),
                 os.path.join("Didj", "Base", "lib"),
                 os.path.join("Didj", "Base", "Brio", "Module"),
                 "lib", os.path.join("usr", "lib")]
# Undefined-and-weak in every title, resolved to nothing by the loader
# without complaint. Reported by nm as missing; they are not.
WEAK_NOISE = {"_Jv_RegisterClasses", "__register_frame_info",
              "__deregister_frame_info"}


def die(msg):
    sys.stderr.write("didj-backport: %s\n" % msg)
    sys.exit(1)


def say(msg=""):
    print(msg)
    sys.stdout.flush()


# ---- the title archive ----------------------------------------------------

class Title(object):
    """A title as shipped: a tar, a zip or a directory, with App.so and
    meta.inf at its top — or one directory down, which some dumps do."""

    def __init__(self, path):
        self.path = path
        self.kind = None
        self.arc = None
        self.prefix = ""
        if os.path.isdir(path):
            self.kind = "dir"
            names = self._walk_dir(path)
        elif zipfile.is_zipfile(path):
            self.kind = "zip"
            self.arc = zipfile.ZipFile(path)
            names = [i.filename for i in self.arc.infolist() if not i.is_dir()]
        elif tarfile.is_tarfile(path):
            self.kind = "tar"
            self.arc = tarfile.open(path)
            names = [m.name for m in self.arc.getmembers() if m.isfile()]
        else:
            die("%s is not a tar, a zip or a directory" % path)
        names = [n.lstrip("./") for n in names]
        # One shared top directory means the title is one level down.
        tops = set(n.split("/", 1)[0] for n in names)
        if len(tops) == 1 and all("/" in n for n in names):
            self.prefix = tops.pop() + "/"
        self.names = [n[len(self.prefix):] for n in names if n.startswith(self.prefix)]
        if "meta.inf" not in self.names or "App.so" not in self.names:
            die("%s has no meta.inf and App.so at its top — not a title package" % path)

    def _walk_dir(self, path):
        out = []
        for root, _, files in os.walk(path):
            for f in files:
                out.append(os.path.relpath(os.path.join(root, f), path))
        return out

    def read(self, name):
        full = self.prefix + name
        if self.kind == "dir":
            return open(os.path.join(self.path, full), "rb").read()
        if self.kind == "zip":
            return self.arc.read(full)
        return self.arc.extractfile(full).read()

    def extract_all(self, dest):
        """Every file, under dest, refusing paths that escape it."""
        n = 0
        for name in self.names:
            if name.startswith("/") or ".." in name.split("/"):
                die("refusing member %r: it escapes the title directory" % name)
            target = os.path.join(dest, name)
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(self.read(name))
            n += 1
        return n


# ---- meta.inf -------------------------------------------------------------

def parse_meta(text):
    fields = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            fields[k.strip()] = v.strip().strip('"')
    return fields


def set_meta(text, key, value):
    """Set key="value", replacing the line if there is one, appending if not.
    Line-based so every other line survives byte for byte."""
    lines = text.splitlines()
    out, done = [], False
    for line in lines:
        if line.split("=", 1)[0].strip() == key:
            if not done:
                out.append('%s="%s"' % (key, value))
                done = True
            continue
        out.append(line)
    if not done:
        out.append('%s="%s"' % (key, value))
    return "\n".join(out) + "\n"


def png_size(path):
    try:
        with open(path, "rb") as f:
            head = f.read(24)
    except OSError:
        return None
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", head[16:24])


def pick_icon(title_dir, fields):
    """-> the name of a 64x64 PNG to use as Icon=, making one if it must."""
    cur = fields.get("Icon", "")
    pngs = {}
    for f in sorted(os.listdir(title_dir)):
        if f.lower().endswith(".png"):
            sz = png_size(os.path.join(title_dir, f))
            if sz:
                pngs[f] = sz
    # A PNG THE PACKAGE ALREADY NAMES IS KEPT, whatever its size. The shell
    # asserts on an icon that is not a PNG; a PNG of another size it simply
    # draws (Clam Prix's 60x57 Game_Icon.png shows on the ring). Replacing
    # it would make the hardware tree differ from what ran in the emulator.
    if cur in pngs:
        return cur, "kept, %dx%d" % pngs[cur]
    sixty_four = [f for f, sz in pngs.items() if sz == (64, 64)]
    iconish = [f for f in sixty_four if "icon" in f.lower()]
    if iconish:
        return iconish[0], "the package's own 64x64 icon"
    if sixty_four:
        return sixty_four[0], "a 64x64 PNG from the package"
    # Nothing the right size: make one from the largest art there is, with
    # the same resampler the LeapPad2 "Didj support" installer uses.
    src = None
    for cand in (fields.get("LargeIcon"), fields.get("PreviewImage"), cur):
        if cand in pngs:
            src = cand
            break
    if src is None and pngs:
        src = max(pngs, key=lambda f: pngs[f][0] * pngs[f][1])
    if src is None:
        return None, "no PNG anywhere in the package"
    spec = importlib.util.spec_from_file_location(
        "install_didj", os.path.join(HERE, "install-didj.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    out = "didj_icon.png"
    if not mod.make_tile(os.path.join(title_dir, src), os.path.join(title_dir, out), 64, 64):
        return None, "could not resample %s" % src
    return out, "made from %s" % src


def fix_meta(title_dir):
    """Point Icon= and PreviewImage= at PNGs. -> list of (key, old, new, why)."""
    mp = os.path.join(title_dir, "meta.inf")
    text = open(mp, "r", errors="replace").read()
    fields = parse_meta(text)
    changes = []
    icon, why = pick_icon(title_dir, fields)
    if icon is None:
        die("cannot choose an icon for %s: %s" % (title_dir, why))
    if icon != fields.get("Icon"):
        changes.append(("Icon", fields.get("Icon"), icon, why))
        text = set_meta(text, "Icon", icon)
    for key in ("PreviewImage", "LargeIcon"):
        cur = fields.get(key)
        ok = cur and png_size(os.path.join(title_dir, cur)) is not None
        if ok:
            continue
        # A PNG the package has for the purpose, else the icon.
        pick = None
        for cand in (fields.get("LargeIcon"), fields.get("PreviewImage")):
            if cand and png_size(os.path.join(title_dir, cand)):
                pick = cand
                break
        if pick is None:
            pick = icon
        if key == "LargeIcon" and cur is None:
            continue                     # the Didj's own titles do not all have one
        changes.append((key, cur, pick, "must be a PNG that exists"))
        text = set_meta(text, key, pick)
    if changes:
        orig = mp + ".orig"
        if not os.path.exists(orig):
            shutil.copy2(mp, orig)
        with open(mp, "w") as f:
            f.write(text)
    return fields, changes


# ---- ELF: what a .so imports and exports ----------------------------------

def elf_dynsyms(path):
    """-> (needed, imports, exports) of a 32-bit little-endian ELF, or None."""
    try:
        d = open(path, "rb").read()
    except OSError:
        return None
    if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
        return None
    e_shoff, = struct.unpack_from("<I", d, 32)
    e_shentsize, e_shnum = struct.unpack_from("<HH", d, 46)
    secs = []
    for i in range(e_shnum):
        o = e_shoff + i * e_shentsize
        sh_name, sh_type, sh_flags, sh_addr, sh_off, sh_size, sh_link, sh_info, sh_align, sh_entsize = \
            struct.unpack_from("<IIIIIIIIII", d, o)
        secs.append((sh_type, sh_off, sh_size, sh_link, sh_entsize))

    def cstr(base, off):
        end = d.find(b"\0", base + off)
        return d[base + off:end].decode("latin-1")

    imports, exports, needed = set(), set(), []
    for sh_type, off, size, link, entsize in secs:
        if sh_type == 11 and entsize:                      # SHT_DYNSYM
            strtab = secs[link][1]
            for j in range(size // entsize):
                o = off + j * entsize
                st_name, st_value, st_size, st_info, st_other, st_shndx = \
                    struct.unpack_from("<IIIBBH", d, o)
                if not st_name:
                    continue
                name = cstr(strtab, st_name)
                bind = st_info >> 4
                if st_shndx == 0:
                    if bind != 2:                          # not STB_WEAK
                        imports.add(name)
                elif bind in (1, 2):
                    exports.add(name)
        elif sh_type == 6 and entsize:                     # SHT_DYNAMIC
            strtab = secs[link][1]
            for j in range(size // entsize):
                tag, val = struct.unpack_from("<iI", d, off + j * entsize)
                if tag == 1:                               # DT_NEEDED
                    needed.append(cstr(strtab, val))
                elif tag == 0:
                    break
    return needed, imports, exports


def didj_exports(root, extra_dirs=()):
    """Every symbol the Didj's libraries export, and the file names on hand."""
    exports, files = set(), set()
    dirs = [os.path.join(root, d) for d in DIDJ_LIB_DIRS] + list(extra_dirs)
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            p = os.path.join(d, f)
            if not os.path.isfile(p) or ".so" not in f:
                continue
            if f == LIB_NAME:
                continue        # counted separately, so the report says what it adds
            files.add(f)
            r = elf_dynsyms(p)
            if r:
                exports |= r[2]
    return exports, files


def check_appso(appso, root, lib_path, label):
    """Print the audit for one App.so. -> number of symbols still missing."""
    r = elf_dynsyms(appso)
    if not r:
        say("  %s: App.so is not a 32-bit ELF" % label)
        return -1
    needed, imports, _ = r
    exports, files = didj_exports(root, [os.path.join(PROJ, "runtime", "shimlibs")])
    lib = elf_dynsyms(lib_path) if lib_path and os.path.isfile(lib_path) else None
    lib_exports = lib[2] if lib else set()
    imports -= WEAK_NOISE
    missing = sorted(imports - exports)
    still = sorted(set(missing) - lib_exports)
    needed_missing = [n for n in needed if n not in files and n != LIB_NAME]
    say("  %s: %d imports, %d resolved by the Didj, %d by %s, %d still missing"
        % (label, len(imports), len(imports) - len(missing),
           len(missing) - len(still), LIB_NAME, len(still)))
    if needed_missing:
        say("  NEEDED but not on the Didj: %s" % " ".join(needed_missing))
    for s in still:
        say("    %s" % demangle(s))
    if not still and not needed_missing:
        say("  -> will load. Whether it runs is the experiment.")
    return len(still) + len(needed_missing)


# ---- commands -------------------------------------------------------------

def emulator_root():
    root = os.path.join(PROJ, "runtime", "sysroot")
    if not os.path.isdir(os.path.join(root, "Didj", "ProgramFiles")):
        die("the live device is not the Didj (no runtime/sysroot/Didj) —\n"
            "  ./tadpole.sh --device didj    switches to it, or use --out DIR")
    return root


def install_lib(root):
    dst_dir = os.path.join(root, "Didj", "Base", "Brio", "lib")
    os.makedirs(dst_dir, exist_ok=True)
    dst = os.path.join(dst_dir, LIB_NAME)
    if not os.path.isfile(LIB_SRC):
        die("%s is missing — build it: cd tadpole && make didj-backport" % LIB_SRC)
    if os.path.isfile(dst) and open(dst, "rb").read() == open(LIB_SRC, "rb").read():
        return dst, False
    shutil.copy2(LIB_SRC, dst)
    return dst, True


def cmd_install(args):
    out = None
    as_id = None
    paths = []
    i = 0
    while i < len(args):
        if args[i] == "--out":
            out = args[i + 1]; i += 2
        elif args[i] == "--as":
            as_id = args[i + 1]; i += 2
        else:
            paths.append(args[i]); i += 1
    if not paths:
        die("install needs a title (.tar, .zip or directory)")
    root = out if out else emulator_root()
    for p in paths:
        t = Title(p)
        fields = parse_meta(t.read("meta.inf").decode("latin-1"))
        # THE DIRECTORY NAME. The Didj's own titles carry 3LD= in meta.inf and
        # live under it; not every Explorer package has the line (Clam Prix
        # has neither 3LD nor ShortName). The shell does not care what the
        # directory is called — it scans ProgramFiles for meta.inf — so take
        # --as, else the directory's own name when installing from one, else
        # the initials of Name=, and say which.
        tld = fields.get("3LD") or as_id
        why = "from meta.inf" if fields.get("3LD") else "from --as"
        if not tld and os.path.isdir(p.rstrip("/")):
            tld = os.path.basename(p.rstrip("/")); why = "the source directory's name"
        if not tld:
            words = [w for w in fields.get("Name", "").replace(":", " ").split() if w[0].isalnum()]
            tld = "".join(w[0] for w in words)[:3].upper(); why = "the initials of Name="
        if not tld:
            die("%s: meta.inf has no 3LD and no Name; pass --as XYZ" % p)
        dest = os.path.join(root, "Didj", "ProgramFiles", tld)
        say("%s" % fields.get("Name", tld))
        say("  Device=%s  PackageID=%s  directory %s (%s)"
            % (fields.get("Device"), fields.get("PackageID"), tld, why))
        if os.path.isdir(dest):
            shutil.rmtree(dest)
        n = t.extract_all(dest)
        say("  %d files -> %s" % (n, dest))
        _, changes = fix_meta(dest)
        for key, old, new, why in changes:
            say("  meta.inf %s: %s -> %s  (%s)" % (key, old, new, why))
        if not changes:
            say("  meta.inf already names PNG art")
        lib, fresh = install_lib(root)
        say("  %s %s" % (lib, "installed" if fresh else "already there"))
        audit_root = root if not out else emulator_root_or_none()
        if audit_root:
            check_appso(os.path.join(dest, "App.so"), audit_root, lib, "check")
    if out:
        say("")
        say("Copy %s/Didj onto the Didj's Didj partition, keeping the paths:" % out)
        say("  Didj/ProgramFiles/<3LD>/...  and  Didj/Base/Brio/lib/%s" % LIB_NAME)
    else:
        say("")
        say("Run it:  ./tadpole.sh --device didj")


def emulator_root_or_none():
    root = os.path.join(PROJ, "runtime", "sysroot")
    if os.path.isdir(os.path.join(root, "Didj", "ProgramFiles")):
        return root
    say("  (no Didj sysroot here to audit against; the check needs the emulator's Didj install)")
    return None


def cmd_check(args):
    root = emulator_root()
    lib = LIB_SRC
    if args == ["--installed"]:
        pf = os.path.join(root, "Didj", "ProgramFiles")
        for d in sorted(os.listdir(pf)):
            appso = os.path.join(pf, d, "App.so")
            if os.path.isfile(appso):
                m = parse_meta(open(os.path.join(pf, d, "meta.inf"), errors="replace").read())
                check_appso(appso, root, lib, "%s (%s, Device=%s)" % (d, m.get("Name"), m.get("Device")))
        return
    if not args:
        die("check needs a title, an App.so, or --installed")
    for p in args:
        if os.path.isfile(p) and p.endswith(".so"):
            check_appso(p, root, lib, p)
            continue
        t = Title(p)
        with tempfile.NamedTemporaryFile(suffix=".so", delete=False) as tmp:
            tmp.write(t.read("App.so"))
            name = tmp.name
        try:
            check_appso(name, root, lib, os.path.basename(p))
        finally:
            os.unlink(name)


def cmd_lib(args):
    out = None
    if args[:1] == ["--out"] and len(args) > 1:
        out = args[1]
    root = out if out else emulator_root()
    lib, fresh = install_lib(root)
    say("%s %s" % (lib, "installed" if fresh else "already there"))


def main():
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0
    cmd, args = sys.argv[1], sys.argv[2:]
    if cmd == "install":
        cmd_install(args)
    elif cmd == "check":
        cmd_check(args)
    elif cmd == "lib":
        cmd_lib(args)
    else:
        die("unknown command %r (install, check, lib)" % cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
