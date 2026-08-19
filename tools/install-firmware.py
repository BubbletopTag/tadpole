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

# A kernel gives up after ~40 links; so do we, and for the same reason. The
# rootfs contains at least one two-hop chain (pidof -> pidof.sysvinit ->
# /sbin/killall5), so following exactly one link is not enough.
GUEST_LINK_HOPS = 40


def _guest_join(rootfs, base, target):
    """Resolve one link target as a kernel rooted at `rootfs` would.

    Two rules, and both of them are the whole point:

      * an absolute target is absolute IN THE GUEST. "/sbin/killall5" is the
        rootfs's /sbin/killall5, not the host's — on Windows the host has no
        /sbin at all, and on Linux it has one belonging to somebody else.
      * ".." at the root is the root. Real kernels clamp it there, which is why
        "linuxrc -> ../bin/busybox" is a perfectly good link on the device and
        a path outside the directory tree here.
    """
    if target.startswith("/"):
        joined = os.path.join(rootfs, target.lstrip("/"))
    else:
        joined = os.path.join(base, target)
    parts = []
    for seg in os.path.relpath(joined, rootfs).replace("\\", "/").split("/"):
        if seg in ("", "."):
            continue
        if seg == "..":
            if parts:                    # at the root, ".." is the root
                parts.pop()
            continue
        parts.append(seg)
    return os.path.join(rootfs, *parts) if parts else rootfs


def guest_resolve(rootfs, path):
    """Where the guest would end up following the links at `path`.

    Returns a host path INSIDE `rootfs`, or None if the guest would not find
    anything either. None is a normal answer, not a failure: the firmware
    genuinely ships links to files it does not contain — the LucyAssets videos
    are missing on the device too — and the right thing to do with those is
    what the device does, which is nothing.

    Because every step is re-anchored on `rootfs`, the answer can never be a
    file outside it, whatever the link says.
    """
    rootfs = os.path.abspath(rootfs)
    cur = os.path.abspath(path)
    for _ in range(GUEST_LINK_HOPS):
        if not os.path.islink(cur):
            return cur if os.path.exists(cur) else None
        cur = _guest_join(rootfs, os.path.dirname(cur), os.readlink(cur))
    return None                          # a loop


def copy_tree_as_guest(rootfs, src, dst, _chain=()):
    """Copy `src` to `dst`, following the rootfs's links the way the guest does.

    shutil.copytree cannot do this, and both of its settings are wrong here:

      symlinks=True   makes symlinks, which Windows refuses without Developer
                      Mode or elevation — the reason this function copies.
      symlinks=False  dereferences against the HOST. A dangling link then
                      raises (this is the crash: "shutil.Error ...
                      bin/pidof.sysvinit"), and a link that is NOT dangling on
                      the host is worse than one that is, because
                      "/etc" would copy the host's /etc into the guest image.

    So the walk is done here, one entry at a time, with every link resolved by
    guest_resolve and therefore always re-anchored inside the rootfs. Returns
    the number of entries skipped because the guest could not reach them
    either.

    _chain carries the directories currently being copied, so a directory link
    pointing back up its own path is skipped rather than followed forever.
    """
    os.makedirs(dst, exist_ok=True)
    skipped = 0
    here = _chain + (os.path.abspath(src),)
    for name in sorted(os.listdir(src)):
        s = os.path.join(src, name)
        d = os.path.join(dst, name)
        if os.path.islink(s):
            real = guest_resolve(rootfs, s)
            if real is None:             # dangling for the guest too
                skipped += 1
                continue
            if os.path.isdir(real):
                if os.path.abspath(real) in here:
                    skipped += 1         # a link back up our own path
                    continue
                skipped += copy_tree_as_guest(rootfs, real, d, here)
            else:
                shutil.copy2(real, d)
        elif os.path.isdir(s):
            skipped += copy_tree_as_guest(rootfs, s, d, here)
        else:
            shutil.copy2(s, d)
    return skipped


def link_or_copy(src, dst, rootfs=None):
    """A symlink on Linux, a real copy on Windows.

    Windows can make symlinks only with Developer Mode or elevation, and a
    failure here would leave a sysroot that looks built and is not.

    `rootfs` is the root the guest's own absolute links are relative to, and
    is required whenever `src` is a directory inside a firmware image —
    without it there is no way to know that "/sbin/killall5" means the
    rootfs's. It is not needed for the files copied out of runtime/, which
    are ours and contain no links.
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
            # NOT shutil.copytree. It dereferences against the host, which
            # aborts the whole install on the first link the host cannot
            # follow — "/sbin/killall5" is the rootfs's, and the host has no
            # /sbin. See copy_tree_as_guest.
            copy_tree_as_guest(rootfs or src, src, dst)
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

# THE FIRMWARE IS NOT ALWAYS CALLED Firmware-Base, and assuming it is, is what
# made "Online System Update" fail for the Didj with a list of thirteen
# perfectly good packages and the advice to go and find a fourteenth.
#
# Every LeapPad and Leapster ships its root filesystem in a package whose
# manifest reads Type="DiskImage", Name="Firmware-Base". The Didj — six years
# older than any of them — ships Type="System", Name="Didj Device Firmware",
# and the meta is the only thing that differs: it is still one package, still
# holding the whole root filesystem, still the thing to extract first.
#
# So the test is on the CONTENTS rather than the name. A firmware package is
# the one carrying a root filesystem image, and which kind it carries is also
# the answer to how to unpack it:
#
#     *.ubi     LeapPad2, LeapPad Ultra, Leapster GS   ubi_reader
#     *.jffs2   Didj                                   pkgtool's own reader
#     a tar     LeapPad3                               (see setup-sysroot.sh)
#
# The manifest test is kept as well, and tried first, because it is the
# cheapest and it is right for every device that has ever worked here.
IMAGE_EXTS = ((".ubi", "ubi"), (".jffs2", "jffs2"))


def firmware_image(pkg):
    """-> (member, kind, is_root) for a filesystem image inside `pkg`, or None.

    `is_root` SEPARATES THE ROOT FROM THE MERELY FILESYSTEM-SHAPED, and it has
    to, because the Didj ships both. Its bootloader package carries a
    128 KB bootflags.jffs2 — a JFFS2 volume by every structural test, and one
    that sorts BEFORE the firmware — so a scan that stopped at the first image
    it found extracted the boot flags and then reported the firmware as
    unrecognisable. Every device names the real one erootfs: erootfs.jffs2
    here, C4G-E1M-W4K-erootfs.ubi on the LeapPad2.
    """
    best = None
    for name in members_of(pkg):
        low = name.lower()
        for ext, kind in IMAGE_EXTS:
            if not low.endswith(ext):
                continue
            if "erootfs" in low:
                return name, kind, True
            if best is None:
                best = (name, kind, False)
    return best


def find_firmware(pkgs, want_device=""):
    """-> (package, version, image kind) for the firmware in `pkgs`.

    `want_device` is a Device= from a manifest ("Didj", "LeapPad3"), and when
    it is given a firmware for anything else is passed over.

    THIS IS NOT BELT-AND-BRACES. Online System Update used to stage every
    device's download into one sources/online-update/cache, so a Leapster GS
    fetched last week was still sitting there when a Didj was fetched today —
    and its Firmware-Base sorts first. The unfiltered scan installed a Leapster
    GS, correctly and completely, for a user who had asked for a Didj. That
    directory is now per-device (see online-update.sh), which fixes the case
    Tadpole creates; this is what covers the one it does not, a hand-supplied
    LFC_Downloads folder holding whatever the user's LFConnect ever downloaded.
    """
    listing, wrong, fallback = [], [], None
    for p in pkgs:
        meta = meta_of(p)
        if not meta:
            continue
        t, n, d = field(meta, "Type"), field(meta, "Name"), field(meta, "Device")
        img = firmware_image(p)
        named = t == "DiskImage" and n == "Firmware-Base"
        if not (img or named):
            listing.append((t, n))
            continue
        if want_device and d != want_device:
            # Only the ones that would actually have been taken. The Didj's
            # bootloader package holds a bootflags.jffs2 and so reaches here,
            # and listing it as a firmware for another device would be one more
            # wrong thing in a message whose whole job is to be right.
            if img is not None and (img[2] or named):
                wrong.append((d or "?", n))
            continue
        if img is None:
            die("%s says it is the firmware but carries no root filesystem"
                % os.path.basename(p))
        if img[2] or named:
            return p, field(meta, "Version"), img[1]
        # An image that is not named erootfs and whose manifest does not claim
        # to be the firmware either. Remembered, not taken: a package that IS
        # both wins, and this is only right if nothing better turns up.
        if fallback is None:
            fallback = (p, field(meta, "Version"), img[1])
    if fallback:
        return fallback

    if wrong:
        # NOT "no firmware here", which would be false and would send someone
        # to look for a file they already have. There is one; it is for
        # something else.
        say("no firmware for %s here, but there IS one for:" % want_device)
        for d, n in wrong:
            say("    %-22s %s" % (d, n))
        die("this source holds another device's firmware — point it at %s's, "
            "or drop --device to install what is here" % want_device)
    say("no firmware package here.")
    say("Packages present:")
    for t, n in listing:
        say("    %-14s %s" % (t, n))
    die("supply the full LFC_Downloads directory, which contains it")


def extract_ubi(image, out):
    """The UBI path, and its diagnosis when ubi_reader is not all there."""
    try:
        # KEEP THE TOOL'S OWN ERROR. ubi_reader installed without its LZO
        # backend fails with "No module named 'lzallright'", a one-line fix
        # that is invisible if the message is swallowed.
        pkgtool.cmd_ubi(image, out)
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


# WHAT THE EXTRACTED TREE IS CALLED, which setup-sysroot.sh and device.sh both
# glob for. The name states which filesystem it came out of, so that nobody has
# to open it to find out: ubi_rfs for a UBI volume, emmc_rfs for the LeapPad3's
# tar, jffs2_rfs for the Didj. Calling the Didj's tree ubi_rfs would be a lie
# the next person has to disprove — the same reasoning setup-sysroot.sh gives
# for emmc_rfs, and the reason that comment is worth keeping in step with this.
RFS_DIRNAME = {"ubi": "ubi_rfs", "jffs2": "jffs2_rfs"}


def extract_rootfs(fw, version, stage, kind="ubi"):
    dest = os.path.join(PROJ, "rootfs", "stock-%s" % version)
    rfs = os.path.join(dest, RFS_DIRNAME.get(kind, "ubi_rfs"))
    if os.path.isdir(rfs) and os.listdir(rfs):
        say("==> %s already populated — leaving it alone" % rfs)
        say("    (delete it first if you want to re-extract)")
        return dest, rfs

    fwdir = os.path.join(stage, "fw")
    extract_pkg(fw, fwdir)

    want = ".jffs2" if kind == "jffs2" else ".ubi"
    image = kernel = None
    for root, _dirs, files in os.walk(fwdir):
        for fn in files:
            low = fn.lower()
            if image is None and low.endswith(want) and "erootfs" in low:
                image = os.path.join(root, fn)
            elif image is None and low.endswith(want):
                image = os.path.join(root, fn)
            if low.endswith("kernel.bin"):
                kernel = os.path.join(root, fn)
    if not image:
        die("no %s root filesystem inside %s" % (want, os.path.basename(fw)))
    say("  root filesystem: %s (%d bytes)"
        % (os.path.basename(image), os.path.getsize(image)))
    if kernel:
        say("  kernel: %s" % os.path.basename(kernel))

    say("==> extracting the root filesystem")
    out = os.path.join(stage, "rfs")
    os.makedirs(out, exist_ok=True)

    if kind == "jffs2":
        # NO SECOND-GUESSING HERE. pkgtool reads JFFS2 itself, with nothing to
        # install and nothing that can be missing, so the elaborate ubi_reader
        # diagnosis below has no counterpart: if this fails the image is bad.
        say("    (a 7 MB volume)")
        pkgtool.cmd_jffs2(image, out)
    else:
        say("    (a 53 MB volume — this takes a minute or two)")
        extract_ubi(image, out)

    # ubi_reader nests its output one or two levels deep and pkgtool's JFFS2
    # reader does not, so neither is assumed: find the tree that looks like a
    # root.
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

    # WHOSE FIRMWARE THIS IS, WRITTEN WHERE EVERYTHING LOOKS FOR IT.
    #
    # detect_device() below, tad_detect_device() in runtime/device.sh, the
    # device picker and the park-and-switch all ask one question of an
    # extracted tree: what does Firmware/meta.inf say Device= is. Every LeapPad
    # and Leapster image carries that file. The Didj's does not — its root has
    # no Firmware directory at all — so a faithful extraction identifies as no
    # device and the tree is invisible to all four.
    #
    # The manifest is not fabricated to fix that: it is the firmware package's
    # OWN meta.inf, the one that already says Device="Didj" Version="1.35.2.4222",
    # copied to the place the other devices keep theirs. Never overwritten,
    # so an image that ships its own keeps it.
    seeded = os.path.join(rfs, "Firmware", "meta.inf")
    if not os.path.exists(seeded):
        meta = meta_of(fw)
        if meta:
            write_text(seeded, meta)
            say("  recorded Device=\"%s\" in Firmware/meta.inf"
                % field(meta, "Device"))

    # RESTORE THE EXECUTE BITS. ubi_reader only preserves permissions with
    # -k, which needs root, so everything arrives 0644 — including AppManager
    # ("Exec format error") and every shared library ("can't load library").
    #
    # NOT FOR JFFS2, and this is a real difference rather than an optimisation.
    # pkgtool's reader carries each inode's own mode across, so the tree
    # already has the permissions the device has — and fix-perms.py guesses
    # from file contents, so running it would ADD +x to every .so, which the
    # Didj itself ships 0644. Guessing over a known answer is how a tree stops
    # matching the hardware it came from.
    if kind == "jffs2":
        say("    permissions came out of the image itself")
    else:
        say("==> restoring execute permissions")
        try:
            subprocess.run([sys.executable, os.path.join(HERE, "fix-perms.py"), rfs],
                           check=False)
        except Exception:
            say("    (could not restore permissions)")
    return dest, rfs


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
        link_or_copy(os.path.join(rootfs, d), os.path.join(sysroot, d), rootfs)

    lf = os.path.join(sysroot, "LF")
    if os.path.islink(lf):
        os.remove(lf)
    os.makedirs(os.path.join(lf, "Bulk"), exist_ok=True)
    os.makedirs(os.path.join(lf, "Cart"), exist_ok=True)
    link_or_copy(os.path.join(rootfs, "LF", "Base"), os.path.join(lf, "Base"),
                 rootfs)

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
                link_or_copy(os.path.join(src_usr, b), os.path.join(usr, b), rootfs)
    usrlib = os.path.join(usr, "lib")
    os.makedirs(usrlib, exist_ok=True)
    for src in (os.path.join(rootfs, "usr", "lib"), os.path.join(rootfs, "lib")):
        if not os.path.isdir(src):
            continue
        for f in sorted(os.listdir(src)):
            link_or_copy(os.path.join(src, f), os.path.join(usrlib, f), rootfs)

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
                link_or_copy(os.path.join(src_etc, b), os.path.join(etc, b), rootfs)
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
                link_or_copy(os.path.join(src_var, b), os.path.join(var, b), rootfs)
    sounds = os.path.join(var, "sounds")
    os.makedirs(sounds, exist_ok=True)
    src_sounds = os.path.join(rootfs, "var", "sounds")
    if os.path.isdir(src_sounds):
        for f in sorted(os.listdir(src_sounds)):
            link_or_copy(os.path.join(src_sounds, f), os.path.join(sounds, f), rootfs)
    vid = os.path.join(rootfs, "LF", "Base", "LpadAssets", "Video")
    for v in ("StartupVideo.ogg", "ShutdownVideo.ogg", "TransitionVideo.ogg",
              "powerdown.wav"):
        link_or_copy(os.path.join(vid, v), os.path.join(sounds, v), rootfs)

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

    # WHOSE TREE THIS IS. runtime/setup-sysroot.sh writes the same line, and
    # everything that asks "which device is live" reads it: the shell's
    # tad_active_device(), the front end's device picker, and the switch that
    # parks one tree and moves another in. Re-detecting it from the sysroot's
    # own Firmware/meta.inf does not work — package-manager rewrites every
    # meta.inf in the tree and blanks Device= — so it is recorded here, once,
    # while the answer is still known.
    #
    # This is the Windows path, where the shell script cannot run. It has to
    # write the marker itself or a Windows install identifies as no device at
    # all and the picker has nothing to tick.
    dev = detect_device(rootfs)
    if dev:
        write_text(os.path.join(sysroot, ".tadpole-device"), dev + "\n")

    return sysroot


def detect_device(rootfs):
    """-> the DEV_ID this firmware belongs to, by the same test the shell
    makes: Firmware/meta.inf's Device= against each profile's
    DEV_META_DEVICE. Empty when nothing matches, which is the honest answer
    for a firmware there is no profile for."""
    meta = os.path.join(rootfs, "Firmware", "meta.inf")
    want = ""
    try:
        with open(meta, "r", errors="replace") as f:
            for line in f:
                if line.startswith('Device="'):
                    want = line.split('"')[1]
                    break
    except OSError:
        return ""
    if not want:
        return ""
    devdir = os.path.join(PROJ, "runtime", "devices")
    try:
        names = sorted(os.listdir(devdir))
    except OSError:
        return ""
    for name in names:
        if not name.endswith(".conf"):
            continue
        dev_id = meta_dev = ""
        with open(os.path.join(devdir, name), "r", errors="replace") as f:
            for line in f:
                if line.startswith("DEV_ID="):
                    dev_id = line.split("=", 1)[1].strip().strip('"')
                elif line.startswith("DEV_META_DEVICE="):
                    meta_dev = line.split("=", 1)[1].strip().strip('"')
        if meta_dev and meta_dev == want:
            return dev_id
    return ""


def profile_meta_device(dev_id):
    """-> the DEV_META_DEVICE of runtime/devices/<dev_id>.conf, or "".

    The inverse of detect_device: that turns a firmware's Device= into a
    DEV_ID, this turns a DEV_ID back into the Device= to look for.
    """
    conf = os.path.join(PROJ, "runtime", "devices", dev_id + ".conf")
    try:
        with open(conf, "r", errors="replace") as f:
            for line in f:
                if line.startswith("DEV_META_DEVICE="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return ""


# WHERE EACH DEVICE KEEPS ITS SHARED OBJECTS, relative to the root it is given.
#
# The LeapPad line puts everything under one tree, so one root answers for all
# of it. The Didj does not: its /Didj/Base comes from PACKAGES and is assembled
# into the sysroot, while /lib and /usr/lib come out of the firmware image — so
# it is two roots, and link_runtime_libs takes a list for that reason.
LIB_DIRS = ("lib", "usr/lib", "LF/Base/lib", "LF/Base/Brio/lib",
            "LF/Base/Flash/lib",
            "Didj/Base/lib", "Didj/Base/Brio/lib")


def link_runtime_libs(*roots):
    """runtime/libs — every shared object, flat, for LD_LIBRARY_PATH.

    EARLIER ROOTS WIN. For the Didj the sysroot is passed first, so a library
    the Brio package ships takes precedence over a same-named one in the
    firmware image — which is the order the device's own /etc/profile puts
    them in.
    """
    libdir = os.path.join(PROJ, "runtime", "libs")
    os.makedirs(libdir, exist_ok=True)
    for f in os.listdir(libdir):
        p = os.path.join(libdir, f)
        if os.path.islink(p):
            os.remove(p)
    n, seen = 0, set()
    for root in roots:
        for d in LIB_DIRS:
            src = os.path.join(root, d)
            if not os.path.isdir(src):
                continue
            for so in sorted(os.listdir(src)):
                if ".so" not in so or so in seen:
                    continue
                if link_or_copy(os.path.join(src, so),
                                os.path.join(libdir, so)):
                    seen.add(so)
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


# ---- the Didj ---------------------------------------------------------------
#
# A DIFFERENT SHAPE, not a different amount of work. The Didj is a 2008 LF1000
# handheld and the oldest device Tadpole knows; everything above assumes the
# LeapPad shape, and three of those assumptions are wrong here:
#
#   * the root filesystem is JFFS2 rather than a UBI volume (extract_rootfs),
#   * the tree root is /Didj rather than /LF, with cartridges at /Cart,
#   * the packages are Type="System" and install by NAME into /Didj/Base,
#     where a LeapPad's content is typed and installs under LF/Bulk.
#
# So the Didj gets its own sysroot builder and its own destination map rather
# than a pile of `if didj:` inside the LeapPad ones. See docs/DIDJ.md.

# WHERE EACH PACKAGE GOES. Transcribed from the same community inventory as
# tools/packagelists/Didj.xml, which keeps this map in comments beside each
# entry because it cannot be derived from the CDN. Keyed on Type= because that
# is what the manifests actually carry — all eleven system packages say
# Type="System" — with the two that are not laid into the tree at all
# recognised by their contents just above.
DIDJ_DESTS = {
    "System": os.path.join("Didj", "Base"),
    "Avatar": os.path.join("Didj", "Data", "Avatars"),
    "Application": os.path.join("Didj", "ProgramFiles"),
}
DIDJ_OTHER = os.path.join("Didj", "Data", "MDL")

# See the note in build_sysroot_didj for where each value comes from.
DIDJ_SYSFS = {
    "sys/devices/platform/lf1000-nand/cartridge": "none\n",
    "sys/devices/platform/lf1000-usbgadget/vbus": "0\n",
    "sys/devices/platform/lf1000-power/status": "1\n",
}


def install_didj_content(pkgs, sysroot, fw):
    """Lay the Didj's packages into the sysroot's /Didj tree.

    EVERY PACKAGE CARRIES ITS OWN TOP DIRECTORY — Brio/, bin/, lib/, BLT/,
    Avatar1/, POW/ — so each one extracts straight into its destination and
    lands as /Didj/Base/Brio, /Didj/Base/bin and so on. That is why there is no
    self_wraps() dance here: on this device it is not a question, it is how all
    thirteen are built, and /Didj/Base/bin/AppManager (the DEV_SHELL in
    runtime/devices/didj.conf) is the file it produces.

    ONLY Device="Didj" PACKAGES, and this map needs that more than the LeapPad
    one does. Its last rule is a catch-all — anything unrecognised goes to
    /Didj/Data/MDL — so pointed at a directory holding two devices' downloads
    it would not skip the stranger, it would file it. A Leapster GS package set
    left in a shared cache is how that stops being hypothetical.
    """
    say("==> installing Didj packages")
    n = {}

    def bump(k):
        n[k] = n.get(k, 0) + 1

    for f in pkgs:
        if os.path.abspath(f) == os.path.abspath(fw):
            continue                       # the root filesystem, already out
        meta = meta_of(f)
        if not meta:
            bump("unreadable")
            continue
        if field(meta, "Device") != "Didj":
            bump("another device (skipped)")
            continue
        # THE BOOTLOADER IS NOT PART OF THE FILESYSTEM. lightning-boot writes
        # to the NOR flash the SoC boots from, and its package holds a .bin and
        # a bootflags image — nothing that belongs anywhere in a tree. The Didj
        # package list carries it because a real update flashes it; Tadpole
        # never boots this device from its own bootloader, so it is skipped
        # deliberately rather than dropped into /Didj/Data/MDL as an unknown.
        if any(os.path.basename(m) == "lightning-boot.bin" for m in members_of(f)):
            bump("bootloader (not installed)")
            continue
        typ = field(meta, "Type")
        dest = os.path.join(sysroot, DIDJ_DESTS.get(typ, DIDJ_OTHER))
        extract_pkg(f, dest)
        bump(typ or "other")

    for k in sorted(n):
        say("    %-24s %d" % (k, n[k]))


def build_sysroot_didj(rootfs):
    """The Didj's runtime tree: the firmware, plus a /Didj for its packages."""
    sysroot = os.path.join(PROJ, "runtime", "sysroot")
    say("==> building the sysroot")
    os.makedirs(sysroot, exist_ok=True)

    for d in ("bin", "boot", "linuxrc", "mnt", "mnt2", "opt", "sbin", "test",
              "Firmware"):
        link_or_copy(os.path.join(rootfs, d), os.path.join(sysroot, d), rootfs)

    # /lib AND /usr/lib BOTH GET BOTH, and that is not tidiness.
    #
    # qemu's -L only redirects a path that EXISTS under the prefix; anything
    # missing resolves against the developer's machine instead. The Didj keeps
    # libz, libpng and liblzo2 in /usr/lib while something in the Brio stack
    # asks for /lib/libz.so by absolute path, so a faithful copy of the tree
    # produces
    #
    #     AppManager: '/lib/libz.so' is not an ELF executable for ARM
    #
    # which names a host x86-64 object and reads as broken firmware. It is the
    # trap docs/DIDJ.md records for LD_LIBRARY_PATH, in its other form: the fix
    # is to make both directories complete so neither lookup can escape.
    for d, others in (("lib", ("lib", "usr/lib")),
                      ("usr/lib", ("usr/lib", "lib"))):
        out = os.path.join(sysroot, d)
        if os.path.islink(out):
            os.remove(out)
        # BUILT FRESH, because the entries below are skipped when something is
        # already there and a re-install after a version change would
        # otherwise keep every link into the rootfs that has just been
        # replaced. Nothing here is the guest's: it is all links (or, on
        # Windows, copies) of the firmware's own libraries.
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out, exist_ok=True)
        for src in others:
            src = os.path.join(rootfs, src)
            if not os.path.isdir(src):
                continue
            for f in sorted(os.listdir(src)):
                p = os.path.join(out, f)
                if os.path.islink(p) or os.path.exists(p):
                    continue               # the directory's own wins
                link_or_copy(os.path.join(src, f), p, rootfs)

    usr = os.path.join(sysroot, "usr")
    src_usr = os.path.join(rootfs, "usr")
    if os.path.isdir(src_usr):
        for b in sorted(os.listdir(src_usr)):
            if b != "lib":
                link_or_copy(os.path.join(src_usr, b), os.path.join(usr, b), rootfs)

    # /etc and /var as real directories: rcS and launch_main write into both,
    # and a symlink to the rootfs would put a running system's scribbles into
    # the extracted firmware.
    for d in ("etc", "var"):
        out = os.path.join(sysroot, d)
        if os.path.islink(out):
            os.remove(out)
        os.makedirs(out, exist_ok=True)
        src = os.path.join(rootfs, d)
        if os.path.isdir(src):
            for b in sorted(os.listdir(src)):
                link_or_copy(os.path.join(src, b), os.path.join(out, b), rootfs)

    # THE MOUNT POINTS. On the device /Didj and /Cart are separate MTD
    # partitions mounted onto an otherwise empty pair of directories in the
    # root image — which is why the extracted rootfs has both and neither has
    # anything in it. /Didj is what the packages fill; /Cart stays empty until
    # there is a cartridge, exactly as on hardware with the slot empty.
    for d in ("Didj/Base", "Didj/Data", "Didj/ProgramFiles", "Cart",
              "dev/input", "sys", "proc", "tmp", "flags"):
        os.makedirs(os.path.join(sysroot, d), exist_ok=True)

    # THE DEVICE NODES THE SHIM ANSWERS FOR, as empty files.
    #
    # Exactly the trick the LeapPad sysroot uses for /dev/fb0..2: the shim
    # intercepts open() by path, but only if the guest gets that far — and
    # portaudio stat()s /dev/dsp before opening it, so a node that does not
    # exist is never opened and never intercepted. An ordinary file makes the
    # stat succeed; nothing is ever written to it.
    # dsp        portaudio stat()s it before opening; the shim answers the
    #            OSS ioctls and forwards the samples.
    # layer0..2  the MLC's three planes — the shim maps them onto the same
    #            arena /dev/fb0..2 use everywhere else.
    # mlc dpc    the display controller's own nodes, ioctl-only.
    # gpio ga3d  opened by libDisplay and by the GL stack; without gpio,
    #            "DisplayModule::InitModule: failed to open GPIO device".
    for f in ("dev/dsp", "dev/gpio", "dev/mlc", "dev/dpc", "dev/ga3d",
              "dev/layer0", "dev/layer1", "dev/layer2"):
        touch(os.path.join(sysroot, f))

    # THE THREE SYSFS FILES THE FIRMWARE READS, AND ONLY THOSE THREE.
    #
    # Brio's libUtility.so and libDisplay.so between them name every /sys path
    # this device uses, and there are three. Each value below is read out of
    # the firmware rather than guessed, because the firmware documents its own
    # enumerations in shell scripts that ship beside the binaries:
    #
    #   cartridge  usr/bin/cartinfo lists the whole set in its help text —
    #              production, development, manufacturing, base, none — and
    #              defaults CART_TYPE to "none". An emulator has an empty slot,
    #              so "none" is not a placeholder, it is the right answer.
    #              WITHOUT THIS FILE AppManager does not start: the open fails
    #              and CButtonModule::LightningButtonTask asserts on it.
    #   vbus       usr/bin/lftest_usb asserts `vbus = 0` for "cable is
    #              unplugged" and `= 1` for plugged in. Nothing is plugged in.
    #   status     etc/init.d/lightning switches on it and treats 3 and 4 as
    #              low battery, anything else as normal. 1 is what the
    #              LeapPad2's own capture records for EXTERNAL power (see
    #              SYSFS above, lf2000-power/status), and this is the same
    #              driver family one generation earlier.
    #
    # NOTHING ELSE IS INVENTED. The LeapPad sysroot writes some thirty sysfs
    # files whose values were read off real hardware; nobody has read a Didj's,
    # so the rest of /sys stays empty rather than plausible. The two flags
    # launch_main tests for are absent on a healthy device, so absent is also
    # correct:
    #
    #     /flags/needs_repair   set only by a failed update
    #     /flags/vbus           set while USB is plugged in
    for rel, text in DIDJ_SYSFS.items():
        write_text(os.path.join(sysroot, rel), text)

    # ONE libdl, AND IT MUST BE OURS — the same rule setup-sysroot.sh applies
    # to every other non-Qt device, for the same reason. AppManager here names
    # libdl.so.0 in its DT_NEEDED, which is how the shim gets into the process
    # at all; leaving /lib/libdl.so.0 pointing at the real uClibc one puts a
    # second dl provider in the link map beside the shim.
    #
    # INTO THE SYSROOT, NOT THROUGH IT. sysroot/lib is a real directory here
    # (see the union above), so this writes where it says it does. On the
    # LeapPad path the same line lands inside rootfs/ because sysroot/lib is a
    # symlink to it — worth fixing there, and worth not repeating here.
    shim = os.path.join(PROJ, "runtime", "shimlibs", "libdl.so.0")
    if os.path.exists(shim):
        for d in ("lib", os.path.join("usr", "lib")):
            link_or_copy(shim, os.path.join(sysroot, d, "libdl.so.0"))

    else:
        say("    WARNING: the shim's libdl is not built — no input or display")
        say("    until 'cd tadpole && make shim' has run.")

    dev = detect_device(rootfs)
    if dev:
        write_text(os.path.join(sysroot, ".tadpole-device"), dev + "\n")
    return sysroot


def replace_didj_portaudio(sysroot):
    """Put our libportaudio and libopengles_lite over the device's own.

    WHY, in one line: the stock one's callback thread runs perfectly and its
    parent never stops waiting to be told so. shim/tadpole_portaudio.c has the
    whole account.

    HERE RATHER THAN ON LD_LIBRARY_PATH, because /Didj/Base/Brio/lib is first
    on that path and has to stay first — it is where the device's own Brio
    libraries live. AFTER the packages, because that directory is one of them
    and does not exist until they are installed.
    """
    libdir = os.path.join(sysroot, "Didj", "Base", "Brio", "lib")
    if not os.path.isdir(libdir):
        return
    for name, why in (("libportaudio.so",
                       "audio init will not return and nothing after it runs"),
                      ("libopengles_lite.so",
                       "its constructor maps the 3D registers and takes SIGBUS")):
        src = os.path.join(PROJ, "runtime", "shimlibs", name)
        if not os.path.exists(src):
            say("    %s NOT replaced — 'cd tadpole && make shimpa' first, or"
                % name)
            say("    %s." % why)
            continue
        link_or_copy(src, os.path.join(libdir, name))
        say("    %s replaced" % name)


def park_other_device(dev_id):
    """Move the live tree aside if it belongs to a DIFFERENT device.

    THIS IS NOT TIDINESS, IT IS DATA LOSS. Everything that builds a sysroot
    writes to runtime/sysroot, and installing device B while device A is live
    overwrites A's tree in place — including LF/Bulk, which holds A's installed
    CONTENT and exists nowhere else. runtime/setup-sysroot.sh has parked the
    live tree before building another since devices became switchable; the
    installer never learned to, so the one path that can destroy a working
    install was the one that did not check.

    Found by doing it: a Leapster GS that booted to its sign-in screen was
    reduced to a crash in libLightningJSON by a Didj install run beside it, and
    the only way back was a full reinstall of its packages.

    Parking is two renames — see tad_park_active() in runtime/device.sh, which
    is what actually does it, so the layout stays defined in one place.
    """
    script = ('. "%s/device.sh"; a="$(tad_active_device)"; '
              '[ -n "$a" ] && [ "$a" != "%s" ] && { echo "$a"; tad_park_active; }'
              % (os.path.join(PROJ, "runtime"), dev_id))
    try:
        out = subprocess.run(["bash", "-c", script], capture_output=True,
                             text=True, timeout=120)
    except Exception:
        return
    other = out.stdout.strip().splitlines()
    if other and other[0]:
        say("==> parked %s (its tree is at runtime/installs/%s)"
            % (other[0], other[0]))


def refresh_real_libs(rootfs):
    """Re-derive the shim's helper libraries from THIS firmware.

    THE OTHER HALF OF THE FIX IN tools/real-libs.py. runtime/setup-sysroot.sh
    refreshes them on every build and every device switch — but it refuses the
    Didj outright, because it cannot build a /Didj tree, so for that device
    nothing here would ever have run it. Installing a Didj beside a Leapster GS
    then left the GS's libdl in front of it, which is a spin in the loader on
    one device and a NULL dlsym table on the other.

    Every device, not only the Didj: an install is exactly the moment the
    answer changes, whichever tree is being laid down.
    """
    tool = os.path.join(HERE, "real-libs.py")
    if not os.path.exists(tool):
        return
    say("==> shim helper libraries")
    try:
        subprocess.run([sys.executable, tool, "--rootfs", rootfs], check=False)
    except Exception as e:
        say("    WARNING: could not refresh them (%s) — a guest may crash on" % e)
        say("    launch if they belong to another device")


# ---- main -------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("src", help="LFC_Downloads dir, or a .lfp/.lf2/.zip")
    ap.add_argument("--no-content", action="store_true",
                    help="system files only; skip the content packages")
    ap.add_argument("--device", default="",
                    help="a DEV_ID from runtime/devices: install only this "
                         "device's firmware, whatever else is in the source")
    args = ap.parse_args(argv[1:])

    src = args.src
    if not os.path.exists(src):
        die("no such path: %s" % src)

    # --device NAMES A PROFILE; find_firmware MATCHES A MANIFEST. The wizard
    # and online-update speak in DEV_IDs ("didj", "leapstergs"), while the
    # packages say Device="Didj". The profile is what maps one to the other,
    # so an unknown --device is an error here rather than a filter that
    # silently matches nothing later.
    want_meta = ""
    if args.device:
        want_meta = profile_meta_device(args.device)
        if not want_meta:
            die("no device profile named %s (see runtime/devices/)" % args.device)

    stage = tempfile.mkdtemp(prefix="tadpole-fw-")
    try:
        pkgs = gather(src, stage)
        fw, version, kind = find_firmware(pkgs, want_meta)
        say("==> firmware version %s" % version)
        _dest, rootfs = extract_rootfs(fw, version, stage, kind)

        # BEFORE ANYTHING WRITES TO runtime/sysroot. detect_device reads the
        # tree we just extracted, so this is the first moment we know whose
        # install this is — and the last moment before build_sysroot* starts
        # overwriting whatever is there.
        installing = detect_device(rootfs)
        if installing:
            park_other_device(installing)

        # THE SYSROOT BEFORE THE CONTENT. Content installs INTO the sysroot,
        # and the sort file that decides what appears on the home screen lives
        # there. The other order leaves content where nothing looks for it.
        if installing == "didj":
            sysroot = build_sysroot_didj(rootfs)
            if not args.no_content:
                install_didj_content(pkgs, sysroot, fw)
            # AFTER THE PACKAGES, not before. Half of what belongs in
            # runtime/libs is the Brio package's — /Didj/Base/Brio/lib does not
            # exist until install_didj_content has run, so linking earlier
            # silently produces a directory with the firmware's libraries and
            # none of Brio's. Everything that builds against the guest reads
            # this: tadpole/Makefile links the shim against
            # runtime/libs/libc.so.0 and refuses without it.
            link_runtime_libs(sysroot, rootfs)
            refresh_real_libs(rootfs)
            replace_didj_portaudio(sysroot)
        else:
            sysroot = build_sysroot(rootfs)
            link_runtime_libs(rootfs)
            refresh_real_libs(rootfs)
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
