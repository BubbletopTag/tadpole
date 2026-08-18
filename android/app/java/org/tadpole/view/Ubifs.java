package org.tadpole.view;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.PrintStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.zip.Inflater;

/**
 * UBIFS, read-only, enough to unpack a firmware volume onto a filesystem.
 *
 * <p>BY SCANNING THE NODES, NOT BY WALKING THE INDEX, and that is the decision
 * this file turns on. UBIFS keeps a B-tree whose root the master node names,
 * and following it is the "proper" way to read the filesystem — it is also
 * several hundred lines of znode and branch handling, all of which exists to
 * make lookups fast. Nothing here looks anything up: it wants every file
 * exactly once. A linear pass over the logical blocks finds every node in the
 * volume for the cost of one read of each block.
 *
 * <p>WHICH MEANS STALE NODES HAVE TO BE RESOLVED, because UBIFS is
 * log-structured: rewriting a file leaves the old data node in place until
 * garbage collection gets to it, so a scan finds both. Every node carries a
 * 64-bit sequence number in its common header and the highest one wins. That
 * single rule is what makes scanning safe, and skipping it gives you a tree
 * that is mostly right and occasionally, silently, an older version of a file.
 *
 * <p>Little-endian throughout, unlike the UBI headers underneath — see Ubi.
 */
final class Ubifs {
    private static final int NODE_MAGIC = 0x06101831;
    private static final int CH_LEN     = 24;      /* common header */
    private static final int BLOCK_SIZE = 4096;    /* UBIFS_BLOCK_SIZE */
    private static final int ROOT_INO   = 1;

    /* node_type */
    private static final int INO_NODE = 0, DATA_NODE = 1, DENT_NODE = 2, SB_NODE = 6;
    /* key type, in the top three bits of the second key word */
    private static final int KEY_DATA = 1, KEY_DENT = 2;
    /* dent type */
    private static final int ITYPE_REG = 0, ITYPE_DIR = 1, ITYPE_LNK = 2;

    private static final class Inode {
        long sqnum, size;
        int mode, dataLen;
        long dataAt;                 /* absolute offset of the inode's inline data */
    }
    private static final class Dent {
        long sqnum;
        long inum;
        int type;
        String name;
    }
    private static final class Block {
        long sqnum, at;              /* absolute offset of the node */
        int nodeLen, size, compr;
    }

    private final Map<Long, Inode> inodes = new HashMap<Long, Inode>();
    /** parent inum -> name -> entry */
    private final Map<Long, Map<String, Dent>> dents = new HashMap<Long, Map<String, Dent>>();
    /** file inum -> block number -> where its data is */
    private final Map<Long, TreeMap<Integer, Block>> data = new HashMap<Long, TreeMap<Integer, Block>>();

    private final Ubi ubi;
    private final Ubi.Volume vol;

    private Ubifs(Ubi ubi, Ubi.Volume vol) { this.ubi = ubi; this.vol = vol; }

    /** The volume in this image that actually holds a filesystem. */
    static Ubifs find(Ubi ubi) throws IOException {
        byte[] head = new byte[CH_LEN + 8];
        for (Ubi.Volume v : ubi.dataVolumes()) {
            if (v.lebs.isEmpty()) continue;
            Long at = v.lebs.get(0);
            if (at == null) continue;
            ubi.readAt(at, head, 0, head.length);
            if (le32(head, 0) == NODE_MAGIC && (head[20] & 0xff) == SB_NODE)
                return new Ubifs(ubi, v);
        }
        throw new IOException("no UBIFS superblock in any volume of this image");
    }

    String volumeName() { return vol.name.isEmpty() ? ("vol" + vol.volId) : vol.name; }

    /* ---- pass one: find every node ---------------------------------------- */

    void scan(PrintStream out) throws IOException {
        byte[] leb = new byte[ubi.lebSize];
        int lebs = 0;
        for (Map.Entry<Integer, Long> e : vol.lebs.entrySet()) {
            int lnum = e.getKey();
            int got = ubi.readLeb(vol, lnum, leb);
            if (got <= 0) continue;
            lebs++;
            scanLeb(leb, got, ubi.offsetOf(vol, lnum, 0));
        }
        out.println("  read " + lebs + " logical blocks: " + inodes.size()
                    + " inodes, " + countDents() + " names, " + countBlocks() + " data blocks");
    }

    /**
     * Nodes are 8-byte aligned and never cross a logical block, so a block can
     * be walked on its own. A length that does not fit is treated as the end of
     * the written part of the block rather than as an error: the tail of a
     * block is unwritten flash, and reading 0xff as a header is normal.
     */
    private void scanLeb(byte[] b, int len, long lebAt) {
        int o = 0;
        while (o + CH_LEN <= len) {
            if (le32(b, o) != NODE_MAGIC) { o += 8; continue; }
            int nlen = (int) le32u(b, o + 16);
            int type = b[o + 20] & 0xff;
            if (nlen < CH_LEN || o + nlen > len) { o += 8; continue; }
            long sqnum = le64(b, o + 8);
            switch (type) {
            case INO_NODE:  takeInode(b, o, sqnum, lebAt); break;
            case DENT_NODE: takeDent(b, o, nlen, sqnum);   break;
            case DATA_NODE: takeData(b, o, nlen, sqnum, lebAt); break;
            default: break;
            }
            o += (nlen + 7) & ~7;
        }
    }

    private void takeInode(byte[] b, int o, long sqnum, long lebAt) {
        long inum = le32u(b, o + CH_LEN);
        Inode cur = inodes.get(inum);
        if (cur != null && cur.sqnum >= sqnum) return;
        Inode n = new Inode();
        n.sqnum   = sqnum;
        n.size    = le64(b, o + 48);
        n.mode    = (int) le32u(b, o + 104);
        n.dataLen = (int) le32u(b, o + 112);
        n.dataAt  = lebAt + o + 160;
        inodes.put(inum, n);
    }

    private void takeDent(byte[] b, int o, int nlen, long sqnum) {
        long parent = le32u(b, o + CH_LEN);
        int nameLen = (int) (le32u(b, o + 50) & 0xffff);
        if (nameLen <= 0 || o + 56 + nameLen > b.length) return;
        String name;
        try { name = new String(b, o + 56, nameLen, "UTF-8"); }
        catch (java.io.UnsupportedEncodingException e) { return; }
        if (name.equals(".") || name.equals("..")) return;

        Map<String, Dent> kids = dents.get(parent);
        if (kids == null) dents.put(parent, kids = new HashMap<String, Dent>());
        Dent cur = kids.get(name);
        if (cur != null && cur.sqnum >= sqnum) return;
        Dent d = new Dent();
        d.sqnum = sqnum;
        d.inum  = le64(b, o + 40);
        d.type  = b[o + 49] & 0xff;
        d.name  = name;
        /* inum 0 is an UNLINK, and it has to be recorded rather than skipped:
         * it is the newest word on that name and dropping it resurrects a
         * deleted file. */
        kids.put(name, d);
    }

    private void takeData(byte[] b, int o, int nlen, long sqnum, long lebAt) {
        long inum = le32u(b, o + CH_LEN);
        int keyHi = (int) le32u(b, o + CH_LEN + 4);
        if ((keyHi >>> 29) != KEY_DATA) return;
        int block = keyHi & 0x1fffffff;

        TreeMap<Integer, Block> blocks = data.get(inum);
        if (blocks == null) data.put(inum, blocks = new TreeMap<Integer, Block>());
        Block cur = blocks.get(block);
        if (cur != null && cur.sqnum >= sqnum) return;
        Block bl = new Block();
        bl.sqnum   = sqnum;
        bl.at      = lebAt + o;
        bl.nodeLen = nlen;
        bl.size    = (int) le32u(b, o + 40);
        bl.compr   = (int) (le32u(b, o + 44) & 0xffff);
        blocks.put(block, bl);
    }

    /* ---- pass two: write it out ------------------------------------------- */

    /** Extracts the tree under `dest`. Returns how many files were written. */
    int extract(File dest, PrintStream out) throws IOException {
        int[] counts = new int[2];              /* files, dirs */
        if (!dest.isDirectory() && !dest.mkdirs())
            throw new IOException("cannot create " + dest);
        walk(ROOT_INO, dest, counts, 0);
        out.println("  wrote " + counts[0] + " files and " + counts[1] + " directories");
        return counts[0];
    }

    private void walk(long inum, File dir, int[] counts, int depth) throws IOException {
        if (depth > 64) return;                  /* a loop in a corrupt image */
        Map<String, Dent> kids = dents.get(inum);
        if (kids == null) return;
        List<Dent> list = new ArrayList<Dent>(kids.values());
        for (Dent d : list) {
            if (d.inum == 0) continue;           /* unlinked */
            /* A name is a name, not a path: a crafted image must not be able to
             * write outside the directory it is being unpacked into. */
            if (d.name.indexOf('/') >= 0 || d.name.equals("..")) continue;
            File child = new File(dir, d.name);
            Inode ino = inodes.get(d.inum);
            switch (d.type) {
            case ITYPE_DIR:
                if (!child.isDirectory() && !child.mkdirs()) continue;
                counts[1]++;
                applyMode(child, ino);
                walk(d.inum, child, counts, depth + 1);
                break;
            case ITYPE_LNK:
                if (ino != null) writeSymlink(child, ino);
                break;
            case ITYPE_REG:
                if (ino != null) { writeFile(child, d.inum, ino); counts[0]++; applyMode(child, ino); }
                break;
            default:
                /* Device nodes, fifos and sockets. Nothing here can make them
                 * and nothing in the guest needs the real thing — the shim
                 * fakes every device the firmware opens. */
                break;
            }
        }
    }

    private void writeFile(File out, long inum, Inode ino) throws IOException {
        TreeMap<Integer, Block> blocks = data.get(inum);
        OutputStream os = new FileOutputStream(out);
        try {
            if (blocks == null) return;          /* a zero-length file */
            byte[] node = new byte[BLOCK_SIZE + 128];
            byte[] plain = new byte[BLOCK_SIZE];
            long written = 0;
            int next = 0;
            for (Map.Entry<Integer, Block> e : blocks.entrySet()) {
                int idx = e.getKey();
                Block b = e.getValue();
                /* HOLES ARE REAL. A sparse file simply has no data node for a
                 * block, and the bytes are zeros; writing the blocks that exist
                 * back to back would shorten the file and shift everything
                 * after the hole. */
                while (next < idx) {
                    long gap = Math.min(BLOCK_SIZE, ino.size - written);
                    if (gap <= 0) break;
                    java.util.Arrays.fill(plain, 0, (int) gap, (byte) 0);
                    os.write(plain, 0, (int) gap);
                    written += gap;
                    next++;
                }
                int payload = b.nodeLen - 48;
                if (payload < 0 || b.size < 0 || b.size > BLOCK_SIZE) continue;
                if (payload > node.length) continue;
                ubi.readAt(b.at + 48, node, 0, payload);
                int got = inflateBlock(node, payload, plain, b.size, b.compr);
                long room = ino.size - written;
                if (room <= 0) break;
                if (got > room) got = (int) room;
                os.write(plain, 0, got);
                written += got;
                next = idx + 1;
            }
            /* And a hole at the END of the file, which nothing above reaches. */
            while (written < ino.size) {
                int gap = (int) Math.min(BLOCK_SIZE, ino.size - written);
                java.util.Arrays.fill(plain, 0, gap, (byte) 0);
                os.write(plain, 0, gap);
                written += gap;
            }
        } finally { os.close(); }
    }

    private int inflateBlock(byte[] in, int inLen, byte[] out, int size, int compr)
            throws IOException {
        if (compr == 0) {                                     /* stored */
            int n = Math.min(inLen, size);
            System.arraycopy(in, 0, out, 0, n);
            return n;
        }
        if (compr == 1) {                                     /* LZO */
            try {
                return Lzo.decompress(in, 0, inLen, out, size);
            } catch (Lzo.Corrupt e) {
                throw new IOException("LZO: " + e.getMessage());
            }
        }
        if (compr == 2) {                                     /* zlib (deflate) */
            Inflater inf = new Inflater(true);
            try {
                inf.setInput(in, 0, inLen);
                int n = inf.inflate(out, 0, size);
                return n;
            } catch (java.util.zip.DataFormatException e) {
                throw new IOException("zlib: " + e.getMessage());
            } finally { inf.end(); }
        }
        throw new IOException("unknown compression type " + compr);
    }

    /**
     * A symlink's target is stored INLINE IN THE INODE, not in data nodes —
     * which is why an extractor that only walks data nodes produces a tree full
     * of empty links.
     */
    private void writeSymlink(File out, Inode ino) throws IOException {
        if (ino.dataLen <= 0 || ino.dataLen > 4096) return;
        byte[] t = new byte[ino.dataLen];
        ubi.readAt(ino.dataAt, t, 0, ino.dataLen);
        String target = new String(t, "UTF-8");
        try {
            java.nio.file.Path p = out.toPath();
            java.nio.file.Files.deleteIfExists(p);
            java.nio.file.Files.createSymbolicLink(p, new File(target).toPath());
        } catch (Throwable e) {
            /* A filesystem that will not take symlinks — external storage is
             * one — gets the target as a plain file rather than nothing, so the
             * tree stays diagnosable. */
            OutputStream os = new FileOutputStream(out);
            try { os.write(t); } finally { os.close(); }
        }
    }

    /**
     * THE EXECUTE BITS MATTER AND ARE IN THE INODE. install-firmware.py runs
     * tools/fix-perms.py afterwards precisely because ubi_reader drops them
     * unless it is run as root; the mode is right here, so it is applied here
     * and there is nothing to repair.
     */
    private void applyMode(File f, Inode ino) {
        if (ino == null) return;
        int m = ino.mode;
        f.setReadable((m & 0400) != 0, true);
        f.setWritable((m & 0200) != 0, true);
        f.setExecutable((m & 0100) != 0, true);
        if ((m & 044) != 0) f.setReadable(true, false);
        if ((m & 011) != 0) f.setExecutable(true, false);
    }

    private int countDents() {
        int n = 0;
        for (Map<String, Dent> m : dents.values()) n += m.size();
        return n;
    }
    private int countBlocks() {
        int n = 0;
        for (TreeMap<Integer, Block> m : data.values()) n += m.size();
        return n;
    }

    private static int le32(byte[] b, int o) {
        return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8)
             | ((b[o + 2] & 0xff) << 16) | ((b[o + 3] & 0xff) << 24);
    }
    private static long le32u(byte[] b, int o) { return le32(b, o) & 0xffffffffL; }
    private static long le64(byte[] b, int o) {
        return le32u(b, o) | (le32u(b, o + 4) << 32);
    }
}
