package org.tadpole.view;

import java.io.File;
import java.io.IOException;
import java.io.PrintStream;

/**
 * Puts the guest-side shim libraries in place, which is what makes the
 * firmware bootable at all.
 *
 * <p>WITHOUT THESE THE GUEST STARTS AND THEN DIES, and it took a tester's log
 * to see it, because both of the devices this was developed on had been handed
 * the shim by push-firmware.sh from a desktop. Anyone who installed the APK and
 * used Online System Update got a sysroot with no shim in it at all, and the
 * guest's own log says exactly what that looks like:
 *
 * <pre>
 *   [0x5] InitModule: Screen = 0 x 0, pitch = 0
 *   [0x5] InitModule: Mapped 00000000 to 0xffffffff, size 00000000
 *   [0x5] CreateHandle: No framebuffer allocation available
 *         Can't find touchscreen event device in /dev/input
 *         cannot open: gpio-keys / Power Button / USB / Accelerometer
 *   Unhandled SIGSEGV -> GUEST FAULT: 3bfffe5c was never mapped
 * </pre>
 *
 * <p>No framebuffer and no input devices, because the thing that fakes them was
 * never installed. Sysroot.oneLibdl() has been printing "(no shim libdl yet)"
 * this whole time and carrying on.
 *
 * <p>TWO KINDS OF FILE, AND ONLY ONE OF THEM SHIPS.
 *
 * <ul>
 *   <li>Ours — libdl.so.0, libasound.so.2, libz.so.1, libEGL.so,
 *       libGLESv1_CM.so — are built from tadpole/shim/*.c and travel in the
 *       APK as assets. TadpoleActivity unpacks them.
 *   <li>libdl.so.9 and libz.so.9 are the FIRMWARE'S OWN libraries under a
 *       second name, so the impersonator can chain through to the real one.
 *       They cannot ship: Tadpole contains no LeapFrog code and neither does
 *       this APK. They are derived here from the firmware the user installed,
 *       by the same one-string patch the desktop Makefile does.
 * </ul>
 */
final class Shims {

    private Shims() {}

    /** Called once the sysroot exists — the derived pair need the firmware. */
    static void install(File rootfs, PrintStream out) {
        File shim   = new File(Tools.proj(), "runtime/shimlibs");
        File shimZ  = new File(Tools.proj(), "runtime/shimlibs-z");
        File shimGl = new File(Tools.proj(), "runtime/shimlibs-gl");

        if (!new File(shim, "libdl.so.0").isFile()) {
            /* The assets are unpacked at startup, so this means an APK built
             * without them — say so rather than leaving a guest that dies with
             * a fault address and no explanation. */
            out.println("  shim: libdl.so.0 is missing from this build —"
                      + " the guest will not boot");
            return;
        }

        int n = 0;
        n += rename(new File(rootfs, "lib"), "libdl-", "libdl.so.0", "libdl.so.9",
                    new File(shim, "libdl.so.9"), out) ? 1 : 0;
        /* FROM THE ROOTFS, NOT THE SYSROOT. This runs before Sysroot.build(),
         * so runtime/sysroot/usr/lib does not exist yet — and a tester's log
         * caught it: libdl.so.9 was made (it reads the rootfs) while libz.so.9
         * was not, and the guest stopped at
         *
         *     /LF/Base/bin/AppManager: can't load library 'libz.so.9'
         *
         * The order cannot simply be swapped: Sysroot.oneLibdl() links the
         * shim into the tree it builds, so the shim has to be there first. */
        n += rename(new File(rootfs, "usr/lib"), "libz.so.1", "libz.so.1",
                    "libz.so.9", new File(shimZ, "libz.so.9"), out) ? 1 : 0;

        /* The loader resolves DT_NEEDED by SONAME as a FILENAME, so libEGL's
         * dependency on libGLESv1_CM.so.1 needs that exact name on disk. And
         * native titles link libopengles_lite.so, which on the real device is
         * a symlink to the same library — without it the search falls through
         * to the stock driver, which then cannot resolve its own EGL internals
         * against our libEGL. */
        link(shimGl, "libGLESv1_CM.so", "libGLESv1_CM.so.1", out);
        link(shimGl, "libEGL.so",       "libEGL.so.1",       out);
        link(shimGl, "libGLESv1_CM.so", "libopengles_lite.so", out);

        out.println("  shim: ready (" + n + " derived from your firmware)");
    }

    /**
     * Copy one of the firmware's libraries under a second name, with its
     * SONAME string rewritten so both can be loaded at once.
     *
     * <p>A byte replacement rather than an ELF edit, because the two names are
     * the same length by construction — libdl.so.0 -> libdl.so.9 — so nothing
     * moves and no offset in the file changes.
     */
    private static boolean rename(File dir, String prefix, String from, String to,
                                  File dest, PrintStream out) {
        if (dest.isFile() && dest.length() > 0) return false;   /* already done */
        File src = pick(dir, prefix);
        if (src == null) {
            out.println("  shim: no " + prefix + "* in " + Tools.rel(dir)
                      + " — cannot make " + dest.getName());
            return false;
        }
        try {
            byte[] b = readAll(src);
            byte[] a = (from + "\0").getBytes("ISO-8859-1");
            byte[] c = (to + "\0").getBytes("ISO-8859-1");
            int hits = 0;
            for (int i = 0; i + a.length <= b.length; i++) {
                boolean m = true;
                for (int j = 0; j < a.length && m; j++) m = b[i + j] == a[j];
                if (!m) continue;
                System.arraycopy(c, 0, b, i, c.length);
                hits++;
            }
            if (hits == 0) {
                out.println("  shim: " + src.getName() + " does not name itself "
                          + from + " — left alone");
                return false;
            }
            File p = dest.getParentFile();
            if (p != null && !p.isDirectory() && !p.mkdirs()) return false;
            java.io.OutputStream os = new java.io.FileOutputStream(dest);
            try { os.write(b); } finally { os.close(); }
            return true;
        } catch (IOException e) {
            out.println("  shim: cannot make " + dest.getName() + ": " + e.getMessage());
            return false;
        }
    }

    /**
     * The firmware spells these two different ways, and both have to work:
     * libdl is a versioned FILE (libdl-0.9.32.1-git.so) while libz.so.1 is a
     * SYMLINK to libz.so.1.2.5. So try the exact name first — following a link
     * is what we want, and gives the real bytes — then fall back to a prefix
     * scan for the versioned spelling.
     *
     * <p>The fallback deliberately does not require a ".so" suffix: the file
     * behind libz.so.1 ends in ".1.2.5", and an earlier version of this
     * insisted on ".so" and would have found nothing had the link been absent.
     */
    private static File pick(File dir, String prefix) {
        File exact = new File(dir, prefix);
        if (exact.isFile()) return exact;            /* follows a symlink */
        File[] kids = dir.listFiles();
        File best = null;
        if (kids != null)
            for (File k : kids)
                if (k.isFile() && k.getName().startsWith(prefix)
                    && (best == null || k.length() > best.length())) best = k;
        return best;
    }

    private static void link(File dir, String target, String name, PrintStream out) {
        File l = new File(dir, name);
        /* Unconditionally: exists() follows the link, so a stale one reports
         * false and never gets replaced — the same trap linkEngine documents. */
        l.delete();
        try {
            android.system.Os.symlink(target, l.getAbsolutePath());
        } catch (Throwable t) {
            out.println("  shim: could not link " + name + ": " + t);
        }
    }

    private static byte[] readAll(File f) throws IOException {
        byte[] b = new byte[(int) f.length()];
        java.io.InputStream in = new java.io.FileInputStream(f);
        try {
            int off = 0, n;
            while (off < b.length && (n = in.read(b, off, b.length - off)) > 0) off += n;
            if (off != b.length) throw new IOException("short read");
        } finally { in.close(); }
        return b;
    }
}
