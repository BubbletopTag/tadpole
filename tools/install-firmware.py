#!/usr/bin/env python3
# Tadpole — install LeapPad2 system files, with no shell involved.
#
#   tools/install-firmware.py <LFC_Downloads dir | .lfp | .lf2 | .zip>
#
# You supply the firmware. Tadpole ships no LeapFrog code; this reads the
# packages LFConnect leaves in its download cache on your own machine.
#
# WHY THIS EXISTS ALONGSIDE THE SHELL SCRIPTS
# -------------------------------------------
# install-firmware.sh, runtime/setup-sysroot.sh and install-content.sh do this
# job on Linux and are unchanged. Windows has no shell to run them, and the
# viewer said so honestly rather than half-working:
#
#     cannot run tools/install-firmware.sh — that tool is a shell script
#     and Windows has no shell for it
#
# which left a Windows user able to download firmware and unable to install it.
# This is the three of them as one program, in the same spirit as
# tools/install-game.py. The shell versions remain the Linux entry points.
#
# WHAT THE PACKAGES ARE
#   .lf2   bzip2 tar, despite the extension
#   .lfp   ordinary ZIP
#   .lf3   encrypted; skipped unless keys/lf3.keys exists
#
# Both hold a package directory with a meta.inf manifest. LFConnect's cache is
# a flat pile of hash-named files, so the only way to know what one is, is to
# read the manifest inside. This scans them all.
#
# THE ROOT FILESYSTEM IS A UBIFS VOLUME, not a tar, and reading it needs
# ubi_reader. That is not vendored — writing a UBIFS reader is a project in
# itself and the tool already exists — so if it is missing this says exactly
# that instead of half installing something.
#
# LINKS VERSUS COPIES. The sysroot is a symlink farm on Linux and a copy on
# Windows, which is what setup-sysroot.sh already does for MSYS. Copies cost
# about 76 MB; symlinks cost nothing, and on Linux the rootfs is right there.

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import pkgtool  # noqa: E402  — a sibling tool, not a package

WINDOWS = os.name == "nt"


def say(msg=""):
    print(msg, flush=True)


def die(msg):
    print("error: " + msg, file=sys.stderr, flush=True)
    sys.exit(1)


# ---- reading packages -------------------------------------------------------

def meta_of(path):
    """The package's meta.inf as text, or "" if it has none we can read.

    Read through pkgtool.members() rather than pkgtool.cmd_meta(), which is a
    CLI entry point: it WRITES the manifest to stdout and returns an exit
    status. Calling it as a function yields an int, and the failure surfaces
    much later as a regex complaining it was handed one.
    """
    try:
        for name, read in pkgtool.members(path):
            if os.path.basename(name) == "meta.inf":
                return read().decode("utf-8", "replace")
    except Exception:
        pass
    return ""


def field(meta, name):
    """A manifest field.

    ANCHORED AT LINE START. An unanchored Version= also matches
    MetaVersion="1.0", which is how this once reported the firmware as
    version 1.0.
    """
    m = re.search(r'^%s="([^"]*)"' % re.escape(name), meta, re.M)
    return m.group(1) if m else ""


def extract_pkg(path, dest):
    os.makedirs(dest, exist_ok=True)
    pkgtool.cmd_extract(path, dest)


def members_of(path):
    """Member NAMES. pkgtool.members yields (name, reader) pairs."""
    try:
        return [name for name, _ in pkgtool.members(path)]
    except Exception:
        return []


# ---- filesystem helpers -----------------------------------------------------

def link_or_copy(src, dst):
    """A symlink on Linux, a real copy on Windows.

    Windows can make symlinks only with Developer Mode or elevation, and a
    failure here would leave a sysroot that looks built and is not.
    """
    if not os.path.exists(src):
        return False
    if os.path.islink(dst) or os.path.exists(dst):
        if os.path.isdir(dst) and not os.path.islink(dst):
            shutil.rmtree(dst, ignore_errors=True)
        else:
            try:
                os.remove(dst)
            except OSError:
                pass
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    if WINDOWS:
        if os.path.isdir(src):
            shutil.copytree(src, dst, symlinks=False)
        else:
            shutil.copy2(src, dst)
    else:
        os.symlink(os.path.abspath(src), dst)
    return True


def write_text(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="") as f:
        f.write(text)


def touch(path, size=0):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        if size:
            f.truncate(size)


# ---- 1. gather packages -----------------------------------------------------

PKG_EXT = (".lf2", ".lfp", ".lf3")


def gather(src, stage):
    pkgs = os.path.join(stage, "pkgs")
    os.makedirs(pkgs, exist_ok=True)
    low = src.lower()

    if low.endswith(".zip"):
        say("==> unpacking %s" % src)
        outer = os.path.join(stage, "outer")
        extract_pkg(src, outer)
        for root, _dirs, files in os.walk(outer):
            for fn in files:
                if fn.lower().endswith(PKG_EXT):
                    shutil.copy2(os.path.join(root, fn), pkgs)
    elif low.endswith(PKG_EXT):
        shutil.copy2(src, pkgs)
    elif os.path.isdir(src):
        # LFConnect keeps downloads in <dir>/cache; accept either level.
        for d in (os.path.join(src, "cache"), src):
            if not os.path.isdir(d):
                continue
            for fn in sorted(os.listdir(d)):
                p = os.path.join(d, fn)
                if os.path.isfile(p) and fn.lower().endswith(PKG_EXT):
                    shutil.copy2(p, pkgs)
    else:
        die("not a directory or a known archive: %s" % src)

    found = sorted(os.path.join(pkgs, f) for f in os.listdir(pkgs))
    if not found:
        die("no .lf2 or .lfp packages found in %s" % src)
    say("==> %d package(s) to inspect" % len(found))
    return found


# ---- 2. the base firmware ---------------------------------------------------

def find_firmware(pkgs):
    listing = []
    for p in pkgs:
        meta = meta_of(p)
        if not meta:
            continue
        t, n = field(meta, "Type"), field(meta, "Name")
        listing.append((t, n))
        if t == "DiskImage" and n == "Firmware-Base":
            return p, field(meta, "Version")
    say("no Firmware-Base (Type=DiskImage) package here.")
    say("Packages present:")
    for t, n in listing:
        say("    %-14s %s" % (t, n))
    die("supply the full LFC_Downloads directory, which contains it")


def extract_rootfs(fw, version, stage):
    dest = os.path.join(PROJ, "rootfs", "stock-%s" % version)
    rfs = os.path.join(dest, "ubi_rfs")
    if os.path.isdir(rfs) and os.listdir(rfs):
        say("==> %s already populated — leaving it alone" % rfs)
        say("    (delete it first if you want to re-extract)")
        return dest

    fwdir = os.path.join(stage, "fw")
    extract_pkg(fw, fwdir)

    ubi = kernel = None
    for root, _dirs, files in os.walk(fwdir):
        for fn in files:
            low = fn.lower()
            if ubi is None and low.endswith(".ubi") and "erootfs" in low:
                ubi = os.path.join(root, fn)
            elif ubi is None and low.endswith(".ubi"):
                ubi = os.path.join(root, fn)
            if low.endswith("kernel.bin"):
                kernel = os.path.join(root, fn)
    if not ubi:
        die("no .ubi root filesystem inside Firmware-Base")
    say("  root filesystem: %s (%d bytes)"
        % (os.path.basename(ubi), os.path.getsize(ubi)))
    if kernel:
        say("  kernel: %s" % os.path.basename(kernel))

    say("==> extracting the root filesystem")
    say("    (a 53 MB volume — this takes a minute or two)")
    out = os.path.join(stage, "rfs")
    os.makedirs(out, exist_ok=True)
    try:
        # KEEP THE TOOL'S OWN ERROR. ubi_reader installed without its LZO
        # backend fails with "No module named 'lzallright'", a one-line fix
        # that is invisible if the message is swallowed.
        pkgtool.cmd_ubi(ubi, out)
    except SystemExit:
        raise
    except Exception as e:
        say("")
        say("The root filesystem is a UBIFS volume and it could not be read:")
        say("    %s" % e)
        if "lzallright" in str(e) or "No module named" in str(e):
            say("")
            say("ubi_reader is installed but cannot decompress LZO, which this")
            say("filesystem uses. Install its backend:")
            say("    pip install --user lzallright")
        else:
            say("")
            say("Install ubi_reader:")
            say("    pip install --user ubi_reader lzallright")
        die("could not extract the root filesystem")

    # ubi_reader nests its output; find the tree that looks like a root.
    root_tree = None
    for cur, dirs, _files in os.walk(out):
        if all(os.path.isdir(os.path.join(cur, d))
               for d in ("bin", "lib", "sbin", "etc")):
            root_tree = cur
            break
    if not root_tree:
        die("extraction produced no recognisable root filesystem")

    say("==> installing to %s" % rfs)
    os.makedirs(dest, exist_ok=True)
    shutil.rmtree(rfs, ignore_errors=True)
    # SKIP SPECIAL FILES. The image carries /dev/initctl, a named pipe, and
    # shutil.copytree refuses it outright where cp -a would have copied it.
    # Pipes, sockets and device nodes mean nothing in a sysroot — the shim
    # provides the devices the guest actually uses — and cannot exist on
    # Windows at all, so they are dropped rather than fatal.
    def copy_plain(src, dst, *, follow_symlinks=True):
        mode = os.lstat(src).st_mode
        if (stat.S_ISFIFO(mode) or stat.S_ISSOCK(mode)
                or stat.S_ISBLK(mode) or stat.S_ISCHR(mode)):
            return dst
        shutil.copy2(src, dst, follow_symlinks=follow_symlinks)
        return dst

    shutil.copytree(root_tree, rfs, symlinks=True, copy_function=copy_plain,
                    ignore_dangling_symlinks=True)
    if kernel:
        shutil.copy2(kernel, os.path.join(dest, "kernel.bin"))

    # RESTORE THE EXECUTE BITS. ubi_reader only preserves permissions with
    # -k, which needs root, so everything arrives 0644 — including AppManager
    # ("Exec format error") and every shared library ("can't load library").
    say("==> restoring execute permissions")
    try:
        subprocess.run([sys.executable, os.path.join(HERE, "fix-perms.py"), rfs],
                       check=False)
    except Exception:
        say("    (could not restore permissions)")
    return dest


# ---- 3. the sysroot ---------------------------------------------------------

SYSFS = {
    "sys/devices/system/board/platform": "VALENCIA",
    "sys/devices/system/board/platform_family": "LPAD",
    "sys/devices/system/board/system_rev": "0x310",
    "sys/devices/system/board/lcd_size": "480x272",
    "sys/devices/system/board/lcd_type": "ILI6480G2",
    "sys/devices/system/board/lcd_mfg": "K&D-1",
    "sys/devices/system/board/lcd_mfg_get": "K&D-1",
    "sys/devices/platform/lf1000-dpc/xres": "480",
    "sys/devices/platform/lf1000-dpc/yres": "272",
    "sys/devices/platform/lf1000-gpio/board_id": "0",
    "sys/devices/platform/lf2000-power/status": "1",     # 1 = EXTERNAL
    "sys/class/graphics/fb0/rotate": "0",
    "sys/devices/platform/lf2000-touchscreen/max_tnt_down": "23",
    "sys/devices/platform/lf2000-touchscreen/min_tnt_up": "521",
    "sys/devices/platform/lf2000-touchscreen/max_delta_tnt": "5",
    "sys/devices/platform/lf2000-touchscreen/tnt_mode": "0",
    "sys/devices/platform/lf2000-touchscreen/averaging": "-1",
    "proc/asound/card0/id": "socaudiolfp100\n",
    "tmp/bulk_ready": "1\n",
    "tmp/splash": "0",
    "tmp/cart_brio_state": "7, CARTRIDGE_STATE_REINSERT",
    "flags/volume": "7",
    "flags/lasttime": "1357015435",
    "flags/pointercal": "0 65536 0 0 0 65536 65536\n",
}

MTD = """dev:    size   erasesize  name
mtd0: 0007e000 00001000 "NOR_Boot"
mtd1: 00001000 00001000 "MfgData0"
mtd2: 00001000 00001000 "MfgData1"
mtd3: 00400000 00100000 "Reserved"
mtd4: 01000000 00100000 "Kernel"
mtd5: 0a000000 00100000 "RFS"
mtd6: f4c00000 00100000 "Bulk"
"""

ASOUND = """pcm.plugdmix   { type null }
pcm.dmixbrio   { type null }
pcm.!default   { type null }
ctl.!default   { type hw card 0 }
"""

UIDATA = ('{"BadgeNumber": 0,"HasProfileBeenViewed": true,'
          '"_bookPickerWasLaunchedAtleastOnce": true,'
          '"_connectAlreadyPlayed": false}\n')


def build_sysroot(rootfs):
    sysroot = os.path.join(PROJ, "runtime", "sysroot")
    say("==> building the sysroot")
    os.makedirs(sysroot, exist_ok=True)

    for d in ("bin", "boot", "etc", "Firmware", "lib", "linuxrc", "mnt",
              "sbin", "erootfs.md5"):
        link_or_copy(os.path.join(rootfs, d), os.path.join(sysroot, d))

    lf = os.path.join(sysroot, "LF")
    if os.path.islink(lf):
        os.remove(lf)
    os.makedirs(os.path.join(lf, "Bulk"), exist_ok=True)
    os.makedirs(os.path.join(lf, "Cart"), exist_ok=True)
    link_or_copy(os.path.join(rootfs, "LF", "Base"), os.path.join(lf, "Base"))

    for d in ("dev/input", "sys", "proc", "tmp", "flags"):
        os.makedirs(os.path.join(sysroot, d), exist_ok=True)

    # /usr overlay, so absolute-path library lookups resolve.
    usr = os.path.join(sysroot, "usr")
    if os.path.islink(usr):
        os.remove(usr)
    os.makedirs(usr, exist_ok=True)
    src_usr = os.path.join(rootfs, "usr")
    if os.path.isdir(src_usr):
        for b in sorted(os.listdir(src_usr)):
            if b != "lib":
                link_or_copy(os.path.join(src_usr, b), os.path.join(usr, b))
    usrlib = os.path.join(usr, "lib")
    os.makedirs(usrlib, exist_ok=True)
    for src in (os.path.join(rootfs, "usr", "lib"), os.path.join(rootfs, "lib")):
        if not os.path.isdir(src):
            continue
        for f in sorted(os.listdir(src)):
            link_or_copy(os.path.join(src, f), os.path.join(usrlib, f))

    shim = os.path.join(PROJ, "runtime", "shimlibs", "libdl.so.0")
    if os.path.exists(shim):
        link_or_copy(shim, os.path.join(sysroot, "lib", "libdl.so.0"))
        link_or_copy(shim, os.path.join(usrlib, "libdl.so.0"))
    else:
        say("    WARNING: the shim's libdl is not built — native titles will")
        say("    crash on launch until it is.")

    # /etc overlay: everything but asound.conf, which we replace with a null
    # sink so ALSA does not go looking for hardware.
    etc = os.path.join(sysroot, "etc")
    if os.path.islink(etc):
        os.remove(etc)
    os.makedirs(etc, exist_ok=True)
    src_etc = os.path.join(rootfs, "etc")
    if os.path.isdir(src_etc):
        for b in sorted(os.listdir(src_etc)):
            if b != "asound.conf":
                link_or_copy(os.path.join(src_etc, b), os.path.join(etc, b))
    write_text(os.path.join(etc, "asound.conf"), ASOUND)

    # /var/sounds — rcS makes these per-platform on the device.
    var = os.path.join(sysroot, "var")
    if os.path.islink(var):
        os.remove(var)
    os.makedirs(var, exist_ok=True)
    src_var = os.path.join(rootfs, "var")
    if os.path.isdir(src_var):
        for b in sorted(os.listdir(src_var)):
            if b != "sounds":
                link_or_copy(os.path.join(src_var, b), os.path.join(var, b))
    sounds = os.path.join(var, "sounds")
    os.makedirs(sounds, exist_ok=True)
    src_sounds = os.path.join(rootfs, "var", "sounds")
    if os.path.isdir(src_sounds):
        for f in sorted(os.listdir(src_sounds)):
            link_or_copy(os.path.join(src_sounds, f), os.path.join(sounds, f))
    vid = os.path.join(rootfs, "LF", "Base", "LpadAssets", "Video")
    for v in ("StartupVideo.ogg", "ShutdownVideo.ogg", "TransitionVideo.ogg",
              "powerdown.wav"):
        link_or_copy(os.path.join(vid, v), os.path.join(sounds, v))

    for rel, text in SYSFS.items():
        write_text(os.path.join(sysroot, rel), text)
    write_text(os.path.join(sysroot, "proc", "mtd"), MTD)

    for n, size in ((0, 0x7e000), (1, 0x1000), (2, 0x1000)):
        touch(os.path.join(sysroot, "dev", "mtd%d" % n), size)
    for i in range(25):
        touch(os.path.join(sysroot, "dev", "input", "event%d" % i))
    for i in range(3):
        touch(os.path.join(sysroot, "dev", "fb%d" % i))
    for f in ("tmp/initial", "flags/developer", "flags/poweron"):
        touch(os.path.join(sysroot, f))

    bulk = os.path.join(sysroot, "LF", "Bulk")
    uipkg = "PAD2-0x1F1E0002-100000"
    for d in ("Data/Uploads/0", "Data/Uploads/1", "Data/Uploads/2",
              "Data/Uploads/3", "Data/Downloads", "Data/Settings",
              "ProgramFiles/KeyboardWidget", "ProgramFiles/CameraWidget",
              "ProgramFiles/PhotoEditor", "ProgramFiles/SneakPeekWidget",
              "Downloads/PAD2-0x00210008-200000",
              "Downloads/PADS-0x1F1E0002-300000"):
        os.makedirs(os.path.join(bulk, d), exist_ok=True)
    for prof in ("0", "1", "2", "3", "All"):
        p = os.path.join(bulk, "Data", "Local", prof, uipkg)
        os.makedirs(p, exist_ok=True)
        j = os.path.join(p, "UIData.json")
        if not os.path.exists(j):
            write_text(j, UIDATA)

    return sysroot


def link_runtime_libs(rootfs):
    """runtime/libs — every shared object, flat, for LD_LIBRARY_PATH."""
    libdir = os.path.join(PROJ, "runtime", "libs")
    os.makedirs(libdir, exist_ok=True)
    for f in os.listdir(libdir):
        p = os.path.join(libdir, f)
        if os.path.islink(p):
            os.remove(p)
    n = 0
    for d in ("lib", "usr/lib", "LF/Base/lib", "LF/Base/Brio/lib",
              "LF/Base/Flash/lib"):
        src = os.path.join(rootfs, d)
        if not os.path.isdir(src):
            continue
        for so in sorted(os.listdir(src)):
            if ".so" in so and link_or_copy(os.path.join(src, so),
                                            os.path.join(libdir, so)):
                n += 1
    say("    %d libraries linked" % n)


# ---- 4. content packages ----------------------------------------------------

def self_wraps(path):
    """True when the archive already contains its own package directory."""
    names = members_of(path)
    if not names:
        return False
    tops = {n.split("/", 1)[0] for n in names if n.strip()}
    return len(tops) == 1 and "meta.inf" not in names


def extract_as(path, pkgdir):
    """Extract so the package lands AT pkgdir.

    Some archives already contain their own top-level directory and some do
    not; extracting the first kind into pkgdir would nest it one deep.
    """
    if self_wraps(path):
        extract_pkg(path, os.path.dirname(pkgdir))
    else:
        extract_pkg(path, pkgdir)


def install_content(pkgs, sysroot):
    """The destination rules, which are per package Type and not obvious."""
    say("==> installing content packages")
    bulk = os.path.join(sysroot, "LF", "Bulk")
    n = {}

    def bump(k):
        n[k] = n.get(k, 0) + 1

    # TWO PASSES. A DeviceAsset installs INTO the package it belongs to, so
    # every other kind has to be on disk before they are looked at.
    for pass_no in (1, 2):
        for f in pkgs:
            if f.lower().endswith(".lf3"):
                continue
            meta = meta_of(f)
            if not meta:
                if pass_no == 1:
                    bump("unreadable")
                continue
            typ = field(meta, "Type")
            pid = field(meta, "PackageID") or ("unknown-" + os.path.basename(f)[:8])

            if pass_no == 1 and typ == "DeviceAsset":
                continue
            if pass_no == 2 and typ != "DeviceAsset":
                continue

            if typ == "Application":
                extract_as(f, os.path.join(bulk, "ProgramFiles", pid))
            elif typ in ("Download", "MicroDownload"):
                extract_pkg(f, os.path.join(bulk, "Downloads", pid))
            elif typ == "LanguagePack":
                extract_pkg(f, bulk)
            elif typ in ("Music", "MusicInfo"):
                extract_pkg(f, os.path.join(bulk, "Music", pid))
            elif typ == "DeviceAsset":
                # PID looks like XXXX-0xPRODUCT-NNNDA; the parent is the
                # package whose manifest mentions PRODUCT-000.
                bits = pid.split("-")
                parent = None
                if len(bits) >= 3:
                    key = bits[1] + "-" + bits[2].replace("DA", "00")
                    for root, _d, files in os.walk(bulk):
                        if "meta.inf" not in files:
                            continue
                        try:
                            with open(os.path.join(root, "meta.inf"),
                                      "r", errors="replace") as fh:
                                if key in fh.read():
                                    parent = root
                                    break
                        except OSError:
                            pass
                if not parent:
                    say("    DeviceAsset  %s -> no parent (skipped)" % pid)
                    bump("skipped")
                    continue
                extract_as(f, parent)
                extract_pkg(f, parent)
            elif typ in ("DiskImage", "System"):
                bump("skipped")
                continue
            else:
                extract_pkg(f, os.path.join(bulk, "Downloads", pid))
            bump(typ or "other")

    for k in sorted(n):
        say("    %-16s %d" % (k, n[k]))
    grant_profile_access(sysroot, bulk)


def grant_profile_access(sysroot, bulk):
    """Make the home screen's own titles visible to every profile.

    The sort file lists what belongs on the home screen. A package with no
    ProfileAccess line is not offered to any profile, so it installs and then
    does not appear — which reads as a failed install rather than a missing
    manifest field.
    """
    sort = os.path.join(sysroot, "LF", "Base", "LpadAssets_en", "Data",
                        "ProgramFileAppOrder.json")
    if not os.path.isfile(sort):
        say("    (no sort file; skipping profile access)")
        return
    try:
        with open(sort, "r", errors="replace") as f:
            wanted = set(re.findall(r'"([A-Z0-9]+-0x[0-9A-Fa-f]+-[0-9A-Za-z]+)"',
                                    f.read()))
    except OSError:
        return
    done = 0
    progdir = os.path.join(bulk, "ProgramFiles")
    if not os.path.isdir(progdir):
        return
    for pkg in sorted(os.listdir(progdir)):
        m = os.path.join(progdir, pkg, "meta.inf")
        if not os.path.isfile(m):
            continue
        try:
            with open(m, "r", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        pid = field(text, "PackageID")
        if pid not in wanted or re.search(r"^ProfileAccess=", text, re.M):
            continue
        with open(m, "a") as f:
            if not re.search(r"^DeviceAccess=", text, re.M):
                f.write("DeviceAccess=0x00000000\n")
            f.write("ProfileAccess=-1,0,1,2,3\n")
        done += 1
    say("    profile access    %d" % done)


# ---- main -------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("src", help="LFC_Downloads dir, or a .lfp/.lf2/.zip")
    ap.add_argument("--no-content", action="store_true",
                    help="system files only; skip the content packages")
    args = ap.parse_args(argv[1:])

    src = args.src
    if not os.path.exists(src):
        die("no such path: %s" % src)

    stage = tempfile.mkdtemp(prefix="tadpole-fw-")
    try:
        pkgs = gather(src, stage)
        fw, version = find_firmware(pkgs)
        say("==> Firmware-Base version %s" % version)
        rootfs = os.path.join(extract_rootfs(fw, version, stage), "ubi_rfs")

        # THE SYSROOT BEFORE THE CONTENT. Content installs INTO the sysroot,
        # and the sort file that decides what appears on the home screen lives
        # there. The other order leaves content where nothing looks for it.
        sysroot = build_sysroot(rootfs)
        link_runtime_libs(rootfs)
        if not args.no_content:
            install_content(pkgs, sysroot)

        lf3 = [p for p in pkgs if p.lower().endswith(".lf3")]
        if lf3:
            say("==> %d digital purchase(s) (.lf3)" % len(lf3))
            keys = os.path.join(PROJ, "keys", "lf3.keys")
            if not os.path.exists(keys):
                say("  can't open .lf3 files, decryption key missing.")
                say("  Put key in keys/lf3.keys")
                say("  (everything else installed normally; only these were skipped)")

        say("")
        say("Done. Firmware %s installed under rootfs/stock-%s/" % (version, version))
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
