package org.tadpole.view;

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;

/**
 * The UBI layer: physical erase blocks in, one volume's logical blocks out.
 *
 * <p>A .ubi image is a flat run of physical erase blocks. Each one opens with
 * an erase-counter header ("UBI#"), carries a volume-id header ("UBI!") at
 * {@code vid_hdr_offset}, and its payload begins at {@code data_offset} —
 * both of which the EC header states, so nothing here is assumed from the file
 * name even though LeapFrog helpfully puts the geometry in it
 * ("C4G-E1M-W4K-erootfs.ubi" is a 1 MiB erase block and a 4 KiB page).
 *
 * <p>THE HEADERS ARE BIG-ENDIAN and everything inside UBIFS is little-endian,
 * which is the single easiest thing to get wrong here. UBI came from the MTD
 * world and kept network byte order; the filesystem on top of it did not.
 *
 * <p>PHYSICAL ORDER IS NOT LOGICAL ORDER. Each VID header says which logical
 * block its physical block is holding, and wear levelling means the two
 * disagree — on the stock 4.6.0.784 image the layout volume takes the first
 * two blocks and the 49 blocks of ubi_rfs follow in an order that is nearly
 * but not exactly sequential. Reading the file straight through gives a
 * filesystem with its blocks shuffled, which fails in a way that looks like
 * corruption rather than like a missing sort.
 */
final class Ubi implements java.io.Closeable {

    /** A volume: its name and where each of its logical blocks physically is. */
    static final class Volume {
        final int volId;
        String name = "";
        /** lnum -> absolute file offset of that block's DATA. */
        final TreeMap<Integer, Long> lebs = new TreeMap<Integer, Long>();
        Volume(int id) { this.volId = id; }
    }

    /** UBI's own bookkeeping volume, which is not a filesystem. */
    private static final int LAYOUT_VOLUME_ID = 0x7fffefff;

    private final RandomAccessFile raf;
    final int pebSize, vidHdrOffset, dataOffset, lebSize;
    final Map<Integer, Volume> volumes = new HashMap<Integer, Volume>();

    private Ubi(RandomAccessFile raf, int peb, int vid, int data) {
        this.raf = raf;
        this.pebSize = peb;
        this.vidHdrOffset = vid;
        this.dataOffset = data;
        this.lebSize = peb - data;
    }

    static Ubi open(File image) throws IOException {
        RandomAccessFile raf = new RandomAccessFile(image, "r");
        try {
            byte[] ec = new byte[64];
            raf.seek(0);
            raf.readFully(ec);
            if (be32(ec, 0) != 0x55424923L)          /* "UBI#" */
                throw new IOException("not a UBI image (no UBI# at offset 0)");
            int vidOff  = (int) be32(ec, 16);
            int dataOff = (int) be32(ec, 20);
            if (vidOff <= 0 || dataOff <= vidOff)
                throw new IOException("nonsense UBI header offsets");

            /* THE ERASE BLOCK SIZE IS NOT IN THE HEADER, so it is found rather
             * than read: walk candidate sizes and take the first at which every
             * block starts with UBI#. Powers of two from the data offset up,
             * which covers every real device — 128 KiB parts through the 1 MiB
             * this one uses — and is a handful of seeks. */
            int peb = -1;
            for (int cand = 65536; cand <= 4 * 1024 * 1024; cand <<= 1) {
                if (cand <= dataOff) continue;
                if (raf.length() % cand != 0) continue;
                if (looksLikePeb(raf, cand)) { peb = cand; break; }
            }
            if (peb < 0) throw new IOException("could not work out the erase block size");

            Ubi u = new Ubi(raf, peb, vidOff, dataOff);
            u.scan();
            return u;
        } catch (IOException e) {
            raf.close();
            throw e;
        }
    }

    /** Every block at this stride opens with UBI#, checked over the whole file. */
    private static boolean looksLikePeb(RandomAccessFile raf, int cand) throws IOException {
        long n = raf.length() / cand;
        if (n < 2) return false;
        byte[] m = new byte[4];
        for (long i = 0; i < n; i++) {
            raf.seek(i * cand);
            raf.readFully(m);
            /* An erased-but-unused block is legal and is all 0xff. */
            if (be32(m, 0) == 0x55424923L) continue;
            if ((m[0] & 0xff) == 0xff && (m[1] & 0xff) == 0xff) continue;
            return false;
        }
        return true;
    }

    private void scan() throws IOException {
        long n = raf.length() / pebSize;
        byte[] vid = new byte[64];
        byte[] ec = new byte[4];
        for (long p = 0; p < n; p++) {
            raf.seek(p * pebSize);
            raf.readFully(ec);
            if (be32(ec, 0) != 0x55424923L) continue;       /* unused block */
            raf.seek(p * pebSize + vidHdrOffset);
            raf.readFully(vid);
            if (be32(vid, 0) != 0x55424921L) continue;      /* "UBI!" — free */
            int volId = (int) be32(vid, 8);
            int lnum  = (int) be32(vid, 12);
            Volume v = volumes.get(volId);
            if (v == null) volumes.put(volId, v = new Volume(volId));
            /* Later copies win: UBI leaves the old block in place until the new
             * one is written, and the VID header's sqnum orders them. Physical
             * order is a good enough proxy here because this is a freshly built
             * install image, but the sqnum is the actual rule. */
            v.lebs.put(lnum, p * pebSize + dataOffset);
        }
        readVolumeNames();
    }

    /**
     * The layout volume holds the volume table: 128-byte records, one per
     * volume id, giving each its name. Only used to report what was found —
     * the extraction picks the volume by having a UBIFS superblock in it.
     */
    private void readVolumeNames() throws IOException {
        Volume layout = volumes.get(LAYOUT_VOLUME_ID);
        if (layout == null || layout.lebs.isEmpty()) return;
        long at = layout.lebs.values().iterator().next();
        byte[] rec = new byte[128];
        for (int id = 0; id < 128; id++) {
            raf.seek(at + (long) id * 128);
            try { raf.readFully(rec); } catch (IOException e) { return; }
            int nameLen = ((rec[14] & 0xff) << 8) | (rec[15] & 0xff);
            if (nameLen <= 0 || nameLen > 127) continue;
            Volume v = volumes.get(id);
            if (v != null) v.name = new String(rec, 16, nameLen, "UTF-8");
        }
    }

    /** The volumes that are real filesystems, not UBI's own bookkeeping. */
    List<Volume> dataVolumes() {
        List<Volume> out = new ArrayList<Volume>();
        for (Volume v : volumes.values())
            if (v.volId != LAYOUT_VOLUME_ID) out.add(v);
        return out;
    }

    /** Read one logical block of a volume. Returns how many bytes were read. */
    int readLeb(Volume v, int lnum, byte[] buf) throws IOException {
        Long at = v.lebs.get(lnum);
        if (at == null) return 0;
        int want = Math.min(lebSize, buf.length);
        raf.seek(at);
        int got = 0;
        while (got < want) {
            int r = raf.read(buf, got, want - got);
            if (r <= 0) break;
            got += r;
        }
        return got;
    }

    /** Absolute file offset of a byte inside a volume's logical block. */
    long offsetOf(Volume v, int lnum, int within) {
        Long at = v.lebs.get(lnum);
        return at == null ? -1 : at + within;
    }

    void readAt(long offset, byte[] buf, int off, int len) throws IOException {
        raf.seek(offset);
        raf.readFully(buf, off, len);
    }

    @Override public void close() throws IOException { raf.close(); }

    private static long be32(byte[] b, int o) {
        return ((long) (b[o] & 0xff) << 24) | ((b[o + 1] & 0xff) << 16)
             | ((b[o + 2] & 0xff) << 8) | (b[o + 3] & 0xff);
    }
}
