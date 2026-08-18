package org.tadpole.view;

import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * tools/install-game.py, in Java: install a LeapFrog game backup into /LF/Bulk.
 *
 *     install-game <game.tar> [more.tar ...]
 *     install-game --from-list list.txt
 *     install-game --fix-saves
 *     install-game --fix-meta
 *
 * <p>The shell script's comments carry the reasoning and are not repeated;
 * summarised, the four things that matter and are easy to get wrong:
 *
 * <ul>
 * <li>THREE tar shapes are real — flat (meta.inf on top), self-wrapped
 *     (&lt;NAME&gt;/meta.inf) and multi-package (several meta.inf). Installing
 *     only the top-level one silently installs nothing for two of the three.
 * <li>Destination by Type, from lfpkg: Application to Bulk/ProgramFiles/&lt;id&gt;,
 *     System to Base/&lt;id&gt;, Download|MicroDownload to Bulk/Downloads/&lt;id&gt;.
 * <li>ProfileAccess is appended to Applications that lack it, or the home
 *     picker filters the title out.
 * <li>The save area Bulk/Data/Local/&lt;profile&gt;/&lt;PackageID&gt;/ must exist
 *     before first launch: nothing creates it on demand, and its absence hangs
 *     Cooking! Recipes on the Road on a white screen with no message.
 * </ul>
 *
 * <p>ONE THING THE PYTHON DOES THAT THIS DOES NOT: hand Didj archives over to
 * install-didj. That tool is not ported, so a Didj dump is reported and skipped
 * rather than installed as something that cannot launch — which is the same
 * outcome as the Python's delegation when the conversion step is missing, said
 * out loud instead of half-done.
 */
final class InstallGame implements Tools.Tool {

    @Override
    public boolean run(String[] argv, PrintStream out) throws Exception {
        if (argv.length == 0) {
            out.println("usage: install-game <game.tar> [...]");
            return false;
        }
        if (argv[0].equals("--fix-saves")) return fixSaves(out);
        if (argv[0].equals("--fix-meta"))  return fixMeta(out);

        List<String> paths = new ArrayList<String>();
        if (argv[0].equals("--from-list")) {
            /* One path per line: twenty ticked titles, each of whose names may
             * hold spaces and brackets, do not belong on a command line. */
            if (argv.length < 2 || !new File(argv[1]).isFile()) {
                out.println("no such list: " + (argv.length > 1 ? argv[1] : ""));
                return false;
            }
            for (String l : Tools.readAll(new File(argv[1])).split("\n")) {
                String t = l.trim();
                if (t.length() > 0) paths.add(t);
            }
            if (paths.isEmpty()) { out.println("nothing listed"); return false; }
            out.println("installing " + paths.size() + " title(s)");
        } else {
            paths.addAll(Arrays.asList(argv));
        }

        int total = paths.size(), n = 0, failed = 0;
        for (String p : paths) {
            n++;
            File f = new File(p);
            if (!f.isFile()) { out.println("no such file: " + p); failed++; continue; }
            out.println("[" + n + "/" + total + "] " + f.getName() + ":");
            Tar.Archive tar = null;
            try {
                tar = Tar.index(f);
                List<Tar.Entry> metas = new ArrayList<Tar.Entry>();
                for (Tar.Entry e : tar.entries)
                    if (e.isFile && (e.name.equals("meta.inf") || e.name.endsWith("/meta.inf")))
                        metas.add(e);
                if (metas.isEmpty())
                    out.println("      no meta.inf — not a LeapFrog package");
                /* DIDJ PACKAGES GO SOMEWHERE ELSE. They arrive here because
                 * this is what "Install .tar directly" runs, and somebody
                 * holding a Didj dump has no reason to know it is a different
                 * kind of package. The Python hands them to install-didj,
                 * which is not ported — so they are reported and skipped
                 * rather than installed as something that will sit on the home
                 * screen and refuse to start. Asked the way install-didj asks
                 * it: a meta.inf whose Device is Didj. */
                boolean didj = false;
                for (Tar.Entry m : metas)
                    if (Tools.field(tar.text(m), "Device").equals("Didj")) { didj = true; break; }
                if (didj) {
                    out.println("      a Didj package — convert it on a desktop with"
                              + " tools/install-didj.py");
                    continue;
                }
                for (Tar.Entry m : metas) installOne(tar, m, out);
            } catch (Exception e) {
                out.println("  cannot read " + f.getName() + ": " + e.getMessage());
                failed++;
            } finally {
                if (tar != null) tar.close();
            }
        }
        out.println("");
        out.println("installed into " + Tools.rel(Tools.bulk()));
        return failed == 0;
    }

    /* ---- one package out of one archive ---------------------------------- */

    private void installOne(Tar.Archive tar, Tar.Entry metaEntry, PrintStream out)
            throws Exception {
        int slash = metaEntry.name.lastIndexOf('/');
        String prefix = slash < 0 ? "." : metaEntry.name.substring(0, slash);
        String meta = tar.text(metaEntry);

        String typ  = Tools.field(meta, "Type");
        String pid  = Tools.field(meta, "PackageID");
        String name = Tools.field(meta, "Name");
        if (pid.isEmpty()) return;

        File dest;
        if (typ.equals("Application")) {
            dest = new File(new File(Tools.bulk(), "ProgramFiles"), pid);
        } else if (typ.equals("System")) {
            dest = new File(Tools.base(), pid);
        } else if (typ.equals("Download") || typ.equals("MicroDownload")) {
            dest = new File(new File(Tools.bulk(), "Downloads"), pid);
        } else {
            out.println(row(typ, pid, name) + " (skipped)");
            return;
        }

        Tools.rmTree(dest);
        if (!dest.isDirectory() && !dest.mkdirs())
            throw new java.io.IOException("cannot create " + dest);
        extract(tar, prefix, dest);

        if (typ.equals("Application")) {
            addProfileAccess(new File(dest, "meta.inf"));
            for (File prof : profiles()) new File(prof, pid).mkdirs();
        }

        out.println(row(typ, pid, name));
        String dep = Tools.field(meta, "Depends");
        if (!dep.isEmpty()) out.println("      needs: " + dep);
    }

    private static String row(String typ, String pid, String name) {
        return String.format(Locale.ROOT, "  %-12s %-26s %s", typ, pid, name);
    }

    /**
     * Members under `prefix`, with the wrapper stripped and nothing escaping.
     *
     * <p>A backup is somebody else's archive: a member named ../../etc/passwd
     * is not a hypothetical, it is the reason this is not a bare extract-all.
     */
    private void extract(Tar.Archive tar, String prefix, File dest) throws Exception {
        for (Tar.Entry e : tar.entries) {
            String name = e.name;
            if (!prefix.equals(".")) {
                if (name.equals(prefix)) continue;
                if (!name.startsWith(prefix + "/")) continue;
                name = name.substring(prefix.length() + 1);
            }
            while (name.startsWith("/")) name = name.substring(1);
            if (name.isEmpty() || name.startsWith("../") || name.contains("/../")) continue;
            if (!e.isFile && !e.isDir) continue;   /* devices and links: no */

            File to = new File(dest, name);
            if (e.isDir) { to.mkdirs(); continue; }
            File parent = to.getParentFile();
            if (parent != null) parent.mkdirs();
            InputStream in = tar.open(e);
            try {
                OutputStream o = new java.io.FileOutputStream(to);
                try {
                    byte[] buf = new byte[65536];
                    int n;
                    while ((n = in.read(buf)) > 0) o.write(buf, 0, n);
                } finally { o.close(); }
            } finally { in.close(); }
        }
    }

    /** The profiles that EXIST, not an invented 0..3 — see the shell script. */
    private static File[] profiles() {
        File local = new File(new File(Tools.bulk(), "Data"), "Local");
        File[] kids = local.listFiles();
        if (kids == null) return new File[0];
        List<File> dirs = new ArrayList<File>();
        for (File k : kids) if (k.isDirectory()) dirs.add(k);
        return dirs.toArray(new File[0]);
    }

    /**
     * Append ProfileAccess when it is missing.
     *
     * <p>THE NEWLINE IS NOT OPTIONAL. Plenty of these files end without one,
     * and appending straight to the end welds two fields into
     * {@code DeviceAccess=1ProfileAccess=...}, losing both: the title installs
     * and then never appears on the home screen.
     */
    private static void addProfileAccess(File mi) throws Exception {
        boolean have = true;
        if (mi.isFile()) {
            have = false;
            for (String l : Tools.readAll(mi).split("\n"))
                if (l.startsWith("ProfileAccess=")) { have = true; break; }
        }
        if (have) return;

        String nl = "\n";
        RandomAccessFile raf = new RandomAccessFile(mi, "rw");
        try {
            long len = raf.length();
            if (len > 0) {
                raf.seek(len - 1);
                int last = raf.read();
                if (last == '\n' || last == '\r') nl = "";
            }
            raf.seek(len);
            raf.write((nl + "ProfileAccess=-1,0,1,2,3\n").getBytes("UTF-8"));
        } finally { raf.close(); }
    }

    /* ---- the two repair modes -------------------------------------------- */

    private boolean fixSaves(PrintStream out) throws Exception {
        int made = 0;
        File pf = new File(Tools.bulk(), "ProgramFiles");
        File[] kids = pf.listFiles();
        if (kids != null) {
            Arrays.sort(kids);
            for (File d : kids) {
                File mi = new File(d, "meta.inf");
                if (!mi.isFile()) continue;
                if (!Pattern.compile("^Type=\"?Application", Pattern.MULTILINE)
                        .matcher(Tools.readAll(mi)).find()) continue;
                for (File prof : profiles()) {
                    File p = new File(prof, d.getName());
                    if (!p.isDirectory() && p.mkdirs()) made++;
                }
            }
        }
        out.println("created " + made + " missing save directories under "
                + Tools.rel(new File(new File(Tools.bulk(), "Data"), "Local")));
        return true;
    }

    /**
     * Repair meta.inf files an earlier append welded together, splitting
     * {@code DeviceAccess=1ProfileAccess=-1,0,1,2,3} back apart.
     *
     * <p>ONLY THE FIELD THIS TOOL APPENDS, and only where it is not already at
     * the start of a line. Splitting on any {@code Word=} found mid-line would
     * be the more general repair and also a way to corrupt a value that
     * legitimately contains one. Idempotent: once split, the lookbehind fails.
     */
    private boolean fixMeta(PrintStream out) throws Exception {
        File pf = new File(Tools.bulk(), "ProgramFiles");
        File[] kids = pf.listFiles();
        int fixed = 0;
        if (kids != null) {
            Arrays.sort(kids);
            for (File d : kids) {
                File mi = new File(d, "meta.inf");
                if (!mi.isFile()) continue;
                String text = Tools.readAll(mi);
                String nw = text.replaceAll("(?<=[^\n])(ProfileAccess=)", "\n$1");
                if (nw.equals(text)) continue;
                if (!nw.endsWith("\n")) nw += "\n";
                Tools.write(mi, nw);
                String name = "";
                Matcher m = Pattern.compile("^Name=\"?([^\"\n]*)", Pattern.MULTILINE).matcher(nw);
                if (m.find()) name = m.group(1);
                out.println(String.format(Locale.ROOT, "  repaired %-30s %s", d.getName(), name));
                fixed++;
            }
        }
        out.println("repaired " + fixed + " meta.inf files");
        return true;
    }

}
