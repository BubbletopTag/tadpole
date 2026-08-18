package org.tadpole.view;

import java.io.File;
import java.io.IOException;
import java.io.PrintStream;
import java.io.RandomAccessFile;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;

/**
 * runtime/setup-sysroot.sh, in Java.
 *
 * <p>The sysroot is not the firmware. It is a link farm over the extracted
 * rootfs plus the parts of a running LeapPad2 that a rootfs image does not
 * contain: the sysfs the boot scripts read, the device nodes the guest
 * enumerates, the /proc entries Brio identifies its hardware from, and the
 * writable areas nothing creates on demand.
 *
 * <p>EVERY CONSTANT BELOW WAS MEASURED ON A REAL DEVICE and every one of them
 * is here because leaving it out broke something specific — the shell script
 * carries the full account and those notes are summarised at each step rather
 * than repeated. They are not defaults and they are not guesses: getting
 * lcd_size wrong gives a display module that sizes itself to nothing, and
 * omitting /flags/poweron gives an emulator that switches itself off after a
 * minute in a way that reads as a crash.
 *
 * <p>SYMLINKS, NOT COPIES, for everything that comes out of the rootfs: it is
 * the single source of truth and 168 duplicated libraries would drift. The
 * links are absolute, and built here they already point at this device — which
 * is the fixing-up that android/push-firmware.sh has to do afterwards when the
 * tree is prepared somewhere else.
 */
final class Sysroot {

    private final File rootfs, sysroot, proj;
    private final PrintStream out;
    private int links;

    Sysroot(File proj, File rootfs, File sysroot, PrintStream out) {
        this.proj = proj; this.rootfs = rootfs; this.sysroot = sysroot; this.out = out;
    }

    void build() throws IOException {
        out.println("  building the sysroot over " + Tools.rel(rootfs));
        skeleton();
        usrOverlay();
        oneLibdl();
        etcOverlay();
        varOverlay();
        sysfs();
        deviceNodes();
        procEntries();
        bulk();
        runtimeFiles();
        guestLibs();
        out.println("  sysroot ready: " + Tools.rel(sysroot) + " (" + links + " links)");
    }

    /* ---- 1. the skeleton --------------------------------------------------- */

    private void skeleton() throws IOException {
        mkdirs(sysroot);
        for (String d : new String[] { "bin", "boot", "Firmware", "lib",
                                       "linuxrc", "mnt", "sbin", "erootfs.md5" })
            lns(new File(rootfs, d), new File(sysroot, d));

        /* /LF must be REAL: Bulk and Cart are written to at runtime. Only the
         * link from an older layout is removed — deleting a real directory here
         * would wipe installed Bulk content. */
        File lf = new File(sysroot, "LF");
        if (isLink(lf)) lf.delete();
        mkdirs(new File(lf, "Bulk"));
        mkdirs(new File(lf, "Cart"));
        lns(new File(rootfs, "LF/Base"), new File(lf, "Base"));

        mkdirs(new File(sysroot, "dev/input"));
        for (String d : new String[] { "sys", "proc", "tmp", "flags" })
            mkdirs(new File(sysroot, d));
    }

    /**
     * 2. /usr overlay. Some binaries resolve libraries by ABSOLUTE path — e.g.
     * /usr/lib/libpthread.so.0 — even though libpthread actually lives in /lib.
     * Making /usr real, with a lib/ holding every guest .so, means an absolute
     * lookup always lands on an ARM object.
     */
    private void usrOverlay() throws IOException {
        File usr = new File(sysroot, "usr");
        if (isLink(usr)) usr.delete();
        mkdirs(usr);
        File[] kids = new File(rootfs, "usr").listFiles();
        if (kids != null) for (File d : kids)
            if (!d.getName().equals("lib")) lns(d, new File(usr, d.getName()));

        File usrLib = new File(usr, "lib");
        mkdirs(usrLib);
        for (String src : new String[] { "usr/lib", "lib" }) {
            File[] so = new File(rootfs, src).listFiles();
            if (so != null) for (File f : so) lns(f, new File(usrLib, f.getName()));
        }
    }

    /**
     * 3. ONE libdl, not two, which is what made native Brio apps crash on
     * launch. The shim impersonates libdl.so.0 and is found by
     * LD_LIBRARY_PATH — but /lib/libdl.so.0 and /usr/lib/libdl.so.0 pointed at
     * the real uClibc one, so anything resolved by absolute path loaded a
     * SECOND dl provider alongside it. Two providers of dlopen/dlsym/dlclose in
     * one link map load fine and UNLOAD fatally: switching apps calls dlclose()
     * and the loader walked the duplicated scope into a SIGSEGV inside
     * ld-uClibc's symbol lookup. Every native app died the same way.
     */
    private void oneLibdl() throws IOException {
        File shim = new File(proj, "runtime/shimlibs/libdl.so.0");
        if (!shim.exists()) {
            out.println("    (no shim libdl yet — native apps will crash until"
                      + " one is installed)");
            return;
        }
        lns(shim, new File(sysroot, "lib/libdl.so.0"));
        lns(shim, new File(sysroot, "usr/lib/libdl.so.0"));
    }

    /**
     * 4. /etc overlay: a null ALSA sink. Brio's CAudioMixer opens "plugdmix"
     * and falls back to "plughw:0,0"; neither can be driven here, and it then
     * RETRIES FOREVER, starving the UI thread so nothing renders. A null sink
     * makes the open succeed and the loop stop. This silences audio rather than
     * implementing it — real output goes through the shim to SDL.
     */
    private void etcOverlay() throws IOException {
        File etc = new File(sysroot, "etc");
        if (isLink(etc)) etc.delete();
        mkdirs(etc);
        File[] kids = new File(rootfs, "etc").listFiles();
        if (kids != null) for (File f : kids)
            if (!f.getName().equals("asound.conf")) lns(f, new File(etc, f.getName()));
        write(new File(etc, "asound.conf"),
              "# Tadpole: null audio sink. See runtime/setup-sysroot.sh for why.\n"
            + "pcm.plugdmix   { type null }\n"
            + "pcm.dmixbrio   { type null }\n"
            + "pcm.!default   { type null }\n"
            + "ctl.!default   { type hw card 0 }\n");
    }

    /**
     * 5. /var/sounds. The shipped video symlinks point at LucyAssets — a
     * DIFFERENT board — and rcS repoints them at boot from
     * /sys/devices/system/board/platform. We never run rcS, so without this
     * they dangle and VideoDaemon exits instead of serving its socket.
     */
    private void varOverlay() throws IOException {
        File var = new File(sysroot, "var");
        if (isLink(var)) var.delete();
        mkdirs(var);
        File[] kids = new File(rootfs, "var").listFiles();
        if (kids != null) for (File d : kids)
            if (!d.getName().equals("sounds")) lns(d, new File(var, d.getName()));

        File sounds = new File(var, "sounds");
        mkdirs(sounds);
        File[] src = new File(rootfs, "var/sounds").listFiles();
        if (src != null) for (File f : src) lns(f, new File(sounds, f.getName()));

        File video = new File(rootfs, "LF/Base/LpadAssets/Video");
        for (String v : new String[] { "StartupVideo.ogg", "ShutdownVideo.ogg",
                                       "TransitionVideo.ogg", "powerdown.wav" }) {
            File f = new File(video, v);
            if (f.exists()) lns(f, new File(sounds, v));
        }
    }

    /** 6. sysfs — rcS branches on platform, and libDisplay reads lcd_size. */
    private void sysfs() throws IOException {
        String b = "sys/devices/system/board/";
        write(new File(sysroot, b + "platform"),        "VALENCIA");
        write(new File(sysroot, b + "platform_family"), "LPAD");
        write(new File(sysroot, b + "system_rev"),      "0x310");
        write(new File(sysroot, b + "lcd_size"),        "480x272");   /* format is %ux%u */
        write(new File(sysroot, b + "lcd_type"),        "ILI6480G2");
        write(new File(sysroot, b + "lcd_mfg"),         "K&D-1");
        write(new File(sysroot, b + "lcd_mfg_get"),     "K&D-1");
        write(new File(sysroot, "sys/devices/platform/lf1000-dpc/xres"), "480");
        write(new File(sysroot, "sys/devices/platform/lf1000-dpc/yres"), "272");
        write(new File(sysroot, "sys/devices/platform/lf1000-gpio/board_id"), "0");
        write(new File(sysroot, "sys/devices/platform/lf2000-power/status"), "1"); /* EXTERNAL */
        write(new File(sysroot, "sys/devices/platform/lf2000-aclmtr/.keep"), "");
        write(new File(sysroot, "sys/class/graphics/fb0/rotate"), "0");

        /* Touchscreen tuning, mirroring /flags/set-ts.sh on the device. */
        String t = "sys/devices/platform/lf2000-touchscreen/";
        write(new File(sysroot, t + "max_tnt_down"),  "23");
        write(new File(sysroot, t + "min_tnt_up"),    "521");
        write(new File(sysroot, t + "max_delta_tnt"), "5");
        write(new File(sysroot, t + "tnt_mode"),      "0");
        write(new File(sysroot, t + "averaging"),     "-1");
    }

    /**
     * 7. Device nodes. These must EXIST as directory entries or the guest stops
     * enumerating after event1; the shim intercepts open() on them regardless of
     * what is in them. The required set comes from usr/bin/make_dev_nodes.sh in
     * the firmware itself.
     */
    private void deviceNodes() throws IOException {
        for (int i = 0; i <= 24; i++) write(new File(sysroot, "dev/input/event" + i), "");
        for (int i = 0; i <= 2; i++)  write(new File(sysroot, "dev/fb" + i), "");
    }

    private void procEntries() throws IOException {
        /* CAudioModule reads /proc/asound/card0/id to identify the codec. With
         * nothing here the guest read the HOST's file and found an Intel HDA
         * codec; give it the device's identity. */
        write(new File(sysroot, "proc/asound/card0/id"), "socaudiolfp100\n");

        /* CMfgData::Init parses /proc/mtd for a partition named MfgData0 and
         * then opens /dev/mtd<N>. Without it, init fails and CMfgData::Read
         * segfaults inside libc; with it the locale lookup degrades to en-us. */
        write(new File(sysroot, "proc/mtd"),
              "dev:    size   erasesize  name\n"
            + "mtd0: 0007e000 00001000 \"NOR_Boot\"\n"
            + "mtd1: 00001000 00001000 \"MfgData0\"\n"
            + "mtd2: 00001000 00001000 \"MfgData1\"\n"
            + "mtd3: 00400000 00100000 \"Reserved\"\n"
            + "mtd4: 01000000 00100000 \"Kernel\"\n"
            + "mtd5: 0a000000 00100000 \"RFS\"\n"
            + "mtd6: f4c00000 00100000 \"Bulk\"\n");

        /* Sparse: the sizes are the documented partition table, and nothing
         * should materialise RFS or Bulk. */
        sparse(new File(sysroot, "dev/mtd0"), 0x7e000);
        sparse(new File(sysroot, "dev/mtd1"), 0x1000);
        sparse(new File(sysroot, "dev/mtd2"), 0x1000);
    }

    /**
     * 8. /LF/Bulk. BaseUtils::CreateFile recurses to create missing parents but
     * only ever retries mkdir("/LF"), so a missing deep path loops about 175000
     * times until the stack blows. On hardware these already exist.
     */
    private void bulk() throws IOException {
        File bulk = new File(sysroot, "LF/Bulk");
        for (String d : new String[] { "Data/Uploads/0", "Data/Uploads/1",
                "Data/Uploads/2", "Data/Uploads/3", "Data/Downloads",
                "Data/Settings", "ProgramFiles/KeyboardWidget",
                "ProgramFiles/CameraWidget", "ProgramFiles/PhotoEditor",
                "ProgramFiles/SneakPeekWidget",
                "Downloads/PAD2-0x00210008-200000",
                "Downloads/PADS-0x1F1E0002-300000" })
            mkdirs(new File(bulk, d));

        /* Per-profile UI state. AppManager logs "CJSonFile::Load() failed" for
         * UIData.json when it is absent. ProgramFileAppOrder.json is
         * DELIBERATELY NOT seeded: the working device's per-profile copy holds
         * only the two tiles the user has ordered, not the full app list, so
         * copying the base file over it is wrong. */
        final String uipkg = "PAD2-0x1F1E0002-100000";
        for (String prof : new String[] { "0", "1", "2", "3", "All" }) {
            File d = new File(bulk, "Data/Local/" + prof + "/" + uipkg);
            mkdirs(d);
            File ui = new File(d, "UIData.json");
            if (!ui.isFile())
                write(ui, "{\"BadgeNumber\": 0,\"HasProfileBeenViewed\": true,"
                        + "\"_bookPickerWasLaunchedAtleastOnce\": true,"
                        + "\"_connectAlreadyPlayed\": false}\n");
        }
    }

    /** 9. Runtime files captured from a live booted device. Contents matter. */
    private void runtimeFiles() throws IOException {
        write(new File(sysroot, "tmp/bulk_ready"),     "1\n");   /* not empty */
        write(new File(sysroot, "tmp/splash"),         "0");
        write(new File(sysroot, "tmp/initial"),        "");
        write(new File(sysroot, "tmp/cart_brio_state"),"7, CARTRIDGE_STATE_REINSERT");
        write(new File(sysroot, "flags/volume"),       "7");
        write(new File(sysroot, "flags/lasttime"),     "1357015435");
        /* tslib's linear module computes x' = (a0 + a1*x + a2*y) / a6, and the
         * viewer already sends screen coordinates — so this is the IDENTITY
         * transform rather than the device's own ADC calibration. */
        write(new File(sysroot, "flags/pointercal"), "0 65536 0 0 0 65536 65536\n");
        write(new File(sysroot, "flags/developer"), "");
        /* STOPS THE DEVICE SWITCHING ITSELF OFF. libLightningBase reads this
         * beside /tmp/shutdown as the inactivity machinery; without it the
         * emulator powers down after about a minute of no input, which reads as
         * a crash rather than as a battery-powered toy saving its battery. */
        write(new File(sysroot, "flags/poweron"), "");
    }

    /**
     * 10. runtime/libs — where the GUEST's own libraries have to be found.
     * LD_LIBRARY_PATH ends with it, and without it the guest gets exactly one
     * clue: "AppManager: can't load library 'libVideoMPI.so'", which points at
     * a library rather than at a missing directory.
     */
    private void guestLibs() throws IOException {
        File libdir = new File(proj, "runtime/libs");
        mkdirs(libdir);
        File[] old = libdir.listFiles();
        if (old != null) for (File f : old) if (isLink(f)) f.delete();
        int n = 0;
        for (String d : new String[] { "lib", "usr/lib", "LF/Base/lib",
                                       "LF/Base/Brio/lib", "LF/Base/Flash/lib" }) {
            File[] so = new File(rootfs, d).listFiles();
            if (so == null) continue;
            for (File f : so) {
                if (!f.getName().contains(".so")) continue;
                lns(f, new File(libdir, f.getName()));
                n++;
            }
        }
        out.println("    " + n + " guest libraries linked");
    }

    /* ---- helpers ----------------------------------------------------------- */

    private void lns(File target, File link) throws IOException {
        if (!target.exists() && !isLink(target)) return;   /* note and carry on */
        mkdirs(link.getParentFile());
        Path p = link.toPath();
        try {
            Files.deleteIfExists(p);
        } catch (IOException e) {
            if (link.isDirectory()) Tools.rmTree(link); else return;
        }
        try {
            Files.createSymbolicLink(p, target.toPath().toAbsolutePath());
            links++;
        } catch (Throwable e) {
            /* A filesystem with no symlinks — external storage is one — gets a
             * copy, which is what setup-sysroot.sh already does for MSYS. */
            Tools.copyTree(target, link);
        }
    }

    private static boolean isLink(File f) {
        return Files.isSymbolicLink(f.toPath());
    }

    private void write(File f, String text) throws IOException {
        mkdirs(f.getParentFile());
        Tools.write(f, text);
    }

    private void sparse(File f, long size) throws IOException {
        mkdirs(f.getParentFile());
        RandomAccessFile raf = new RandomAccessFile(f, "rw");
        try { raf.setLength(size); } finally { raf.close(); }
    }

    private static void mkdirs(File d) throws IOException {
        if (d == null || d.isDirectory()) return;
        if (!d.mkdirs() && !d.isDirectory())
            throw new IOException("cannot create " + d);
    }
}
