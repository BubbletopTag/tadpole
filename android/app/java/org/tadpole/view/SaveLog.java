package org.tadpole.view;

import android.os.Build;
import android.os.Environment;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.PrintStream;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Writes everything needed to diagnose a problem into one file, in a place the
 * user can actually reach.
 *
 * <p>WHY THIS EXISTS. Every other platform Tadpole runs on has a terminal: the
 * log is already in front of whoever started it. On a phone there is none, and
 * the documented way to get one is `adb logcat` — which means installing the
 * Android SDK, enabling developer options, and finding a USB cable. Asking a
 * beta tester to do all that before they can report a bug is asking them not to
 * report it. The reports that came back instead were "it doesn't work", which
 * took a day and a borrowed phone to turn into an actual cause.
 *
 * <p>WRITTEN TO Download/, NOT to the app's own directory. An app's files are
 * invisible to every file manager and to the attachment picker in every chat
 * and mail client. Download is where a share sheet looks, so the file can be
 * sent without a computer, which is the entire point.
 *
 * <p>WHAT GOES IN, and it is deliberately more than the log. Half the questions
 * asked of a report are about the device rather than the failure — what ABI,
 * what Android version, is the engine there, is the firmware installed — and
 * all of them are answerable here without a second round trip.
 *
 * <p>LOGCAT IS THIS APP'S OWN OUTPUT ONLY. Android has filtered the log by uid
 * since 4.1, so a plain `logcat -d` from inside the app returns Tadpole's lines
 * and nobody else's. Nothing here reads another app's data or the system's.
 */
final class SaveLog implements Tools.Tool {

    /** Enough to hold a whole session; a phone's buffer rarely has more. */
    private static final int LOGCAT_LINES = 4000;

    @Override
    public boolean run(String[] argv, PrintStream out) {
        File dir = downloads();
        if (dir == null) {
            out.println("cannot find the Downloads folder");
            return false;
        }
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT).format(new Date());
        File dest = new File(dir, "tadpole-log-" + stamp + ".txt");

        StringBuilder b = new StringBuilder(1 << 16);
        facts(b);
        installed(b);
        section(b, "logcat", logcat());
        File proj = Tools.proj();
        /* The guest's own crash report first: when there is one, it is the
         * answer, and it names the guest library and offset. */
        file(b, new File(proj, "crash.log"), 400);
        file(b, new File(proj, "rootless.log"), 400);
        file(b, new File(proj, "gl-warnings.log"), 200);

        try {
            FileOutputStream os = new FileOutputStream(dest);
            try { os.write(b.toString().getBytes("UTF-8")); } finally { os.close(); }
        } catch (IOException e) {
            out.println("could not write it: " + e.getMessage());
            return false;
        }

        /* SHORT LINES ON PURPOSE. The progress panel is narrow in portrait and
         * clips rather than wraps, and this is the one message in the app the
         * user has to be able to act on. */
        out.println("");
        out.println("Saved to your Downloads folder:");
        out.println("");
        out.println("  " + dest.getName());
        out.println("");
        out.println("(" + (dest.length() >> 10) + " KB)");
        out.println("Attach that file to your");
        out.println("bug report. Any file manager");
        out.println("or share sheet can find it.");
        return true;
    }

    /* ---- the parts ------------------------------------------------------- */

    private void facts(StringBuilder b) {
        b.append("=== Tadpole diagnostic log ===\n");
        b.append("when          ").append(new Date()).append('\n');
        b.append("app           ").append(Tools.version()).append('\n');
        b.append("device        ").append(Build.MANUFACTURER).append(' ')
         .append(Build.MODEL).append("  (").append(Build.DEVICE).append(")\n");
        b.append("android       ").append(Build.VERSION.RELEASE)
         .append("  (API ").append(Build.VERSION.SDK_INT).append(")\n");
        b.append("abilist       ");
        String[] abis = Build.SUPPORTED_ABIS;
        for (int i = 0; abis != null && i < abis.length; i++)
            b.append(i > 0 ? "," : "").append(abis[i]);
        b.append('\n');
        /* THE ONE THAT DECIDES WHICH PATH IS IN USE. A 32-bit process runs the
         * guest natively; a 64-bit one must go through the engine. */
        b.append("this process  ").append(is64() ? "64-bit (engine path)"
                                                 : "32-bit (native path)").append('\n');
        b.append('\n');
    }

    private static boolean is64() {
        String arch = System.getProperty("os.arch");
        return arch != null && (arch.contains("64"));
    }

    private void installed(StringBuilder b) {
        File proj = Tools.proj();
        b.append("=== what is installed ===\n");
        File engine = new File(proj, "glasspole/build/glasspole");
        b.append("engine        ").append(engine.exists()
                 ? "linked and present" : "ABSENT (32-bit build, or link failed)").append('\n');
        b.append("rootfs        ").append(state(new File(proj, "rootfs"))).append('\n');
        b.append("sysroot       ").append(state(Tools.sysroot())).append('\n');
        File pf = new File(Tools.bulk(), "ProgramFiles");
        String[] kids = pf.list();
        b.append("packages      ").append(kids == null ? "none" : kids.length + " installed").append('\n');
        b.append("didj support  ").append(new File(Tools.base(), "DidjPatches").isDirectory()
                 ? "installed" : "not installed").append('\n');
        b.append('\n');
    }

    private static String state(File f) {
        if (!f.exists()) return "missing";
        String[] kids = f.list();
        return kids == null || kids.length == 0 ? "empty" : kids.length + " entries";
    }

    /** This app's own logcat. Never another's — the log daemon filters by uid. */
    private String logcat() {
        StringBuilder b = new StringBuilder(1 << 16);
        Process p = null;
        try {
            p = new ProcessBuilder("logcat", "-d", "-v", "time",
                                   "-t", String.valueOf(LOGCAT_LINES))
                    .redirectErrorStream(true).start();
            BufferedReader r = new BufferedReader(new InputStreamReader(p.getInputStream()));
            try {
                String line;
                while ((line = r.readLine()) != null) b.append(line).append('\n');
            } finally { r.close(); }
            p.waitFor();
        } catch (Exception e) {
            b.append("(could not run logcat: ").append(e).append(")\n");
        } finally {
            if (p != null) p.destroy();
        }
        return b.toString();
    }

    /** The tail of a file, because the interesting end of a log is the end. */
    private void file(StringBuilder b, File f, int lines) {
        if (!f.isFile() || f.length() == 0) return;
        try {
            String all = Tools.readAll(f);
            String[] rows = all.split("\n");
            int from = rows.length > lines ? rows.length - lines : 0;
            StringBuilder t = new StringBuilder();
            if (from > 0) t.append("(first ").append(from).append(" lines omitted)\n");
            for (int i = from; i < rows.length; i++) t.append(rows[i]).append('\n');
            section(b, f.getName(), t.toString());
        } catch (IOException e) {
            section(b, f.getName(), "(could not read: " + e.getMessage() + ")\n");
        }
    }

    private static void section(StringBuilder b, String name, String body) {
        b.append("=== ").append(name).append(" ===\n").append(body).append('\n');
    }

    /**
     * Downloads, by the public path rather than the app's own external
     * directory: files under Android/data are as invisible to a file manager as
     * the app's private storage is, which would defeat the purpose.
     */
    private File downloads() {
        File d = null;
        try {
            d = Environment.getExternalStoragePublicDirectory(Environment.DIRECTORY_DOWNLOADS);
        } catch (Throwable ignored) { }
        if (d == null || !d.isDirectory())
            d = new File(Environment.getExternalStorageDirectory(), "Download");
        if (!d.isDirectory() && !d.mkdirs()) return null;
        return d.canWrite() ? d : null;
    }
}
