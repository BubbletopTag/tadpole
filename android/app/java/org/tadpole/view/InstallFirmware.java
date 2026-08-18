package org.tadpole.view;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import java.util.zip.ZipInputStream;

/**
 * tools/install-firmware.py, in Java: LeapPad2 system files, on the device.
 *
 *     install-firmware &lt;LFC_Downloads dir | .lfp | .zip&gt;
 *
 * <p>You supply the firmware. Tadpole ships no LeapFrog code; this reads the
 * packages LFConnect leaves in its download cache.
 *
 * <p>WHAT THE PACKAGES ARE. LFConnect's cache is a flat pile of hash-named
 * files, so the only way to know what one is, is to read the manifest inside:
 *
 * <ul>
 * <li>.lfp — an ordinary ZIP. The base firmware is one of these, and inside it
 *     is a UBI image: {@code Firmware-Base/5,53477376,C4G-E1M-W4K-erootfs.ubi}.
 * <li>.lf2 — a bzip2 tar despite the extension. These are the CONTENT packs,
 *     and they are not optional: the base image alone does not boot, dying in
 *     libLightningBase having said only "CMfgData::Init:
 *     GetNorPartitionFilename failed", because the widgets it wants live in
 *     them. InstallContent handles them once the rootfs is down.
 * <li>.lf3 — encrypted; skipped without keys, same as the Python.
 * </ul>
 *
 * <p>THE ROOT FILESYSTEM IS A UBIFS VOLUME, not a tar, and the Python says so
 * and hands the job to ubi_reader. There is no ubi_reader here and no Python to
 * run it with, so Ubi and Ubifs read it directly — verified byte-for-byte
 * against ubi_reader's output on a real image before this was wired up.
 *
 * <p>This is install-firmware.sh, setup-sysroot.sh and the extraction as one
 * program, the same way the Python is.
 */
final class InstallFirmware implements Tools.Tool {

    @Override
    public boolean run(String[] argv, PrintStream out) throws Exception {
        if (argv.length == 0) {
            out.println("usage: install-firmware <LFC_Downloads folder or .lfp>");
            return false;
        }
        File src = new File(argv[0]);
        if (!src.exists()) { out.println("no such path: " + src); return false; }

        List<File> zips = new ArrayList<File>();
        List<String> skipped = new ArrayList<String>();
        collect(src, zips, skipped, out);

        if (zips.isEmpty()) {
            out.println("no firmware package here.");
            out.println("Point this at the LFC_Downloads folder LFConnect left on your");
            out.println("computer, or at the .lfp inside it.");
            if (!skipped.isEmpty())
                out.println("Seen but not readable here: " + join(skipped));
            return false;
        }

        File ubiFile = null;
        String version = "";
        for (File z : zips) {
            Found f = findUbi(z, out);
            if (f != null) { ubiFile = f.ubi; version = f.version; break; }
        }
        if (ubiFile == null) {
            out.println("none of the " + zips.size() + " package(s) holds a rootfs image.");
            if (!skipped.isEmpty())
                out.println("Not readable here: " + join(skipped));
            return false;
        }

        if (version.isEmpty()) version = "installed";
        File rootfs = new File(new File(Tools.proj(), "rootfs"), version);
        File ubiRfs = new File(rootfs, "ubi_rfs");
        out.println("==> extracting into " + Tools.rel(ubiRfs));

        Tools.rmTree(ubiRfs);
        Ubi ubi = Ubi.open(ubiFile);
        try {
            Ubifs fs = Ubifs.find(ubi);
            out.println("  volume " + fs.volumeName());
            fs.scan(out);
            fs.extract(ubiRfs, out);
        } finally {
            ubi.close();
            ubiFile.delete();                 /* the unpacked copy, not the .lfp */
        }

        out.println("==> sysroot");
        new Sysroot(Tools.proj(), ubiRfs, Tools.sysroot(), out).build();

        /* THE CONTENT PACKS ARE PART OF THE INSTALL, not an optional extra.
         * The base firmware on its own does not boot — AppManager dies in
         * libLightningBase having said only "CMfgData::Init:
         * GetNorPartitionFilename failed" — and what it wants is the widgets
         * these carry. Doing them here means one action installs a system that
         * starts, rather than one that extracts. */
        out.println("==> content");
        new InstallContent().install(src, out);

        out.println("");
        out.println("Installed. Tadpole can boot the system menu now.");
        return true;
    }

    /* ---- finding the packages --------------------------------------------- */

    private void collect(File src, List<File> zips, List<String> skipped, PrintStream out) {
        if (src.isFile()) { classify(src, zips, skipped); return; }
        List<File> all = new ArrayList<File>();
        walk(src, all, 0);
        Collections.sort(all);
        /* BIGGEST FIRST. The base firmware is by far the largest package in a
         * download cache — 33 MB against a few hundred KB for the rest — so
         * this finds it on the first open rather than after twenty. */
        Collections.sort(all, new java.util.Comparator<File>() {
            @Override public int compare(File a, File b) {
                return Long.compare(b.length(), a.length());
            }
        });
        for (File f : all) classify(f, zips, skipped);
        out.println("==> " + zips.size() + " package(s) to look in"
                    + (skipped.isEmpty() ? "" : ", " + skipped.size() + " skipped"));
    }

    private void walk(File dir, List<File> out, int depth) {
        if (depth > 6) return;
        File[] kids = dir.listFiles();
        if (kids == null) return;
        for (File f : kids) {
            if (f.isDirectory()) walk(f, out, depth + 1);
            else if (f.isFile() && f.length() > 0) out.add(f);
        }
    }

    private void classify(File f, List<File> zips, List<String> skipped) {
        String n = f.getName().toLowerCase(Locale.ROOT);
        if (n.endsWith(".lf3")) { skipped.add(f.getName() + " (encrypted)"); return; }
        /* .lf2 is a bzip2 tar and Tar reads those now; it is content rather
         * than firmware, so it is not looked in for a UBI image — installing
         * it is InstallContent's job, below. */
        if (n.endsWith(".lf2")) return;
        if (looksLikeZip(f)) zips.add(f);
        else if (n.endsWith(".lfp") || n.endsWith(".zip")) skipped.add(f.getName());
    }

    private static boolean looksLikeZip(File f) {
        InputStream in = null;
        try {
            in = new java.io.FileInputStream(f);
            int a = in.read(), b = in.read();
            return a == 'P' && b == 'K';
        } catch (IOException e) {
            return false;
        } finally {
            if (in != null) try { in.close(); } catch (IOException e) { /* ignore */ }
        }
    }

    private static final class Found {
        File ubi; String version = "";
    }

    /**
     * Look inside one ZIP for a UBI image, unpacking it to a temporary file.
     *
     * <p>UNPACKED RATHER THAN READ IN PLACE because the reader seeks all over
     * it — a ZIP entry is a forward-only stream, and a 51 MB volume is not
     * something to hold in a tablet's heap.
     */
    private Found findUbi(File zip, PrintStream out) {
        ZipFile zf = null;
        try {
            zf = new ZipFile(zip);
            ZipEntry ubi = null, meta = null;
            java.util.Enumeration<? extends ZipEntry> en = zf.entries();
            while (en.hasMoreElements()) {
                ZipEntry e = en.nextElement();
                String n = e.getName().toLowerCase(Locale.ROOT);
                if (n.endsWith(".ubi") && (ubi == null || e.getSize() > ubi.getSize())) ubi = e;
                if (n.endsWith("meta.inf") && meta == null) meta = e;
            }
            if (ubi == null) return null;

            Found f = new Found();
            if (meta != null) {
                String m = readText(zf, meta);
                String v = Tools.field(m, "Version");
                String name = Tools.field(m, "Name");
                if (!v.isEmpty()) f.version = sanitise(name.isEmpty() ? v : name + "-" + v);
                out.println("  " + zip.getName() + ": " + name + " " + v);
            } else {
                out.println("  " + zip.getName() + ": a firmware image");
            }

            out.println("  unpacking " + ubi.getName() + " ("
                        + (ubi.getSize() / (1024 * 1024)) + " MB)");
            File tmp = File.createTempFile("tadpole-ubi", ".ubi", Tools.proj());
            InputStream in = zf.getInputStream(ubi);
            try {
                OutputStream os = new java.io.FileOutputStream(tmp);
                try {
                    byte[] buf = new byte[1 << 16];
                    long done = 0, mark = 0, total = ubi.getSize();
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        os.write(buf, 0, n);
                        done += n;
                        /* Every 8 MB: a 51 MB copy off a tablet's flash is a
                         * minute of nothing otherwise. */
                        if (done - mark >= 8L << 20) {
                            mark = done;
                            out.println("    " + (done >> 20) + " of "
                                        + (total >> 20) + " MB...");
                        }
                    }
                } finally { os.close(); }
            } finally { in.close(); }
            f.ubi = tmp;
            return f;
        } catch (IOException e) {
            out.println("  " + zip.getName() + ": " + e.getMessage());
            return null;
        } finally {
            if (zf != null) try { zf.close(); } catch (IOException e) { /* ignore */ }
        }
    }

    private static String readText(ZipFile zf, ZipEntry e) throws IOException {
        InputStream in = zf.getInputStream(e);
        try {
            java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
            byte[] buf = new byte[4096];
            int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            return new String(bo.toByteArray(), "UTF-8");
        } finally { in.close(); }
    }

    /** A version string is going into a path, so it may not carry a separator. */
    private static String sanitise(String s) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < s.length() && sb.length() < 64; i++) {
            char c = s.charAt(i);
            sb.append(Character.isLetterOrDigit(c) || c == '.' || c == '-' || c == '_'
                      ? c : '-');
        }
        return sb.toString();
    }

    private static String join(List<String> l) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < l.size() && i < 4; i++) {
            if (i > 0) sb.append(", ");
            sb.append(l.get(i));
        }
        if (l.size() > 4) sb.append(" and ").append(l.size() - 4).append(" more");
        return sb.toString();
    }
}
