package org.tadpole.view;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.PrintStream;

/**
 * The eleven bytes-in-place edits that let the guest's own loader run inside an
 * Android app's seccomp filter.
 *
 * <p>WHY THIS CANNOT BE DONE IN THE SHIM, which is where everything else of
 * this kind lives. The shim is a shared library; it is loaded BY ld-uClibc,
 * which means ld-uClibc has already run — has already opened, stat'd and mapped
 * every library — before a single line of ours exists. The syscalls it makes on
 * the way are unreachable by any interposition, so the only thing left is the
 * binary.
 *
 * <p>WHAT IS BEING WORKED AROUND. An app process runs under the seccomp-bpf
 * filter zygote installs, filters are inherited across execve, and thirteen
 * syscalls are refused — measured with android/seccomp-probe.c, not inferred.
 * A refusal is not an error return; it is SIGSYS and a dead process. Three of
 * the thirteen are stat, lstat and fstat, which bionic reaches through
 * fstatat64, so the legacy numbers were never generated into the whitelist.
 * uClibc from 2013 uses them directly.
 *
 * <p>WHY IT IS SAFE, patch by patch — and this is the part that matters, since
 * this is editing someone else's libc:
 *
 * <ul>
 *   <li>The two ld.so `svc #0` sites become `mvn r0,#0`, an immediate -1. Both
 *       are stat() calls whose failure path already exists and is already
 *       taken: there is no /etc/ld.so.cache on this firmware, so that lookup
 *       was failing before we touched it, and the main program's stat only
 *       fills in l_dev/l_ino.
 *   <li>The _dl_load_elf_shared_library group does not remove a call, it
 *       UPGRADES one: `mov r7,#108` -> `#197` is __NR_fstat -> __NR_fstat64,
 *       the question the filter allows. The other five edits enlarge the stack
 *       frame from a 88-byte struct stat to a 104-byte struct stat64 and move
 *       the two field reads to where stat64 keeps them.
 *   <li>libpthread's two are set_robust_list, which writes nothing to
 *       userspace, so `mov r0,#0` is not a lie about what happened.
 * </ul>
 *
 * <p>VERSION-LOCKED ON PURPOSE. Every edit carries the bytes it expects to
 * find. If a file does not match — a different firmware build, a different
 * uClibc — nothing at all is written to it and the reason is printed. Patching
 * by offset into a binary you have not identified is how you get a system that
 * loads and then misbehaves somewhere else entirely. Already-patched bytes are
 * recognised and count as success, so this is safe to run twice.
 *
 * <p>Applied HERE, from the firmware install, because the install extracts the
 * rootfs fresh and would otherwise silently undo them: the guest would keep
 * working under the root helper and stop working without it, which is a
 * confusing way to lose a feature. The originals are kept as {@code *.orig}.
 */
final class SeccompPatch {

    private SeccompPatch() {}

    private static final class Patch {
        final String file; final int off; final byte[] from, to;
        Patch(String file, int off, String from, String to) {
            this.file = file; this.off = off;
            this.from = hex(from); this.to = hex(to);
        }
    }

    /** Sizes the edits were measured against; a mismatch refuses the file. */
    private static final String LD  = "ld-uClibc-0.9.32.1-git.so";
    private static final String PT  = "libpthread-0.9.32.1-git.so";
    private static final int LD_SZ  = 29488, PT_SZ = 71184;

    private static final Patch[] PATCHES = {
        /* ld-uClibc: _dl_map_cache's stat("/etc/ld.so.cache") -> mvn r0,#0.
         * There is no cache file on this firmware, so the call was already
         * failing and the no-cache route was already the one taken. */
        new Patch(LD, 0x22E2, "00ef", "e0e3"),

        /* _dl_load_elf_shared_library: fstat -> fstat64, and the frame to
         * match. sub sp,sp,#292 -> #416 makes room for the bigger struct... */
        new Patch(LD, 0x3DB4, "49df", "1ade"),
        /* ...sub r1,r11,#100 -> #432 puts it above the outgoing arguments... */
        new Patch(LD, 0x3DF4, "6410", "1b1e"),
        /* ...mov r7,#108 -> #197 is __NR_fstat -> __NR_fstat64 itself... */
        new Patch(LD, 0x3DF8, "6c", "c5"),
        /* ...and the four reads move to where stat64 keeps those fields:
         * st_dev at the same place, st_ino at __st_ino +12. */
        new Patch(LD, 0x3E30, "6400", "b001"),
        new Patch(LD, 0x3E60, "6030", "a431"),
        new Patch(LD, 0x4618, "6400", "b001"),
        new Patch(LD, 0x4628, "6000", "a401"),

        /* _dl_get_ready_to_run's stat of the main program -> mvn r0,#0. The
         * error path sets errno and rejoins, losing only l_dev/l_ino. */
        new Patch(LD, 0x581E, "00ef", "e0e3"),

        /* libpthread: set_robust_list -> mov r0,#0. The kernel writes nothing
         * back to userspace for this call, so reporting success is exact. */
        new Patch(PT, 0x8916, "00ef", "a0e3"),
        new Patch(PT, 0xAC96, "00ef", "a0e3"),
    };

    /**
     * Patches what it finds in {@code libDir}. Never throws; a firmware that
     * does not match is reported and left exactly as it was.
     */
    static void apply(File libDir, PrintStream out) {
        one(new File(libDir, LD), LD, LD_SZ, out);
        one(new File(libDir, PT), PT, PT_SZ, out);
    }

    private static void one(File f, String name, int size, PrintStream out) {
        if (!f.isFile()) return;             /* not this firmware's layout */
        if (f.length() != size) {
            out.println("  seccomp: " + name + " is " + f.length() + " bytes, not "
                        + size + " — left alone");
            return;
        }
        byte[] b;
        try { b = readAll(f); } catch (IOException e) {
            out.println("  seccomp: cannot read " + name + ": " + e.getMessage());
            return;
        }

        /* CHECKED IN FULL BEFORE ANYTHING IS WRITTEN. A half-patched loader is
         * worse than an unpatched one: it would load, and fail later. */
        int todo = 0;
        for (Patch p : PATCHES) {
            if (!p.file.equals(name)) continue;
            if (match(b, p.off, p.from))    { todo++; continue; }
            if (match(b, p.off, p.to))      { continue; }      /* already done */
            out.println("  seccomp: " + name + " does not match at 0x"
                        + Integer.toHexString(p.off) + " — nothing patched");
            return;
        }
        if (todo == 0) return;               /* already patched, silently */

        File orig = new File(f.getAbsolutePath() + ".orig");
        if (!orig.isFile()) {
            try { Tools.copyTree(f, orig); } catch (IOException e) {
                out.println("  seccomp: cannot back up " + name + ": " + e.getMessage());
                return;
            }
        }
        for (Patch p : PATCHES)
            if (p.file.equals(name)) System.arraycopy(p.to, 0, b, p.off, p.to.length);

        try {
            OutputStream os = new FileOutputStream(f);
            try { os.write(b); } finally { os.close(); }
        } catch (IOException e) {
            out.println("  seccomp: cannot write " + name + ": " + e.getMessage());
            return;
        }
        out.println("  seccomp: " + name + " patched (" + todo + " edit(s))");
    }

    private static boolean match(byte[] b, int off, byte[] want) {
        if (off < 0 || off + want.length > b.length) return false;
        for (int i = 0; i < want.length; i++) if (b[off + i] != want[i]) return false;
        return true;
    }

    private static byte[] hex(String s) {
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++)
            out[i] = (byte) Integer.parseInt(s.substring(i * 2, i * 2 + 2), 16);
        return out;
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
