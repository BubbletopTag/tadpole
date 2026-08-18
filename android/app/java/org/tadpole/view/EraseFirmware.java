package org.tadpole.view;

import java.io.File;
import java.io.PrintStream;
import java.text.SimpleDateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.Locale;

/**
 * tools/erase-firmware.py, in Java.
 *
 * <p>Removes the installed system files, returning to a first-run state, for
 * testing the setup wizard — which is otherwise awkward to reach once an
 * install works — and for backing out of a half-done install.
 *
 * <p>IT MOVES RATHER THAN DELETES, by default, and that is the whole design of
 * the original: re-installing needs the firmware packages and a UBIFS reader,
 * and on Android it needs a desktop and push-firmware.sh, so a genuine delete
 * can leave you with nothing to run and no quick way back. A rename costs
 * nothing and is reversible. --really-delete is the opt-in.
 *
 * <p>Never touches games/ (cartridge backups), sources/ (firmware downloads),
 * or anything built.
 *
 * <p>The one deliberate difference from the Python: it does not print the
 * "re-install with tools/install-firmware.py" line, because that tool is not
 * one this device has. It names push-firmware.sh instead, which is the route
 * that exists here.
 */
final class EraseFirmware implements Tools.Tool {

    @Override
    public boolean run(String[] argv, PrintStream out) throws Exception {
        boolean hard = Arrays.asList(argv).contains("--really-delete");
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.ROOT)
                .format(new Date());
        File backup = new File(Tools.proj(), ".erased-" + stamp);
        int[] moved = { 0 };

        out.println("Erasing installed system files...");

        /* The extracted firmware. Sorted, so the log reads the same twice. */
        File rootfs = Tools.rootfs();
        if (rootfs.isDirectory()) {
            File[] kids = rootfs.listFiles();
            if (kids != null) {
                Arrays.sort(kids);
                for (File d : kids)
                    if (d.isDirectory() && !takeaway(d, hard, backup, moved, out))
                        return false;
            }
        }

        /* The sysroot is GENERATED — links into rootfs plus the writable
         * directories the guest needs. Removing it is safe; it is rebuilt. */
        if (!takeaway(Tools.sysroot(), hard, backup, moved, out))
            return false;

        if (moved[0] == 0) {
            out.println("  nothing installed — already at a first-run state");
            return true;
        }

        out.println("");
        if (hard) {
            out.println("Deleted.");
        } else {
            out.println("Moved to: " + Tools.rel(backup));
            out.println("Restore by copying its contents back over " + Tools.rel(Tools.proj()) + ".");
        }
        out.println("Re-install from a desktop with android/push-firmware.sh.");
        out.println("Tadpole will show the setup wizard on next launch.");
        return true;
    }

    /** Returns false only on a failure worth stopping for. */
    private boolean takeaway(File path, boolean hard, File backup, int[] moved,
                             PrintStream out) {
        if (!path.exists()) return true;

        if (hard) {
            Tools.rmTree(path);
            out.println("  deleted  " + Tools.rel(path));
            moved[0]++;
            return true;
        }

        /* PRESERVE THE RELATIVE PATH inside the backup, so restoring is a
         * single copy. Flattening everything into one directory loses where
         * each piece came from — rootfs/<ver> and runtime/sysroot land side by
         * side and the restore becomes a guess. */
        String relPath = Tools.rel(path);
        int slash = relPath.lastIndexOf('/');
        File dest = slash < 0 ? backup : new File(backup, relPath.substring(0, slash));
        if (!dest.isDirectory() && !dest.mkdirs()) {
            out.println("  cannot create " + Tools.rel(dest));
            return false;
        }
        try {
            if (!Tools.move(path, new File(dest, path.getName()))) {
                out.println("  cannot move " + relPath);
                out.println("  stop the guest and any running title, then retry.");
                return false;
            }
        } catch (java.io.IOException e) {
            /* A file still held open by a RUNNING GUEST is the common way this
             * fails, and on Android the guest is a root process the app cannot
             * even signal — so say which one and what to do about it, rather
             * than letting the panel show a bare exception. */
            out.println("  cannot move " + relPath + ": " + e.getMessage());
            out.println("  stop the guest and any running title, then retry.");
            return false;
        }
        out.println("  moved    " + relPath);
        moved[0]++;
        return true;
    }
}
