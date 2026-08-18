package org.tadpole.view;

import java.io.File;
import java.io.PrintStream;
import java.util.Locale;

/**
 * Which script name maps to which Java implementation, and the bits of the
 * project layout every tool needs.
 *
 * <p>KEYED ON THE SCRIPT NAME THE VIEWER ALREADY PASSES — "tools/install-game.sh"
 * and the rest — rather than on an enum of our own. The viewer names its tools
 * by path on every platform, Windows swaps the .sh for a .py in a table beside
 * spawn_script(), and this is the same swap a third time. Adding a tool here
 * needs no change in C.
 *
 * <p>An unknown name returns null and the front end reports that it could not
 * start, which is the same outcome as before this existed and is honest: a
 * button whose tool is not ported yet should say so rather than appear to work.
 */
final class Tools {
    private static String projectDir;
    private static String home;

    private Tools() {}

    static synchronized void setProjectDir(String dir) { projectDir = dir; }

    /** What the C side's HOME is set to — see TadpoleActivity. */
    static synchronized void setHome(String dir) { home = dir; }
    static synchronized String home() { return home; }

    /** The project directory: on Android, the app's own files directory. */
    static synchronized File proj() {
        return new File(projectDir == null ? "/data/local/tmp" : projectDir);
    }

    static File rootfs()  { return new File(proj(), "rootfs"); }
    static File sysroot() { return new File(proj(), "runtime/sysroot"); }
    static File bulk()    { return new File(sysroot(), "LF/Bulk"); }
    static File base()    { return new File(sysroot(), "LF/Base"); }

    /** A path as the tools print it: relative to the project when it is under it. */
    static String rel(File f) {
        String p = f.getAbsolutePath();
        String root = proj().getAbsolutePath();
        if (p.startsWith(root + "/")) return p.substring(root.length() + 1);
        return p;
    }

    static Tool forScript(String script) {
        if (script == null) return null;
        String name = new File(script).getName().toLowerCase(Locale.ROOT);
        int dot = name.lastIndexOf('.');
        if (dot > 0) name = name.substring(0, dot);   /* .sh and .py are one tool */

        /* DELIBERATELY ABSENT, and returning null here is how they say so:
         *
         *   micromods    fetches per-title packages from LeapFrog's CDN; 1100
         *                lines of Python and a lot of network etiquette.
         *                (online-update IS ported — see OnlineUpdate.)
         *   cart2tar     converts a raw cartridge dump, which is a desktop job
         *                — the dump arrives over FTP from a real device.
         *
         * The front end names them and says "not available on Android yet",
         * which is the honest state of a port in progress and better than a
         * button that appears to work. */
        if (name.equals("erase-firmware")) return new EraseFirmware();
        if (name.equals("install-game"))   return new InstallGame();
        if (name.equals("scan-games"))     return new ScanGames();
        if (name.equals("install-firmware")) return new InstallFirmware();
        if (name.equals("install-content"))  return new InstallContent();
        if (name.equals("online-update"))    return new OnlineUpdate();
        return null;
    }

    /* ---- shared helpers ------------------------------------------------- */

    /** Recursive delete. Returns false if anything survived. */
    static boolean rmTree(File f) {
        boolean ok = true;
        if (f.isDirectory()) {
            File[] kids = f.listFiles();
            if (kids != null) for (File k : kids) ok &= rmTree(k);
        }
        return f.delete() && ok;
    }

    /**
     * Move, falling back to copy-then-delete.
     *
     * <p>File.renameTo FAILS ACROSS FILESYSTEMS and says nothing about why —
     * it returns false. Everything here is inside one app data directory today,
     * but the games folder can be on external storage, and a tool that silently
     * did nothing would be much worse than one that copied.
     */
    static boolean move(File from, File to) throws java.io.IOException {
        if (from.renameTo(to)) return true;
        copyTree(from, to);
        return rmTree(from);
    }

    static void copyTree(File from, File to) throws java.io.IOException {
        if (from.isDirectory()) {
            if (!to.isDirectory() && !to.mkdirs())
                throw new java.io.IOException("cannot create " + to);
            File[] kids = from.listFiles();
            if (kids != null) for (File k : kids) copyTree(k, new File(to, k.getName()));
            return;
        }
        File parent = to.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs())
            throw new java.io.IOException("cannot create " + parent);
        java.io.InputStream in = new java.io.FileInputStream(from);
        try {
            java.io.OutputStream out = new java.io.FileOutputStream(to);
            try {
                byte[] buf = new byte[65536];
                int n;
                while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
            } finally { out.close(); }
        } finally { in.close(); }
    }

    /** Name="value" out of a meta.inf — first match, empty when absent. */
    static String field(String meta, String name) {
        java.util.regex.Matcher m = java.util.regex.Pattern
                .compile(java.util.regex.Pattern.quote(name) + "=\"([^\"]*)\"")
                .matcher(meta);
        return m.find() ? m.group(1) : "";
    }

    static String readAll(File f) throws java.io.IOException {
        byte[] buf = new byte[(int) Math.min(f.length(), 1 << 20)];
        java.io.InputStream in = new java.io.FileInputStream(f);
        try {
            int off = 0, n;
            while (off < buf.length && (n = in.read(buf, off, buf.length - off)) > 0) off += n;
            return new String(buf, 0, off, "ISO-8859-1");
        } finally { in.close(); }
    }

    static void write(File f, String text) throws java.io.IOException {
        File parent = f.getParentFile();
        if (parent != null && !parent.isDirectory() && !parent.mkdirs())
            throw new java.io.IOException("cannot create " + parent);
        java.io.OutputStream out = new java.io.FileOutputStream(f);
        try { out.write(text.getBytes("ISO-8859-1")); } finally { out.close(); }
    }

    /** What a tool is — mirrored from TadpoleTools so tools need only this file. */
    interface Tool extends TadpoleTools.Tool {}

    /** Convenience for tools that want the same "  thing" indent the .py ones use. */
    static void say(PrintStream out, String fmt, Object... args) {
        out.println(args.length == 0 ? fmt : String.format(Locale.ROOT, fmt, args));
    }
}
