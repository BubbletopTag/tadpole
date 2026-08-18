package org.tadpole.view;

import android.graphics.Bitmap;
import android.graphics.BitmapFactory;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.PrintStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.Comparator;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * tools/scan-games.py, in Java: turn a folder of backups into names and icons.
 *
 *     scan-games &lt;folder&gt; [--cache DIR] [--force]
 *
 * <p>WHY. Installing a game meant picking a .tar out of a file list, and a file
 * list is the worst possible view of a library: the names are whatever the
 * person who dumped the cartridge typed, several titles differ only by a
 * version number in brackets, and nothing says which you already have. The
 * cartridge carries the name and icon the LeapPad itself shows, so this reads
 * those out and the viewer draws them.
 *
 * <p>THE OUTPUT IS READ BY C, so the formats are the Python's exactly:
 * index.tsv, e/&lt;key&gt;.rec per archive, and i/&lt;key&gt;.tpi per icon —
 * {@code 'TPI1' | u16 w | u16 h | w*h*4 bytes of RGBA}. And the cache
 * directory is resolved by walking the same environment chain
 * games_cache_dir() walks in tadpole_ui.c, because a scanner that writes an
 * index nothing reads reports success and leaves the library empty.
 *
 * <p>THE ONE PLACE THIS IS SIMPLER THAN THE PYTHON, and it is a real gain:
 * icon decoding. The Python carries its own PNG decoder because the artwork
 * arrives as 8-bit RGBA PNG, 8-bit RGB PNG and — for two Disney titles — a
 * Flash movie. Android has BitmapFactory, which decodes the PNGs natively and
 * returns null for the Flash movie, which is the same answer the Python's
 * decoder gives by failing. So there is no decoder here, and no box-average
 * scaler either: createScaledBitmap filters, which is what the box average was
 * for — icons are photographic, and dropping every other pixel makes them
 * grainy in a way that reads as a bad decode.
 */
final class ScanGames implements Tools.Tool {
    private static final int ICON_MAX = 96;      /* tiles draw at 40-64px */
    private static final int INDEX_VERSION = 1;

    /** The names an icon goes by, in the order we would rather have them. */
    private static final String[] ICON_NAMES = {
        "icon.png", "game_icon.png", "iconnormal.png", "baseicon.png",
        "baseimage.png", "popupicon.png", "previewimage.png",
        "icon64.png", "th.png"
    };

    @Override
    public boolean run(String[] argv, PrintStream out) throws Exception {
        List<String> plain = new ArrayList<String>();
        boolean force = false;
        File cache = cacheDir();
        for (int i = 0; i < argv.length; i++) {
            if (argv[i].equals("--force")) { force = true; continue; }
            if (argv[i].equals("--cache") && i + 1 < argv.length) { cache = new File(argv[++i]); continue; }
            if (!argv[i].startsWith("--")) plain.add(argv[i]);
        }
        if (plain.isEmpty()) { out.println("usage: scan-games <folder>"); return false; }
        File folder = new File(plain.get(0));
        if (!folder.isDirectory()) { out.println("not a folder: " + folder); return false; }

        new File(cache, "i").mkdirs();
        new File(cache, "e").mkdirs();

        List<File> tars = new ArrayList<File>();
        File[] kids = folder.listFiles();
        if (kids != null) for (File f : kids)
            if (f.isFile() && f.getName().toLowerCase(Locale.ROOT).endsWith(".tar")) tars.add(f);
        Collections.sort(tars);

        if (tars.isEmpty()) {
            out.println("no .tar backups in " + folder);
            /* Still write an empty index: the viewer must be able to tell
             * "scanned, found nothing" from "never scanned". */
            Tools.write(new File(cache, "index.tsv"),
                        "#tadpole-game-index " + INDEX_VERSION + "\n");
            return true;
        }

        out.println("==> " + tars.size() + " archive(s) in " + folder);
        List<String[]> rows = new ArrayList<String[]>();
        int done = 0;
        for (File f : tars) {
            done++;
            String key = keyFor(f);
            Map<String, String> rec = force ? null : loadRec(cache, key, f);
            if (rec != null) {
                rows.add(new String[] {
                    rec.get("name"), str(rec.get("pid")), str(rec.get("version")),
                    rec.containsKey("bytes") ? rec.get("bytes") : String.valueOf(f.length()),
                    "1".equals(rec.get("hasicon")) ? key : "",
                    rec.get("path") });
                continue;
            }
            out.println("  [" + done + "/" + tars.size() + "] " + f.getName());
            Rec got = scanArchive(f, out);
            if (got == null) { out.println("        no game package inside - skipped"); continue; }
            if (got.icon != null)
                writeTpi(new File(new File(cache, "i"), key + ".tpi"), got.icon);
            saveRec(cache, key, got, f);
            rows.add(new String[] { got.name, got.pid, got.version,
                                    String.valueOf(got.size),
                                    got.icon != null ? key : "", got.path });
        }

        Collections.sort(rows, new Comparator<String[]>() {
            @Override public int compare(String[] a, String[] b) {
                return a[0].toLowerCase(Locale.ROOT).compareTo(b[0].toLowerCase(Locale.ROOT));
            }
        });

        StringBuilder sb = new StringBuilder();
        sb.append("#tadpole-game-index ").append(INDEX_VERSION).append('\n');
        for (String[] r : rows) {
            for (int i = 0; i < r.length; i++) {
                if (i > 0) sb.append('\t');
                /* Names come out of a manifest and could hold a tab; the reader
                 * splits on tabs, so they are stripped here. */
                sb.append(str(r[i]).replace('\t', ' ').replace('\n', ' '));
            }
            sb.append('\n');
        }
        File tmp = new File(cache, "index.tsv.tmp");
        Tools.write(tmp, sb.toString());
        File idx = new File(cache, "index.tsv");
        idx.delete();
        if (!tmp.renameTo(idx)) { Tools.copyTree(tmp, idx); tmp.delete(); }

        out.println("==> " + rows.size() + " title(s) ready");
        return true;
    }

    private static String str(String s) { return s == null ? "" : s; }

    /* ---- one archive ------------------------------------------------------ */

    private static final class Rec {
        String name = "", pid = "", version = "", path = "";
        long size;
        Bitmap icon;
    }

    /**
     * WHICH meta.inf. A backup can hold several packages — the game, a shared
     * library pack, a microphone widget — each with its own manifest. The one
     * describing the TITLE is the shallowest Type="Application" that names an
     * Icon; the widgets name none, and library packs are Download or System.
     */
    private Rec scanArchive(File path, PrintStream out) {
        Tar.Archive tar = null;
        try {
            tar = Tar.index(path);

            /* tidied path -> the name the archive actually knows it by. Some
             * backups list everything as "./LPAD/Icon.png" and some as
             * "LPAD/Icon.png", so matching happens on the tidied form while
             * extraction must use the original. */
            Map<String, Tar.Entry> names = new LinkedHashMap<String, Tar.Entry>();
            for (Tar.Entry e : tar.entries)
                if (e.isFile) names.put(tidy(e.name), e);

            Tar.Entry bestMeta = null;
            Map<String, String> bestFld = null;
            long bestRank = Long.MAX_VALUE;
            for (Tar.Entry e : tar.entries) {
                if (!e.isFile || !base(e.name).equals("meta.inf")) continue;
                Map<String, String> fld;
                try { fld = parseMeta(tar.text(e)); } catch (Exception ex) { continue; }
                if (!"Application".equals(fld.get("Type"))) continue;
                int depth = count(e.name, '/');
                /* An Application WITHOUT an icon is a widget (the microphone
                 * one ships inside several titles); prefer anything with one. */
                long rank = (fld.get("Icon") != null && !fld.get("Icon").isEmpty() ? 0L : 1L) * 100000
                          + depth;
                if (rank < bestRank) { bestRank = rank; bestMeta = e; bestFld = fld; }
            }
            if (bestMeta == null) return null;

            String metadir = tidy(dirOf(bestMeta.name));
            Rec rec = new Rec();
            String nm = bestFld.get("Name");
            rec.name = (nm == null || nm.isEmpty()) ? stripExt(path.getName()) : nm;
            rec.pid = str(bestFld.get("PackageID"));
            rec.version = str(bestFld.get("Version"));
            rec.path = path.getAbsolutePath();
            rec.size = path.length();

            Tar.Entry member = pickIcon(names, metadir, bestFld.get("Icon"));
            if (member != null) {
                try {
                    byte[] raw = readAll(tar.open(member), member.size);
                    Bitmap bm = BitmapFactory.decodeByteArray(raw, 0, raw.length);
                    if (bm != null) rec.icon = fit(bm, ICON_MAX);
                } catch (Throwable t) {
                    /* An unreadable icon costs the icon, not the title. */
                }
            }
            return rec;
        } catch (Exception e) {
            out.println("  ! " + path.getName() + ": " + e.getMessage());
            return null;
        } finally {
            if (tar != null) tar.close();
        }
    }

    /**
     * Where the icon really is. meta.inf names it relative to its own
     * directory; when that entry is missing or is a Flash movie, look for the
     * artwork beside it — the Flash-era titles name a .swf and ship the
     * picture as PopUpIcon.png or BaseIcon.png, so "the manifest says swf" has
     * to mean "look next to it", not "no icon".
     */
    private Tar.Entry pickIcon(Map<String, Tar.Entry> names, String metadir, String icon) {
        if (icon != null && !icon.isEmpty()) {
            Tar.Entry hit = find(names, join(metadir, icon));
            if (hit != null) return hit;
            /* Same name, PNG extension: BaseIcon.swf -> BaseIcon.png, which is
             * exactly how Letter Factory ships it. */
            hit = find(names, join(metadir, stripExt(icon) + ".png"));
            if (hit != null) return hit;
        }

        /* Then, in order: a file named like an icon, a file whose name merely
         * CONTAINS "icon", and finally a top-level preview. Depth-limited on
         * purpose — several titles carry a coloring book full of
         * preview_page_NN.png, and a random page of content dressed up as the
         * game's icon is worse than no icon at all. */
        Tar.Entry best = null;
        String bestKey = null;
        for (Map.Entry<String, Tar.Entry> en : names.entrySet()) {
            String t = en.getKey(), b = base(t).toLowerCase(Locale.ROOT);
            if (!b.endsWith(".png")) continue;
            int pri0, pri1;
            int named = Arrays.asList(ICON_NAMES).indexOf(b);
            if (named >= 0)                { pri0 = 0; pri1 = named; }
            else if (b.contains("icon"))   { pri0 = 1; pri1 = 0; }
            else if ((b.equals("preview.png") || b.equals("previewimage.png"))
                     && count(t, '/') <= 1) { pri0 = 2; pri1 = 0; }
            else continue;
            int near = (metadir.length() > 0
                        && t.toLowerCase(Locale.ROOT).startsWith(metadir.toLowerCase(Locale.ROOT)))
                       ? 0 : 1;
            String rank = String.format(Locale.ROOT, "%d%d%03d%03d%s",
                                        pri0, near, pri1, count(t, '/'),
                                        t.toLowerCase(Locale.ROOT));
            if (bestKey == null || rank.compareTo(bestKey) < 0) { bestKey = rank; best = en.getValue(); }
        }
        return best;
    }

    private static Tar.Entry find(Map<String, Tar.Entry> names, String cand) {
        String c = tidy(cand);
        if (!c.toLowerCase(Locale.ROOT).endsWith(".png")) return null;
        Tar.Entry e = names.get(c);
        return e != null ? e : names.get(c.toLowerCase(Locale.ROOT));
    }

    /* ---- icons ------------------------------------------------------------ */

    private static Bitmap fit(Bitmap bm, int maxdim) {
        int w = bm.getWidth(), h = bm.getHeight();
        if (w <= maxdim && h <= maxdim) return bm;
        double scale = Math.max(w / (double) maxdim, h / (double) maxdim);
        int nw = Math.max(1, (int) (w / scale)), nh = Math.max(1, (int) (h / scale));
        return Bitmap.createScaledBitmap(bm, nw, nh, true);
    }

    /** 'TPI1' | u16 w | u16 h | w*h*4 bytes of R,G,B,A — the C reader's order. */
    private static void writeTpi(File path, Bitmap bm) throws Exception {
        int w = bm.getWidth(), h = bm.getHeight();
        int[] argb = new int[w * h];
        bm.getPixels(argb, 0, w, 0, 0, w, h);
        byte[] px = new byte[w * h * 4];
        for (int i = 0, o = 0; i < argb.length; i++) {
            int p = argb[i];
            px[o++] = (byte) ((p >> 16) & 0xff);   /* R */
            px[o++] = (byte) ((p >> 8) & 0xff);    /* G */
            px[o++] = (byte) (p & 0xff);           /* B */
            px[o++] = (byte) ((p >>> 24) & 0xff);  /* A */
        }
        File parent = path.getParentFile();
        if (parent != null) parent.mkdirs();
        OutputStream out = new java.io.FileOutputStream(path);
        try {
            out.write(new byte[] { 'T', 'P', 'I', '1' });
            out.write(w & 0xff); out.write((w >> 8) & 0xff);
            out.write(h & 0xff); out.write((h >> 8) & 0xff);
            out.write(px);
        } finally { out.close(); }
    }

    /* ---- cache ------------------------------------------------------------ */

    /**
     * The SAME directory the viewer reads. games_cache_dir() in tadpole_ui.c
     * walks XDG_CACHE_HOME, then Windows' LOCALAPPDATA, then ~/.cache, and this
     * has to walk it identically. On Android HOME is the app's own data
     * directory, so the last branch lands somewhere writable without anything
     * being set specially — measured, HOME=/data/user/0/&lt;pkg&gt;.
     */
    static File cacheDir() {
        String x = System.getenv("XDG_CACHE_HOME");
        if (x != null && !x.isEmpty()) return new File(x, "tadpole/games");
        String la = System.getenv("LOCALAPPDATA");
        if (la != null && !la.isEmpty()) return new File(la, "Tadpole/cache/games");
        /* NOT System.getenv("HOME"): the JVM snapshots its environment at
         * process start and an Android app has no HOME in it, so that returns
         * null and the chain falls through to /tmp — which this platform does
         * not have. The activity sets HOME natively for the C side and hands
         * the same value here, so both ends agree by construction. */
        String home = Tools.home();
        if (home == null || home.isEmpty()) home = System.getenv("HOME");
        if (home == null || home.isEmpty()) home = System.getenv("USERPROFILE");
        if (home == null || home.isEmpty()) home = "/tmp";
        return new File(home, ".cache/tadpole/games");
    }

    private static String keyFor(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-1");
        byte[] d = md.digest(f.getAbsolutePath().getBytes("UTF-8"));
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < 8; i++) sb.append(String.format(Locale.ROOT, "%02x", d[i]));
        return sb.toString();
    }

    private static Map<String, String> loadRec(File cache, String key, File f) {
        try {
            File p = new File(new File(cache, "e"), key + ".rec");
            if (!p.isFile()) return null;
            Map<String, String> d = new HashMap<String, String>();
            for (String l : Tools.readAll(p).split("\n")) {
                int t = l.indexOf('\t');
                if (t > 0) d.put(l.substring(0, t), l.substring(t + 1));
            }
            if (!String.valueOf(f.lastModified() / 1000).equals(d.get("mtime"))) return null;
            if (!String.valueOf(f.length()).equals(d.get("size"))) return null;
            return d;
        } catch (Exception e) { return null; }
    }

    private static void saveRec(File cache, String key, Rec rec, File f) throws Exception {
        StringBuilder sb = new StringBuilder();
        sb.append("name\t").append(rec.name).append('\n');
        sb.append("pid\t").append(rec.pid).append('\n');
        sb.append("version\t").append(rec.version).append('\n');
        sb.append("path\t").append(rec.path).append('\n');
        sb.append("size\t").append(f.length()).append('\n');
        sb.append("mtime\t").append(f.lastModified() / 1000).append('\n');
        sb.append("bytes\t").append(rec.size).append('\n');
        sb.append("hasicon\t").append(rec.icon != null ? 1 : 0).append('\n');
        Tools.write(new File(new File(cache, "e"), key + ".rec"), sb.toString());
    }

    /* ---- small helpers ---------------------------------------------------- */

    /** Every Name="value" in a manifest. */
    private static Map<String, String> parseMeta(String text) {
        Map<String, String> m = new HashMap<String, String>();
        Matcher mt = Pattern.compile("(\\w+)=\"([^\"]*)\"").matcher(text);
        while (mt.find()) if (!m.containsKey(mt.group(1))) m.put(mt.group(1), mt.group(2));
        return m;
    }

    private static byte[] readAll(InputStream in, long size) throws Exception {
        try {
            ByteArrayOutputStream bo = new ByteArrayOutputStream((int) Math.min(size, 1 << 20));
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            return bo.toByteArray();
        } finally { in.close(); }
    }

    private static String tidy(String s) {
        while (s.startsWith("./")) s = s.substring(2);
        while (s.startsWith("/")) s = s.substring(1);
        return s;
    }
    private static String base(String s) {
        int i = s.lastIndexOf('/');
        return i < 0 ? s : s.substring(i + 1);
    }
    private static String dirOf(String s) {
        int i = s.lastIndexOf('/');
        return i < 0 ? "" : s.substring(0, i);
    }
    private static String stripExt(String s) {
        int i = s.lastIndexOf('.');
        return i <= 0 ? s : s.substring(0, i);
    }
    private static String join(String dir, String name) {
        return dir.isEmpty() ? name : dir + "/" + name;
    }
    private static int count(String s, char c) {
        int n = 0;
        for (int i = 0; i < s.length(); i++) if (s.charAt(i) == c) n++;
        return n;
    }
}
