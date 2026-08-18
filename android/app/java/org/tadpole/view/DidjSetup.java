package org.tadpole.view;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * tools/install-didj.py's setup half, in Java: the four buttons on the wizard's
 * "Didj support" page, which existed on Android and did nothing.
 *
 * <p>THE DIDJ IS TWO GENERATIONS OLDER THAN THE LEAPPAD2 and its games do not
 * simply run on one. Two separate pieces have to be on the device first, which
 * is why the page has two rows and this has four modes:
 *
 * <ul>
 *   <li>the COMPATIBILITY FILES — DidjAvatars, DidjMDLs and DidjPatches, which
 *       LeapFrog shipped on the Leapster Explorer and which land in LF/Base;
 *   <li>the CONTROLLER OVERLAY — the on-screen shoulder buttons, staged in
 *       runtime/didj/overlay, because Didj titles use L and R triggers that the
 *       LeapPad2's case does not have. Measured on JetPack Heroes: its App.so
 *       really does export CBaseScreen::OnLeftShoulder and OnRightShoulder.
 * </ul>
 *
 * <p>Each can be installed from a file the user already has or fetched
 * outright, which is the whole point on a tablet with no computer attached.
 *
 * <p>WHAT IS DELIBERATELY NOT HERE: converting a Didj game dump. That is the
 * other half of install-didj.py and it stays a desktop job — InstallGame
 * already says so when it meets a Device="Didj" package, and tools/install-didj.py
 * --to-tar is what produces the .tar this device can install. Porting the
 * recipe would mean a second implementation of it to drift from the first.
 *
 * <p>VALIDATED BEFORE ANYTHING IS WRITTEN, for the reason the Python records:
 * the wrong zip scatters junk through the firmware tree and the failure does
 * not surface until a game refuses to start, three screens away from the cause.
 */
final class DidjSetup implements Tools.Tool {

    /* Both URLs are the ones tools/install-didj.py uses, and its comment beside
     * them is the one that matters: the overlay was drawn for Tadpole by a
     * member of its Discord and carries no LeapFrog artwork, while DIDJ.zip is
     * LeapFrog's own Leapster Explorer data mirrored by the community guide
     * people already follow by hand. */
    private static final String DIDJ_URL =
        "https://archive.org/download/leappad3capturesetup/DIDJ.zip";
    private static final String OVERLAY_URL =
        "https://archive.org/download/control-overlay/ControlOverlay.zip";

    private static File cache()   { return new File(Tools.proj(), "sources/didj"); }
    private static File overlay() { return new File(Tools.proj(), "runtime/didj/overlay"); }

    @Override
    public boolean run(String[] argv, PrintStream out) throws Exception {
        String mode = argv.length > 0 ? argv[0] : "";
        String path = argv.length > 1 ? argv[1] : null;

        if (mode.equals("--setup")) {
            if (path == null) { out.println("install-didj: --setup needs a DIDJ.zip"); return false; }
            /* The Python takes an optional second zip here and the wizard never
             * sends one, but honouring it costs one line and someone running
             * the tool by hand will expect it. */
            boolean ok = setup(new File(path), out);
            if (ok && argv.length > 2) ok = overlay(new File(argv[2]), out);
            return ok;
        }
        if (mode.equals("--overlay")) {
            if (path == null) { out.println("install-didj: --overlay needs a ControlOverlay.zip"); return false; }
            return overlay(new File(path), out);
        }
        if (mode.equals("--fetch-compat")) {
            out.println("==> Downloading the Didj compatibility files");
            File z = download(DIDJ_URL, new File(cache(), "DIDJ.zip"), out);
            return z != null && setup(z, out);
        }
        if (mode.equals("--fetch-overlay")) {
            out.println("==> Downloading the Didj controller overlay");
            File z = download(OVERLAY_URL, new File(cache(), "ControlOverlay.zip"), out);
            return z != null && overlay(z, out);
        }

        /* A bare path is a game dump: the conversion half, which is not ported.
         * Said in the same words InstallGame uses so the two agree. */
        out.println("install-didj: converting a Didj game dump is a desktop job —");
        out.println("  run  tools/install-didj.py --to-tar <dump>  on a computer,");
        out.println("  then install the .tar it writes from the Game Library.");
        return false;
    }

    /* ---- the compatibility files ----------------------------------------- */

    private boolean setup(File zip, PrintStream out) {
        if (!zip.isFile()) { out.println("install-didj: no such file: " + zip.getName()); return false; }
        File base = Tools.base();
        if (!base.isDirectory()) {
            /* The same order the wizard puts its pages in: system files first.
             * Extracting into a tree that is not there would make three stray
             * directories and no working Didj support. */
            out.println("install-didj: no LF/Base here — install the system firmware first");
            return false;
        }

        Arc arc = null;
        try {
            arc = Arc.open(zip);
            if (!arc.has("DidjPatches/")) {
                out.println("install-didj: " + zip.getName() + " has no DidjPatches/ —"
                          + " that is not the Didj compatibility package");
                return false;
            }
            out.println("==> Installing Didj compatibility files into " + Tools.rel(base));
            int n = arc.extractInto(base, out);
            out.println("    " + n + " file(s)");
        } catch (IOException e) {
            out.println("install-didj: cannot read " + zip.getName() + ": " + e.getMessage());
            return false;
        } finally {
            if (arc != null) arc.close();
        }

        File patches = new File(base, "DidjPatches");
        int titles = 0;
        File[] kids = patches.listFiles();
        if (kids != null) for (File k : kids) if (k.isDirectory()) titles++;
        out.println("    DidjAvatars, DidjMDLs, DidjPatches (" + titles + " title patch(es))");

        if (!new File(overlay(), "GameInfo.json").isFile()) {
            out.println("");
            out.println("Compatibility files installed, but there is NO controller overlay yet.");
            out.println("Didj games need one — they use shoulder buttons this device lacks.");
        }
        return true;
    }

    /* ---- the controller overlay ------------------------------------------ */

    private boolean overlay(File zip, PrintStream out) {
        if (!zip.isFile()) { out.println("install-didj: no such file: " + zip.getName()); return false; }
        Arc arc = null;
        try {
            arc = Arc.open(zip);
            if (!arc.has("GameInfo.json")) {
                out.println("install-didj: " + zip.getName() + " has no GameInfo.json —"
                          + " that is not the controller overlay");
                return false;
            }
            File dest = overlay();
            /* REPLACED, NOT MERGED. A second overlay dropped on top of a first
             * leaves whichever files the new one happens not to have, and the
             * result is art from two different overlays on one screen. */
            Tools.rmTree(dest);
            if (!dest.isDirectory() && !dest.mkdirs()) {
                out.println("install-didj: cannot create " + Tools.rel(dest));
                return false;
            }
            int n = arc.extractInto(dest, out);
            out.println("    controller overlay staged in " + Tools.rel(dest)
                        + " (" + n + " file(s))");
            return true;
        } catch (IOException e) {
            out.println("install-didj: cannot read " + zip.getName() + ": " + e.getMessage());
            return false;
        } finally {
            if (arc != null) arc.close();
        }
    }

    /* ---- archives, by what the file is ----------------------------------- */

    /**
     * Zip or tar, chosen by magic and never by extension, because the Didj
     * dumps circulate as .zip while everything else in Tadpole's library is an
     * LFManager .tar and people rename both.
     */
    private static final class Arc {
        private ZipFile zip;                 /* one of these two is non-null */
        private Tar.Archive tar;

        static Arc open(File f) throws IOException {
            Arc a = new Arc();
            byte[] head = new byte[4];
            InputStream in = new java.io.FileInputStream(f);
            try { in.read(head); } finally { in.close(); }
            if (head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4)
                a.zip = new ZipFile(f);
            else
                a.tar = Tar.index(f);
            return a;
        }

        /** Is there a member at or under this name? */
        boolean has(String needle) {
            for (String n : names()) if (n.equals(needle) || n.startsWith(needle)
                                         || n.endsWith("/" + needle)) return true;
            return false;
        }

        private List<String> names() {
            List<String> out = new ArrayList<String>();
            if (zip != null) {
                for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); )
                    out.add(e.nextElement().getName());
            } else {
                for (Tar.Entry e : tar.entries) out.add(e.name);
            }
            return out;
        }

        /** Everything, into `dest`. Returns how many files were written. */
        int extractInto(File dest, PrintStream out) throws IOException {
            int total = zip != null ? zip.size() : tar.entries.size();
            int done = 0, files = 0, nextMark = 0;

            if (zip != null) {
                for (Enumeration<? extends ZipEntry> en = zip.entries(); en.hasMoreElements(); ) {
                    ZipEntry e = en.nextElement();
                    done++;
                    File to = safe(dest, e.getName());
                    if (to == null) continue;
                    if (e.isDirectory()) { to.mkdirs(); continue; }
                    InputStream in = zip.getInputStream(e);
                    try { spill(in, to); } finally { in.close(); }
                    files++;
                    nextMark = tick(out, done, total, nextMark);
                }
            } else {
                for (Tar.Entry e : tar.entries) {
                    done++;
                    File to = safe(dest, e.name);
                    if (to == null) continue;
                    if (e.isDir) { to.mkdirs(); continue; }
                    if (!e.isFile) continue;
                    InputStream in = tar.open(e);
                    try { spill(in, to); } finally { in.close(); }
                    files++;
                    nextMark = tick(out, done, total, nextMark);
                }
            }
            return files;
        }

        /* THREE THOUSAND SMALL FILES ONTO SLOW STORAGE. Silence for a minute
         * reads as a hang on exactly the hardware this port is for, so it says
         * where it is every tenth of the way.
         *
         * Only for archives big enough to be slow, though: the controller
         * overlay is fifteen files and counting them out one per line buries
         * the one line that matters under a screen of noise. */
        private static final int NOISY = 200;

        private static int tick(PrintStream out, int done, int total, int nextMark) {
            if (total < NOISY || done < nextMark) return nextMark;
            out.println("    " + (done * 100 / total) + "% (" + done + " of " + total + ")");
            return done + Math.max(1, total / 10);
        }

        /**
         * Where a member may be written, or null.
         *
         * <p>A dump is somebody else's archive and a member named
         * ../../etc/passwd is not hypothetical — it is why this exists instead
         * of joining the name onto the destination and opening it.
         */
        private static File safe(File dest, String name) {
            name = name.replace('\\', '/');
            while (name.startsWith("/")) name = name.substring(1);
            if (name.isEmpty() || name.startsWith("../") || name.contains("/../")
                || name.equals("..")) return null;
            return new File(dest, name);
        }

        private static void spill(InputStream in, File to) throws IOException {
            File parent = to.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs())
                throw new IOException("cannot create " + parent);
            OutputStream os = new FileOutputStream(to);
            try {
                byte[] buf = new byte[1 << 16];
                int n;
                while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
            } finally { os.close(); }
        }

        void close() {
            if (zip != null) try { zip.close(); } catch (IOException ignored) {}
            if (tar != null) tar.close();
        }
    }

    /* ---- fetching --------------------------------------------------------- */

    /** -> the file, or null. Resumes nothing: these are 11 MB and 55 KB. */
    private File download(String url, File dest, PrintStream out) {
        File dir = dest.getParentFile();
        if (dir != null && !dir.isDirectory() && !dir.mkdirs()) {
            out.println("install-didj: cannot write to " + Tools.rel(dir));
            return null;
        }
        File part = new File(dest.getAbsolutePath() + ".part");
        out.println("    fetching " + url);

        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(15000);
            c.setReadTimeout(60000);
            c.setInstanceFollowRedirects(true);   /* archive.org always redirects */
            int code = c.getResponseCode();
            if (code != 200) {
                out.println("install-didj: the server answered " + code);
                return null;
            }
            long total = c.getContentLengthLong(), got = 0, mark = 0;
            InputStream in = c.getInputStream();
            try {
                OutputStream os = new FileOutputStream(part);
                try {
                    byte[] buf = new byte[1 << 16];
                    int n;
                    while ((n = in.read(buf)) > 0) {
                        os.write(buf, 0, n);
                        got += n;
                        if (got >= mark) {
                            out.println(total > 0
                                ? String.format(Locale.ROOT, "    %d%% (%.1f of %.1f MB)",
                                                got * 100 / total, got / 1e6, total / 1e6)
                                : String.format(Locale.ROOT, "    %.1f MB", got / 1e6));
                            mark = got + 2000000;
                        }
                    }
                } finally { os.close(); }
            } finally { in.close(); }

            /* A SHORT ZIP IS THE DANGEROUS OUTCOME: it stays a valid-looking
             * archive right up to the member that was cut off, so it fails
             * later and somewhere else. Caught here, where it is still obvious. */
            if (total > 0 && got != total) {
                out.println("install-didj: the download stopped early ("
                            + got + " of " + total + " bytes)");
                part.delete();
                return null;
            }
            dest.delete();
            if (!part.renameTo(dest)) { out.println("install-didj: cannot save " + dest); return null; }
            out.println(String.format(Locale.ROOT, "    saved %s (%.1f MB)",
                                      Tools.rel(dest), dest.length() / 1e6));
            return dest;
        } catch (IOException e) {
            /* NAME THE TRANSPORT FAILURE. "Could not download" sends people
             * checking a network that is fine. */
            part.delete();
            out.println("install-didj: download failed — "
                        + e.getClass().getSimpleName() + ": " + e.getMessage());
            return null;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
