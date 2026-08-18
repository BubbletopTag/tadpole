package org.tadpole.view;

import java.io.File;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/**
 * tools/install-content.sh, in Java: the packs that turn a booting firmware
 * into a usable one.
 *
 * <p>LeapFrog ships content as .lfp (ZIP) and .lf2 (bzip2 tar), each with a
 * meta.inf naming its Type and PackageID, and the destination is decided by the
 * Type — the same table lfpkg uses on the device:
 *
 * <pre>
 *   Application            -&gt; LF/Bulk/ProgramFiles/&lt;PackageID&gt;/
 *   Download|MicroDownload -&gt; LF/Bulk/Downloads/&lt;PackageID&gt;/
 *   LanguagePack           -&gt; LF/Bulk/            (the tarball self-wraps)
 *   Music|MusicInfo        -&gt; LF/Bulk/Music/&lt;PackageID&gt;/
 *   DeviceAsset            -&gt; into its PARENT package's directory
 *   DiskImage|System       -&gt; skipped, that is firmware
 * </pre>
 *
 * <p>SOME PACKAGES ARE FLAT AND SOME SELF-WRAP, and telling them apart is not
 * cosmetic. A self-wrapping archive extracted into a named directory buries its
 * meta.inf one level down, and CSystemData::FindWidget checks
 * FileExists(&lt;entry&gt;/meta.inf) — so the widget becomes invisible and the
 * home screen simply does not show it. A wrapper is detected as "one top-level
 * name, and no meta.inf at the top" and extracted to the PARENT so it names its
 * own directory.
 *
 * <p>DEVICE ASSETS GO INSIDE SOMEBODY ELSE. They carry an app's home-screen
 * icon and its GameInfo.json, and lfpkg installs them into the parent package's
 * directory rather than one of their own. Its rule, from usr/bin/lfpkg: take
 * field 2 of the asset's PackageID, then field 3 with -DA- replaced by -00-,
 * and find the package whose meta.inf mentions that pair — so
 * PADS-0x001F0006-DA0000 belongs to 0x001F0006-000000. Without it the app
 * installs and has no icon, and the home screen never shows it. That is why
 * this runs in two passes: everything else first, so the parents exist to be
 * found.
 */
final class InstallContent implements Tools.Tool {

    @Override
    public boolean run(String[] argv, PrintStream out) throws Exception {
        if (argv.length == 0) { out.println("usage: install-content <folder>"); return false; }
        File src = new File(argv[0]);
        if (!src.exists()) { out.println("no such path: " + src); return false; }
        return install(src, out);
    }

    /** Also called straight after a firmware install, from InstallFirmware. */
    boolean install(File src, PrintStream out) throws Exception {
        List<File> packs = new ArrayList<File>();
        if (src.isFile()) packs.add(src); else walk(src, packs, 0);
        if (packs.isEmpty()) { out.println("  no content packages found"); return true; }

        File bulk = new File(Tools.sysroot(), "LF/Bulk");
        int app = 0, dl = 0, lang = 0, music = 0, asset = 0, skip = 0;

        /* MANIFESTS FIRST, ONCE EACH, and this is not tidiness. Opening a pack
         * means decompressing it, and a .lf2 is a bzip2 tar — so asking each
         * one "what type are you" inside both passes decompressed all seventy
         * of them TWICE, once to find the non-assets and once to find the
         * assets, with the answer thrown away each time. On a desktop that is a
         * slow minute; on the tablet the install sat on "firmware..." for a
         * quarter of an hour with pass 2 grinding through packages it was about
         * to skip. Reading the manifest once costs one decompression per pack
         * and the passes then only open what they are actually going to
         * extract. */
        java.util.Map<File, String[]> manifest = new java.util.HashMap<File, String[]>();
        out.println("  reading " + packs.size() + " manifests...");
        int read = 0;
        for (File f : packs) {
            if (++read % 10 == 0) out.println("    " + read + " of " + packs.size() + "...");
            Pack p = null;
            try {
                p = Pack.open(f);
                if (p == null || p.field("PackageID").isEmpty()) { skip++; continue; }
                manifest.put(f, new String[] { p.field("Type"), p.field("PackageID"),
                                               p.field("Name") });
            } catch (Exception e) {
                out.println("  " + f.getName() + ": " + e.getMessage());
                skip++;
            } finally {
                if (p != null) p.close();
            }
        }

        for (int pass = 1; pass <= 2; pass++) {
            for (File f : packs) {
                String[] mf = manifest.get(f);
                if (mf == null) continue;
                String type = mf[0], pid = mf[1];
                boolean isAsset = type.equals("DeviceAsset");
                if (pass == 1 && isAsset) continue;
                if (pass == 2 && !isAsset) continue;

                Pack p = null;
                try {
                    p = Pack.open(f);
                    if (p == null) continue;

                    if (type.equals("Application")) {
                        extractAs(p, new File(bulk, "ProgramFiles/" + pid));
                        say(out, type, pid, p.field("Name")); app++;
                    } else if (type.equals("Download") || type.equals("MicroDownload")) {
                        p.extractTo(new File(bulk, "Downloads/" + pid));
                        say(out, type, pid, p.field("Name")); dl++;
                    } else if (type.equals("LanguagePack")) {
                        p.extractTo(bulk);
                        say(out, type, pid, p.field("Name")); lang++;
                    } else if (type.equals("Music") || type.equals("MusicInfo")) {
                        p.extractTo(new File(bulk, "Music/" + pid));
                        say(out, type, pid, p.field("Name")); music++;
                    } else if (isAsset) {
                        File parent = findAssetParent(bulk, pid);
                        if (parent == null) {
                            out.println("  DeviceAsset    " + pid + " -> no parent (skipped)");
                            skip++;
                        } else {
                            p.extractTo(parent);
                            say(out, type, pid, "-> " + parent.getName()); asset++;
                        }
                    } else if (type.equals("DiskImage") || type.equals("System")) {
                        skip++;                       /* firmware, not content */
                    } else {
                        p.extractTo(new File(bulk, "Downloads/" + pid));
                        say(out, type, pid, p.field("Name")); dl++;
                    }
                } catch (Exception e) {
                    out.println("  " + f.getName() + ": " + e.getMessage());
                    skip++;
                } finally {
                    if (p != null) p.close();
                }
            }
        }
        out.println("  " + app + " app(s), " + dl + " download(s), " + lang
                    + " language pack(s), " + music + " music, " + asset
                    + " device asset(s), " + skip + " skipped");
        profileAccess(bulk, out);
        /* The Music app reads a database, not the filesystem — see MusicDb. */
        if (music > 0 || new File(bulk, "Music").isDirectory())
            MusicDb.build(bulk, out);
        return true;
    }

    private static void say(PrintStream out, String type, String pid, String name) {
        out.println(String.format(Locale.ROOT, "  %-14s %-26s %s", type, pid, name));
    }

    private void walk(File dir, List<File> out, int depth) {
        if (depth > 6) return;
        File[] kids = dir.listFiles();
        if (kids == null) return;
        for (File f : kids) {
            if (f.isDirectory()) { walk(f, out, depth + 1); continue; }
            String n = f.getName().toLowerCase(Locale.ROOT);
            if (n.endsWith(".lf2") || n.endsWith(".lfp")) out.add(f);
        }
    }

    /** Extract so `dest` ends up being the package directory, flat or wrapped. */
    private void extractAs(Pack p, File dest) throws IOException {
        if (p.selfWraps()) {
            File parent = dest.getParentFile();
            if (parent != null) parent.mkdirs();
            p.extractTo(parent);
        } else {
            p.extractTo(dest);
        }
    }

    /* The meta.inf files under LF/Bulk, found once. Walking that tree per
     * asset meant walking twenty thousand files sixteen times over. */
    private List<File> metaCache;

    /** lfpkg's rule for which package a DeviceAsset belongs to. */
    private File findAssetParent(File bulk, String pid) {
        String[] bits = pid.split("-");
        if (bits.length < 3) return null;
        String key = bits[1] + "-" + bits[2].replace("DA", "00");
        if (metaCache == null) {
            metaCache = new ArrayList<File>();
            findMetas(bulk, metaCache, 0);
        }
        for (File m : metaCache) {
            try {
                if (Tools.readAll(m).contains(key)) return m.getParentFile();
            } catch (IOException e) { /* unreadable: not it */ }
        }
        return null;
    }

    private void findMetas(File dir, List<File> out, int depth) {
        if (depth > 5 || out.size() > 4000) return;
        File[] kids = dir.listFiles();
        if (kids == null) return;
        for (File f : kids) {
            if (f.isDirectory()) findMetas(f, out, depth + 1);
            else if (f.getName().equals("meta.inf")) out.add(f);
        }
    }

    /* ---- one package, whichever container it came in ---------------------- */

    /**
     * A .lfp is a ZIP and a .lf2 is a bzip2 tar, and everything above wants the
     * same three things from either: its manifest, whether it wraps itself, and
     * its contents on disk. Tar already decompresses bzip2 by magic, so the tar
     * side needs nothing special here.
     */
    private static final class Pack implements java.io.Closeable {
        private java.util.zip.ZipFile zip;
        private Tar.Archive tar;
        private String meta = "";

        static Pack open(File f) throws IOException {
            Pack p = new Pack();
            InputStream probe = new java.io.FileInputStream(f);
            int a, b;
            try { a = probe.read(); b = probe.read(); } finally { probe.close(); }

            if (a == 'P' && b == 'K') {
                p.zip = new java.util.zip.ZipFile(f);
                java.util.Enumeration<? extends java.util.zip.ZipEntry> en = p.zip.entries();
                while (en.hasMoreElements()) {
                    java.util.zip.ZipEntry e = en.nextElement();
                    if (!e.getName().endsWith("meta.inf")) continue;
                    p.meta = readAll(p.zip.getInputStream(e));
                    break;
                }
            } else {
                p.tar = Tar.index(f);
                for (Tar.Entry e : p.tar.entries)
                    if (e.isFile && e.name.endsWith("meta.inf")) { p.meta = p.tar.text(e); break; }
            }
            if (p.meta.isEmpty()) { p.close(); return null; }
            return p;
        }

        String field(String name) { return Tools.field(meta, name); }

        /** One top-level name, and no meta.inf at the top. */
        boolean selfWraps() {
            Set<String> tops = new HashSet<String>();
            boolean flatMeta = false;
            for (String n : names()) {
                if (n.equals("meta.inf")) flatMeta = true;
                int i = n.indexOf('/');
                tops.add(i < 0 ? n : n.substring(0, i));
            }
            return tops.size() == 1 && !flatMeta;
        }

        private List<String> names() {
            List<String> out = new ArrayList<String>();
            if (zip != null) {
                java.util.Enumeration<? extends java.util.zip.ZipEntry> en = zip.entries();
                while (en.hasMoreElements()) out.add(tidy(en.nextElement().getName()));
            } else {
                for (Tar.Entry e : tar.entries) out.add(tidy(e.name));
            }
            return out;
        }

        void extractTo(File dest) throws IOException {
            if (dest == null) return;
            if (!dest.isDirectory() && !dest.mkdirs())
                throw new IOException("cannot create " + dest);
            if (zip != null) {
                java.util.Enumeration<? extends java.util.zip.ZipEntry> en = zip.entries();
                while (en.hasMoreElements()) {
                    java.util.zip.ZipEntry e = en.nextElement();
                    File to = safe(dest, tidy(e.getName()));
                    if (to == null) continue;
                    if (e.isDirectory()) { to.mkdirs(); continue; }
                    File parent = to.getParentFile();
                    if (parent != null) parent.mkdirs();
                    copy(zip.getInputStream(e), to);
                }
            } else {
                for (Tar.Entry e : tar.entries) {
                    File to = safe(dest, tidy(e.name));
                    if (to == null) continue;
                    if (e.isDir) { to.mkdirs(); continue; }
                    if (!e.isFile) continue;
                    File parent = to.getParentFile();
                    if (parent != null) parent.mkdirs();
                    copy(tar.open(e), to);
                }
            }
        }

        /** Somebody else's archive: nothing may escape the destination. */
        private static File safe(File dest, String name) {
            if (name.isEmpty() || name.startsWith("../") || name.contains("/../")
                || name.startsWith("/")) return null;
            return new File(dest, name);
        }

        private static String tidy(String s) {
            while (s.startsWith("./")) s = s.substring(2);
            while (s.startsWith("/")) s = s.substring(1);
            while (s.endsWith("/")) s = s.substring(0, s.length() - 1);
            return s;
        }

        private static void copy(InputStream in, File to) throws IOException {
            try {
                OutputStream os = new java.io.FileOutputStream(to);
                try {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                } finally { os.close(); }
            } finally { in.close(); }
        }

        private static String readAll(InputStream in) throws IOException {
            try {
                java.io.ByteArrayOutputStream bo = new java.io.ByteArrayOutputStream();
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
                return new String(bo.toByteArray(), "UTF-8");
            } finally { in.close(); }
        }

        @Override public void close() {
            if (zip != null) try { zip.close(); } catch (IOException e) { /* ignore */ }
            if (tar != null) tar.close();
            zip = null; tar = null;
        }
    }

    /**
     * ProfileAccess — WITHOUT THIS THE HOME SCREEN IS NEARLY EMPTY, which is
     * the whole reason this pass exists.
     *
     * <p>The home picker (HomePickerState::GetAllIcons -> getProgramFileApps)
     * does not show every installed Application. It walks the sort file
     * /LF/Base/LpadAssets_en/Data/ProgramFileAppOrder.json and, for each
     * PackageID in it, checks which PROFILES may see the app. ProfileAccess is
     * that list, and -1,0,1,2,3 means everyone. The LFConnect downloads omit
     * it; the factory Bulk partition has it.
     *
     * <p>Measured on a headless boot by tools/probe-home.sh, varying only the
     * Camera package:
     *
     * <pre>
     *   pristine                                        3 tiles
     *   + DeviceAccess=1                                4 tiles   MISLEADING
     *   + DeviceAccess=0x00000000 (the device's value)  3 tiles
     *   + ProfileAccess=-1,0,1,2,3 alone                7 tiles   the real fix
     * </pre>
     *
     * DeviceAccess=1 was a false positive — it made tiles appear, but the
     * hardware has 0x00000000 and shows them anyway, so it was never the
     * filter. Both fields are written to match the factory exactly; only
     * ProfileAccess is load-bearing.
     *
     * <p>Scoped to the packages the sort file names, because the picker never
     * considers anything else.
     */
    private void profileAccess(File bulk, PrintStream out) throws IOException {
        File sortFile = new File(bulk.getParentFile(),
                                 "Base/LpadAssets_en/Data/ProgramFileAppOrder.json");
        if (!sortFile.isFile()) { out.println("  (no sort file; ProfileAccess skipped)"); return; }

        Set<String> wanted = new HashSet<String>();
        java.util.regex.Matcher m = java.util.regex.Pattern
                .compile("\"([A-Z0-9]+-0x[0-9A-Fa-f]+-[0-9A-Za-z]+)\"")
                .matcher(Tools.readAll(sortFile));
        while (m.find()) wanted.add(m.group(1));
        if (wanted.isEmpty()) { out.println("  (sort file names nothing)"); return; }

        int n = 0;
        File[] pkgs = new File(bulk, "ProgramFiles").listFiles();
        if (pkgs != null) for (File d : pkgs) {
            File mi = new File(d, "meta.inf");
            if (!mi.isFile()) continue;
            String text = Tools.readAll(mi);
            String pid = Tools.field(text, "PackageID");
            if (pid.isEmpty() || !wanted.contains(pid)) continue;
            if (hasLine(text, "ProfileAccess=")) continue;

            StringBuilder add = new StringBuilder();
            /* The newline matters for the same reason it does in install-game:
             * plenty of these files end without one, and appending straight to
             * the end welds two fields together and loses both. */
            if (!text.endsWith("\n") && !text.endsWith("\r")) add.append('\n');
            if (!hasLine(text, "DeviceAccess=")) add.append("DeviceAccess=0x00000000\n");
            add.append("ProfileAccess=-1,0,1,2,3\n");
            Tools.write(mi, text + add);
            out.println("  ProfileAccess -> " + d.getName());
            n++;
        }
        if (n == 0) out.println("  ProfileAccess: nothing to add");
    }

    private static boolean hasLine(String text, String prefix) {
        for (String l : text.split("\n")) if (l.startsWith(prefix)) return true;
        return false;
    }
}
