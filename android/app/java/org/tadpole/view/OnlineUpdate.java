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
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * tools/online-update.py and tools/fetch-firmware.py, in Java: the system files
 * downloaded onto the tablet with no computer and no LeapPad2 involved.
 *
 * <p>The wizard's "Online System Update" button has always existed and has
 * never done anything on Android. This is what it does.
 *
 * <p>WHERE THE PACKAGES ARE, AND HOW THEY ARE FOUND. LeapFrog serve them
 * publicly and LFConnect finds them the same way this does — the device's own
 * package list names the IDs, and the CDN lays them out by ID:
 *
 * <pre>
 *   https://digitalcontent.leapfrog.com/packages/&lt;middle&gt;/&lt;PackageID&gt;.lf2
 * </pre>
 *
 * <p>THE ID LIST IS NOT DISCOVERABLE FROM THE CDN, and that is not a limitation
 * to work around — it is the rule this follows. fetch-firmware.py records why:
 * the bucket's root answers with an S3 ListBucket XML, but the host in front of
 * it ignores query strings, so `marker`, `prefix` and `max-keys` all return the
 * same first thousand keys for ever. The only reliable question is "does this
 * exact object exist", asked with HEAD. So the IDs come from
 * EnglishLeapPad2.xml — the list LFConnect itself uses, shipped as an asset —
 * and nothing here ever enumerates the bucket or probes a key it was not told
 * about.
 *
 * <p>THREE LAYOUTS, and missing the third cost the Python thirty-six packages,
 * which is to say the entire home screen. Content sits under the middle field
 * of its id; the firmware sits under the DEVICE directory as .lfp; and the
 * device's own apps and widgets sit under the device directory as .lf2. Some
 * early packages use the four-letter system name instead. All four are tried,
 * most likely first, and the first that answers wins.
 *
 * <p>ALREADY-DOWNLOADED FILES ARE KEPT. This is 124 MB onto a tablet whose
 * battery fails under load, and starting again from nothing after a reboot
 * would make the feature unusable on exactly the hardware it is for. A file
 * whose size already matches what the server reports is left alone.
 */
final class OnlineUpdate implements Tools.Tool {

    private static final String BASE = "https://digitalcontent.leapfrog.com/packages";
    /** LeapPad2's own directory on the CDN; other devices have their own. */
    private static final String DEVICE_DIR = "PADFW";
    private static final String[] EXTS = { "lf2", "lf3", "lfp" };

    @Override
    public boolean run(String[] argv, PrintStream out) throws Exception {
        File stage = argv.length > 0 && !argv[0].startsWith("--")
                   ? new File(argv[0])
                   : new File(Tools.proj(), "sources/online-update");
        File cache = new File(stage, "cache");
        if (!cache.isDirectory() && !cache.mkdirs()) {
            out.println("cannot write to " + Tools.rel(cache));
            return false;
        }

        out.println("==> Online System Update");
        out.println("    from digitalcontent.leapfrog.com");
        out.println("");

        /* UP FRONT, AND THAT IS THE POINT. "Connection refused" three minutes
         * into a download reads as a broken emulator; said before anything
         * starts it reads as no internet, which is what it is. A 403 or a 404
         * still proves the host answered, so only a transport failure counts. */
        String why = unreachable();
        if (why != null) {
            out.println("cannot reach digitalcontent.leapfrog.com — " + why);
            out.println("Nothing has been changed.");
            return false;
        }

        List<String[]> pkgs = packageList(out);
        if (pkgs.isEmpty()) {
            out.println("no package list — EnglishLeapPad2.xml is missing from the app");
            return false;
        }

        out.println("==> looking for " + pkgs.size() + " package(s)");
        int got = 0, had = 0, missing = 0, failed = 0;
        long bytes = 0;

        for (int i = 0; i < pkgs.size(); i++) {
            String pid = pkgs.get(i)[0], desc = pkgs.get(i)[1];
            if ((i % 10) == 0)
                out.println("    " + i + " of " + pkgs.size() + "...");

            String[] hit = locate(pid);
            if (hit == null) { missing++; continue; }
            String url = hit[0], ext = hit[1];
            long size = Long.parseLong(hit[2]);

            File dest = new File(cache, pid + "." + ext);
            if (dest.isFile() && dest.length() == size) { had++; continue; }

            out.println("  " + pid + "  " + (size >> 10) + " KB  " + desc);
            if (download(url, dest, size)) { got++; bytes += size; }
            else { failed++; dest.delete(); }
        }

        out.println("");
        out.println("==> " + got + " downloaded (" + (bytes >> 20) + " MB), "
                    + had + " already here, " + missing + " not on the CDN"
                    + (failed > 0 ? ", " + failed + " failed" : ""));
        if (got == 0 && had == 0) {
            out.println("nothing was downloaded; nothing has been installed.");
            return false;
        }
        if (failed > 0) {
            /* HALF A FIRMWARE IS WORSE THAN NONE. Installing from an
             * incomplete download gives a system that boots into something
             * broken, and the reason is three screens back in a log. */
            out.println("some downloads failed; nothing has been installed.");
            out.println("Run it again — what did arrive is kept and will be reused.");
            return false;
        }

        out.println("");
        out.println("==> installing");
        return new InstallFirmware().run(new String[] { stage.getAbsolutePath() }, out);
    }

    /* ---- which packages exist -------------------------------------------- */

    /**
     * The IDs, from the manifest LFConnect uses plus the bundled titles that
     * appear in no manifest at all. Both ship as assets; TadpoleActivity
     * unpacks them into the project directory on first run.
     */
    private List<String[]> packageList(PrintStream out) {
        Map<String, String> seen = new LinkedHashMap<String, String>();
        File xml = new File(Tools.proj(), "EnglishLeapPad2.xml");
        if (xml.isFile()) {
            try {
                /* Attribute order and spacing are not uniform in this
                 * hand-maintained file — one entry reads `description= "My
                 * Books"` with a space after the equals — so each attribute is
                 * matched on its own rather than the tag being matched whole. */
                Matcher m = Pattern.compile("<Package\\b([^>]*)>")
                                   .matcher(Tools.readAll(xml));
                while (m.find()) {
                    String attrs = m.group(1);
                    String id = attr(attrs, "id");
                    if (id.isEmpty() || seen.containsKey(id)) continue;
                    seen.put(id, attr(attrs, "description"));
                }
            } catch (IOException e) {
                out.println("  cannot read the package list: " + e.getMessage());
            }
        }

        File bundled = new File(Tools.proj(), "lp2-bundled.txt");
        if (bundled.isFile()) {
            try {
                for (String line : Tools.readAll(bundled).split("\n")) {
                    line = line.trim();
                    if (line.isEmpty() || line.startsWith("#")) continue;
                    int sp = line.indexOf(' ');
                    String pid = sp < 0 ? line : line.substring(0, sp);
                    String desc = sp < 0 ? "" : line.substring(sp + 1).trim();
                    if (!seen.containsKey(pid)) seen.put(pid, desc);
                    /* And each one's icon package, which is derived rather than
                     * listed: always PADS-<middle>-DA0000, whatever the game's
                     * own prefix. Without it the title installs and the home
                     * screen never shows it. */
                    String[] bits = pid.split("-");
                    if (bits.length >= 2) {
                        String da = "PADS-" + bits[1] + "-DA0000";
                        if (!seen.containsKey(da)) seen.put(da, (desc + " DA").trim());
                    }
                }
            } catch (IOException e) { /* the XML alone is still worth having */ }
        }

        List<String[]> out2 = new ArrayList<String[]>();
        for (Map.Entry<String, String> e : seen.entrySet())
            out2.add(new String[] { e.getKey(), e.getValue() });
        return out2;
    }

    private static String attr(String attrs, String name) {
        Matcher m = Pattern.compile(name + "\\s*=\\s*\"([^\"]*)\"").matcher(attrs);
        return m.find() ? m.group(1) : "";
    }

    /* ---- where one package lives ------------------------------------------ */

    /** Candidate URLs, most likely first. */
    private List<String> candidates(String pid) {
        List<String> out = new ArrayList<String>();
        String[] bits = pid.split("-");
        String mid = bits.length > 1 ? bits[1] : "";
        String sys = bits.length > 0 ? bits[0] : "";
        if (!mid.isEmpty()) for (String e : EXTS) out.add(BASE + "/" + mid + "/" + pid + "." + e);
        for (String e : new String[] { "lfp", "lf2", "lf3" })
            out.add(BASE + "/" + DEVICE_DIR + "/" + pid + "." + e);
        if (!sys.isEmpty()) for (String e : EXTS) out.add(BASE + "/" + sys + "/" + pid + "." + e);
        return out;
    }

    /** -> {url, ext, size} for whichever candidate exists, else null. */
    private String[] locate(String pid) {
        for (String u : candidates(pid)) {
            long n = head(u);
            if (n >= 0) return new String[] { u, u.substring(u.lastIndexOf('.') + 1),
                                              String.valueOf(n) };
        }
        return null;
    }

    private long head(String url) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setRequestMethod("HEAD");
            c.setConnectTimeout(15000);
            c.setReadTimeout(20000);
            int code = c.getResponseCode();
            if (code != 200) return -1;
            long n = c.getContentLengthLong();
            return n >= 0 ? n : 0;
        } catch (IOException e) {
            return -1;
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private String unreachable() {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(BASE + "/").openConnection();
            c.setRequestMethod("HEAD");
            c.setConnectTimeout(15000);
            c.setReadTimeout(20000);
            c.getResponseCode();     /* any answer at all proves the host is up */
            return null;
        } catch (IOException e) {
            /* AND IT SAYS WHICH FAILURE. Reporting "cannot reach the server"
             * for everything once sent somebody checking a network that was
             * working perfectly. */
            return e.getClass().getSimpleName() + ": " + e.getMessage();
        } finally {
            if (c != null) c.disconnect();
        }
    }

    private boolean download(String url, File dest, long expect) {
        HttpURLConnection c = null;
        try {
            c = (HttpURLConnection) new URL(url).openConnection();
            c.setConnectTimeout(15000);
            c.setReadTimeout(30000);
            if (c.getResponseCode() != 200) return false;
            InputStream in = c.getInputStream();
            try {
                OutputStream os = new FileOutputStream(dest);
                try {
                    byte[] buf = new byte[1 << 16];
                    long done = 0;
                    int n;
                    while ((n = in.read(buf)) > 0) { os.write(buf, 0, n); done += n; }
                    /* A TRUNCATED FILE IS THE DANGEROUS OUTCOME: a short .lfp
                     * is still a valid-looking ZIP right up until the member
                     * that was cut off, so it fails much later and somewhere
                     * else. Checked here, where it is still obvious. */
                    if (expect > 0 && done != expect) return false;
                } finally { os.close(); }
            } finally { in.close(); }
            return true;
        } catch (IOException e) {
            return false;
        } finally {
            if (c != null) c.disconnect();
        }
    }
}
