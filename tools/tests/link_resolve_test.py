#!/usr/bin/env python3
"""Do the rootfs's symlinks resolve the way the guest's kernel resolves them?

    ./tools/tests/link_resolve_test.py

One script, no framework, run by hand or by CI — the same shape as
glasspole/tests/path_test.cpp, and for the same reason. install-firmware.py
copies rather than symlinks on Windows, so it has to follow the rootfs's links
itself, and it has to follow them like the device does: "/sbin/killall5" means
the ROOTFS's /sbin/killall5, and ".." at the rootfs root is the rootfs root.
Getting that wrong is invisible on Linux, where the links are never followed at
all, so this is the only place it can be caught.

The fixture is built here rather than read out of rootfs/, because the firmware
is not redistributable and is not in the repository — a test that needs a
dumped LeapPad to run is a test nobody runs.
"""
import importlib.util
import os
import shutil
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)

# install-firmware.py is not an importable name (the hyphen), and it is a
# script rather than a package, so load it by path.
_spec = importlib.util.spec_from_file_location(
    "install_firmware", os.path.join(TOOLS, "install-firmware.py"))
fw = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(fw)

fails = []


def check(what, got, want):
    if got != want:
        fails.append("%s\n     got  %r\n     want %r" % (what, got, want))


def build_fixture(root):
    """A miniature of the shapes the real rootfs actually contains."""
    for d in ("bin", "sbin", "etc/init.d", "etc/rc.d", "var/sounds", "LF/Base"):
        os.makedirs(os.path.join(root, d), exist_ok=True)

    def f(rel, text="x"):
        with open(os.path.join(root, rel), "w") as fh:
            fh.write(text)

    f("bin/busybox")
    f("sbin/killall5")
    f("sbin/depmod.26")
    f("etc/init.d/lightning")

    def ln(target, rel):
        os.symlink(target, os.path.join(root, rel))

    # Guest-absolute, target present in the rootfs.
    ln("/sbin/killall5", "bin/pidof.sysvinit")
    # A chain: pidof -> pidof.sysvinit -> /sbin/killall5. Two hops.
    ln("pidof.sysvinit", "bin/pidof")
    # Relative, and ".." walks off the top of the rootfs. The guest clamps it.
    ln("../bin/busybox", "linuxrc")
    # Relative, ordinary.
    ln("../init.d/lightning", "etc/rc.d/S90lightning")
    # Guest-absolute, target genuinely not in this firmware.
    ln("/LF/Bulk/LanguagePack_en/Tutorials", "LF/Base/Tutorials")
    # Relative, target genuinely absent.
    ln("../../LF/Base/LucyAssets/Video/shutdown.ogg", "var/sounds/Shutdown.ogg")
    # A loop.
    ln("loop_b", "bin/loop_a")
    ln("loop_a", "bin/loop_b")
    # An attempt to reach the host filesystem.
    ln("/../../../../etc/passwd", "bin/escape")
    # A directory symlink whose target is present.
    ln("/etc/init.d", "etc/rc.d/all")
    # A directory link naming a path that EXISTS ON THE HOST TOO. The guest's
    # /etc must win; copying the host's /etc into the sysroot would be both
    # wrong and a way to put arbitrary host files inside the image.
    f("etc/FIXTURE_MARKER")
    ln("/etc", "bin/hostetc")
    # A directory link back to the root. Following it while copying is an
    # infinite descent; the device never follows it that way because nothing
    # ever walks the tree there.
    ln("/", "bin/rootlink")


def main():
    root = tempfile.mkdtemp(prefix="link-resolve-")
    try:
        build_fixture(root)
        j = lambda *p: os.path.join(root, *p)

        # A guest-absolute link resolves against the rootfs, not the host.
        check("absolute target inside the rootfs",
              fw.guest_resolve(root, j("bin", "pidof.sysvinit")),
              j("sbin", "killall5"))

        # A chain is followed to the end.
        check("chain pidof -> pidof.sysvinit -> /sbin/killall5",
              fw.guest_resolve(root, j("bin", "pidof")),
              j("sbin", "killall5"))

        # ".." at the rootfs root is the rootfs root, as it is at "/".
        check("'..' clamped at the rootfs root",
              fw.guest_resolve(root, j("linuxrc")),
              j("bin", "busybox"))

        # An ordinary relative link.
        check("ordinary relative link",
              fw.guest_resolve(root, j("etc", "rc.d", "S90lightning")),
              j("etc", "init.d", "lightning"))

        # Not a link at all: it is its own answer.
        check("a plain file resolves to itself",
              fw.guest_resolve(root, j("bin", "busybox")),
              j("bin", "busybox"))

        # A directory target is resolved like any other.
        check("directory symlink",
              fw.guest_resolve(root, j("etc", "rc.d", "all")),
              j("etc", "init.d"))

        # Absent in the guest too — the device has these dangling as well.
        check("absolute target absent from the firmware",
              fw.guest_resolve(root, j("LF", "Base", "Tutorials")), None)
        check("relative target absent from the firmware",
              fw.guest_resolve(root, j("var", "sounds", "Shutdown.ogg")), None)

        # A loop must not spin.
        check("symlink loop", fw.guest_resolve(root, j("bin", "loop_a")), None)

        # Never hand back a path outside the rootfs, whatever the link says.
        check("cannot escape the rootfs",
              fw.guest_resolve(root, j("bin", "escape")), None)

        # ---- and now the copy that install-firmware.py actually performs ----
        dst = tempfile.mkdtemp(prefix="link-resolve-dst-")
        shutil.rmtree(dst)
        fw.copy_tree_as_guest(root, root, dst)
        d = lambda *p: os.path.join(dst, *p)

        def content(p):
            if not os.path.exists(p):
                return None
            with open(p) as fh:
                return fh.read()

        # The whole bug: this used to raise shutil.Error and abort the install.
        check("dangling link is copied as its resolved target",
              content(d("bin", "pidof.sysvinit")), "x")
        check("a chain resolves to the file at the end",
              content(d("bin", "pidof")), "x")
        check("'..' off the top still finds busybox",
              content(d("linuxrc")), "x")
        check("an ordinary file is copied",
              content(d("bin", "busybox")), "x")

        # Absent in the guest: skipped, exactly as the device has them.
        check("absent target is skipped, not fatal",
              os.path.exists(d("LF", "Base", "Tutorials")), False)
        check("absent relative target is skipped",
              os.path.exists(d("var", "sounds", "Shutdown.ogg")), False)
        check("a loop is skipped", os.path.exists(d("bin", "loop_a")), False)

        # The guest's /etc, never the host's.
        check("host /etc is not copied into the sysroot",
              os.path.exists(d("bin", "hostetc", "FIXTURE_MARKER")), True)
        check("host /etc/passwd did not come along",
              os.path.exists(d("bin", "hostetc", "passwd")), False)
        check("a directory link back to the root does not recurse",
              os.path.exists(d("bin", "rootlink")), False)
        shutil.rmtree(dst, ignore_errors=True)
    finally:
        shutil.rmtree(root, ignore_errors=True)

    if fails:
        print("FAIL (%d)" % len(fails))
        for f in fails:
            print("  " + f)
        return 1
    print("ok — guest symlink resolution")
    return 0


if __name__ == "__main__":
    sys.exit(main())
