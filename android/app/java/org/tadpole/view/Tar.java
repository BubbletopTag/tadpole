package org.tadpole.view;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.GZIPInputStream;

/**
 * Just enough tar to read a LeapFrog game backup.
 *
 * <p>Java has no tar in its standard library, and the Android platform does not
 * bundle commons-compress. This is the smallest reader that handles what the
 * backups actually contain — Python's tarfile does all of it and more, and this
 * exists only because there is no tarfile here.
 *
 * <p>AN INDEX, NOT A STREAM, because the tools need two passes over the same
 * archive: install-game finds every meta.inf first (there can be several — a
 * multi-package backup is one of the three real shapes) and only then extracts
 * the members under each one's directory. Reading twice from a stream means
 * decompressing twice; recording each entry's offset once means the second pass
 * is a seek.
 *
 * <p>COMPRESSED ARCHIVES ARE DECOMPRESSED TO A TEMPORARY FILE FIRST rather
 * than indexed in place, because an offset into a gzip or bzip2 stream is not
 * something you can seek to. Python's "r:*" auto-detects and hides this. Both
 * are recognised by magic rather than by extension, which matters here: a
 * LeapFrog .lf2 content pack is a bzip2 tar and says nothing of the sort in its
 * name.
 */
final class Tar {
    static final int BLOCK = 512;

    /** One member: where it is, how big, and what kind. */
    static final class Entry {
        final String name;
        final long size, offset;      /* offset of the DATA, not the header */
        final boolean isFile, isDir;
        Entry(String name, long size, long offset, boolean isFile, boolean isDir) {
            this.name = name; this.size = size; this.offset = offset;
            this.isFile = isFile; this.isDir = isDir;
        }
    }

    /** An indexed archive. Close it when done — it may own a temporary file. */
    static final class Archive implements java.io.Closeable {
        final File file;                    /* the plain-tar file, possibly temp */
        private final File temp;            /* non-null when we decompressed */
        final List<Entry> entries;
        private RandomAccessFile raf;

        Archive(File file, File temp, List<Entry> entries) {
            this.file = file; this.temp = temp; this.entries = entries;
        }

        /** The bytes of one member. Caller closes. */
        InputStream open(Entry e) throws IOException {
            if (raf == null) raf = new RandomAccessFile(file, "r");
            raf.seek(e.offset);
            return new Bounded(raf, e.size);
        }

        /** The whole of one member as text, for meta.inf. */
        String text(Entry e) throws IOException {
            InputStream in = open(e);
            try {
                byte[] buf = new byte[(int) Math.min(e.size, 1 << 20)];
                int off = 0, n;
                while (off < buf.length && (n = in.read(buf, off, buf.length - off)) > 0)
                    off += n;
                return new String(buf, 0, off, "UTF-8");
            } finally { in.close(); }
        }

        @Override public void close() {
            if (raf != null) try { raf.close(); } catch (IOException ignored) {}
            raf = null;
            if (temp != null) temp.delete();
        }
    }

    private Tar() {}

    /**
     * Read every header and record where each member's data begins.
     *
     * <p>Stops at the first all-zero block, which is how tar marks the end;
     * archives are routinely padded well past it and reading that padding as
     * headers produces nonsense entries.
     */
    static Archive index(File path) throws IOException {
        File temp = null;
        File plain = path;

        int kind = magic(path);
        if (kind != 0) {
            temp = File.createTempFile("tadpole-tar", ".tar", path.getParentFile());
            OutputStream out = new java.io.BufferedOutputStream(
                    new FileOutputStream(temp), 1 << 16);
            try {
                if (kind == 1) {
                    InputStream in = new GZIPInputStream(new FileInputStream(path), 65536);
                    try {
                        byte[] buf = new byte[65536];
                        int n;
                        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
                    } finally { in.close(); }
                } else {
                    InputStream in = new java.io.BufferedInputStream(
                            new FileInputStream(path), 1 << 16);
                    try { Bzip2.decompress(in, out); } finally { in.close(); }
                }
            } finally { out.close(); }
            plain = temp;
        }

        List<Entry> out = new ArrayList<Entry>();
        RandomAccessFile raf = new RandomAccessFile(plain, "r");
        try {
            byte[] h = new byte[BLOCK];
            long pos = 0;
            String longName = null;
            while (true) {
                raf.seek(pos);
                int got = raf.read(h);
                if (got < BLOCK) break;
                if (isZero(h)) break;

                long size = octal(h, 124, 12);
                char type = (char) (h[156] & 0xff);
                String name = longName != null ? longName : ustarName(h);
                longName = null;
                long data = pos + BLOCK;
                long step = data + ((size + BLOCK - 1) / BLOCK) * BLOCK;

                if (type == 'L') {
                    /* GNU long name: this entry's DATA is the name of the next
                     * one. Not exotic — a LeapFrog package id plus a deep asset
                     * path passes 100 characters easily, and treating the
                     * header as a member gives you a file called "././@LongLink". */
                    byte[] nb = new byte[(int) Math.min(size, 4096)];
                    raf.seek(data);
                    raf.readFully(nb);
                    int z = 0;
                    while (z < nb.length && nb[z] != 0) z++;
                    longName = new String(nb, 0, z, "UTF-8");
                    pos = step;
                    continue;
                }
                if (type == 'x' || type == 'g' || type == 'K') {
                    pos = step;                       /* pax/long-link headers */
                    continue;
                }

                boolean isDir  = type == '5' || name.endsWith("/");
                boolean isFile = type == '0' || type == 0;
                out.add(new Entry(strip(name), size, data, isFile && !isDir, isDir));
                pos = step;
            }
        } finally { raf.close(); }

        return new Archive(plain, temp, out);
    }

    /** 0 = plain tar, 1 = gzip, 2 = bzip2. By magic, never by extension. */
    private static int magic(File f) throws IOException {
        InputStream in = new FileInputStream(f);
        try {
            int a = in.read(), b = in.read(), c = in.read();
            if (a == 0x1f && b == 0x8b) return 1;
            if (a == 'B' && b == 'Z' && c == 'h') return 2;
            return 0;
        } finally { in.close(); }
    }

    private static boolean isZero(byte[] b) {
        for (byte x : b) if (x != 0) return false;
        return true;
    }

    /** name, plus the ustar prefix field when the path was too long for it. */
    private static String ustarName(byte[] h) throws IOException {
        String name = str(h, 0, 100);
        if (h[257] == 'u' && h[258] == 's' && h[259] == 't' && h[260] == 'a' && h[261] == 'r') {
            String prefix = str(h, 345, 155);
            if (prefix.length() > 0) name = prefix + "/" + name;
        }
        return name;
    }

    private static String str(byte[] b, int off, int len) throws IOException {
        int n = 0;
        while (n < len && b[off + n] != 0) n++;
        return new String(b, off, n, "UTF-8");
    }

    private static String strip(String n) {
        while (n.endsWith("/")) n = n.substring(0, n.length() - 1);
        return n;
    }

    /**
     * Octal, with the GNU base-256 escape handled.
     *
     * <p>A leading 0x80 means the field is a big-endian binary number instead —
     * used for sizes that do not fit eleven octal digits. Not expected in a
     * game backup, but misreading it as octal yields a wildly wrong size and
     * every subsequent header lands in the middle of a file, so the whole
     * archive reads as garbage rather than failing.
     */
    private static long octal(byte[] b, int off, int len) {
        if ((b[off] & 0x80) != 0) {
            long v = 0;
            for (int i = off + 1; i < off + len; i++) v = (v << 8) | (b[i] & 0xff);
            return v;
        }
        long v = 0;
        for (int i = off; i < off + len; i++) {
            int c = b[i] & 0xff;
            if (c == 0 || c == ' ') { if (v != 0) break; else continue; }
            if (c < '0' || c > '7') break;
            v = v * 8 + (c - '0');
        }
        return v;
    }

    /** A view of `len` bytes from the current position of a shared file. */
    private static final class Bounded extends InputStream {
        private final RandomAccessFile raf;
        private long left;
        Bounded(RandomAccessFile raf, long len) { this.raf = raf; this.left = len; }
        @Override public int read() throws IOException {
            if (left <= 0) return -1;
            int c = raf.read();
            if (c >= 0) left--;
            return c;
        }
        @Override public int read(byte[] b, int off, int len) throws IOException {
            if (left <= 0) return -1;
            int n = raf.read(b, off, (int) Math.min(len, left));
            if (n > 0) left -= n;
            return n;
        }
        @Override public void close() { /* the RandomAccessFile is the Archive's */ }
    }
}
