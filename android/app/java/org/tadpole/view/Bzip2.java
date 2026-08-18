package org.tadpole.view;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;

/**
 * bzip2 decompression, because LeapFrog's .lf2 content packs are bzip2 tars
 * despite the extension and Java has no bzip2 in its standard library.
 *
 * <p>WHAT IT UNBLOCKS. The base firmware image alone does not boot: AppManager
 * dies in libLightningBase with a null dereference, having said only
 * "CMfgData::Init: GetNorPartitionFilename failed", and what it is actually
 * missing is the widgets under LF/Bulk/ProgramFiles — CameraWidget,
 * KeyboardWidget, PhotoEditor, SneakPeekWidget, VideoWidget. Those ship in .lf2
 * packs. So this is the difference between a firmware that extracts and a
 * firmware that starts.
 *
 * <p>Written from the format description rather than adapted from libbzip2, and
 * then checked against Python's bz2 over every .lf2 in a real download cache —
 * see the test in the commit. The structure follows the reference decoder's
 * stages in order, because that is what makes it checkable: bit reader, symbol
 * map, Huffman tables, MTF/RLE2, inverse Burrows-Wheeler, RLE1.
 *
 * <p>RANDOMISED BLOCKS ARE REFUSED. The bit is in the format and has been
 * deprecated since 0.9.5 — nothing has produced one this century — and
 * pretending to handle it without a way to test it would be worse than saying
 * so.
 */
final class Bzip2 {

    static final class Corrupt extends IOException {
        Corrupt(String m) { super(m); }
    }

    private static final int RUNA = 0, RUNB = 1;
    private static final int MAX_CODE_LEN = 23;
    private static final int GROUP_SIZE = 50;

    private final InputStream in;
    private int bitBuf, bitCount;

    private Bzip2(InputStream in) { this.in = in; }

    /** Decompress a whole bzip2 stream. Neither side is closed. */
    static void decompress(InputStream in, OutputStream out) throws IOException {
        new Bzip2(in).run(out);
    }

    private void run(OutputStream out) throws IOException {
        if (readBits(8) != 'B' || readBits(8) != 'Z' || readBits(8) != 'h')
            throw new Corrupt("not a bzip2 stream");
        int level = readBits(8) - '0';
        if (level < 1 || level > 9) throw new Corrupt("bad block size " + level);
        final int maxBlock = level * 100000;

        int[] tt = new int[maxBlock + 10];
        while (true) {
            /* The block and end-of-stream markers are 48 bits each: pi and
             * sqrt(pi) in BCD, which is the format's one flourish. */
            long magic = ((long) readBits(24) << 24) | readBits(24);
            if (magic == 0x177245385090L) {
                readBits(32);                       /* combined CRC */
                return;
            }
            if (magic != 0x314159265359L)
                throw new Corrupt("lost the block header");
            readBits(32);                           /* this block's CRC */
            if (readBits(1) != 0)
                throw new Corrupt("randomised block: deprecated, and not supported");
            int origPtr = readBits(24);
            decodeBlock(tt, maxBlock, origPtr, out);
        }
    }

    private void decodeBlock(int[] tt, int maxBlock, int origPtr, OutputStream out)
            throws IOException {

        /* ---- 1. which byte values occur, as a two-level bitmap ------------ */
        boolean[] inUse = new boolean[256];
        int used16 = readBits(16);
        for (int i = 0; i < 16; i++) {
            if ((used16 & (0x8000 >>> i)) == 0) continue;
            int bits = readBits(16);
            for (int j = 0; j < 16; j++)
                if ((bits & (0x8000 >>> j)) != 0) inUse[i * 16 + j] = true;
        }
        int[] seqToUnseq = new int[256];
        int nInUse = 0;
        for (int i = 0; i < 256; i++) if (inUse[i]) seqToUnseq[nInUse++] = i;
        if (nInUse == 0) throw new Corrupt("no symbols in use");
        final int alphaSize = nInUse + 2;
        final int eob = alphaSize - 1;

        /* ---- 2. which Huffman table each group of 50 symbols uses --------- */
        int nGroups = readBits(3);
        int nSelectors = readBits(15);
        if (nGroups < 2 || nGroups > 6) throw new Corrupt("bad group count");
        byte[] selectorMtf = new byte[nSelectors];
        for (int i = 0; i < nSelectors; i++) {
            int j = 0;
            while (readBits(1) == 1) if (++j >= nGroups) throw new Corrupt("bad selector");
            selectorMtf[i] = (byte) j;
        }
        byte[] selector = new byte[nSelectors];
        {   /* the selectors are themselves move-to-front coded */
            byte[] pos = new byte[6];
            for (int i = 0; i < nGroups; i++) pos[i] = (byte) i;
            for (int i = 0; i < nSelectors; i++) {
                int v = selectorMtf[i];
                byte tmp = pos[v];
                while (v > 0) { pos[v] = pos[v - 1]; v--; }
                pos[0] = tmp;
                selector[i] = tmp;
            }
        }

        /* ---- 3. the tables themselves, as delta-coded code lengths -------- */
        int[][] len = new int[nGroups][alphaSize];
        for (int t = 0; t < nGroups; t++) {
            int curr = readBits(5);
            for (int s = 0; s < alphaSize; s++) {
                while (true) {
                    if (curr < 1 || curr > 20) throw new Corrupt("bad code length");
                    if (readBits(1) == 0) break;
                    if (readBits(1) == 0) curr++; else curr--;
                }
                len[t][s] = curr;
            }
        }

        int[][] limit = new int[nGroups][MAX_CODE_LEN + 1];
        int[][] base  = new int[nGroups][MAX_CODE_LEN + 1];
        int[][] perm  = new int[nGroups][alphaSize];
        int[] minLens = new int[nGroups];
        for (int t = 0; t < nGroups; t++) {
            int minLen = 32, maxLen = 0;
            for (int s = 0; s < alphaSize; s++) {
                if (len[t][s] > maxLen) maxLen = len[t][s];
                if (len[t][s] < minLen) minLen = len[t][s];
            }
            makeTable(limit[t], base[t], perm[t], len[t], minLen, maxLen, alphaSize);
            minLens[t] = minLen;
        }

        /* ---- 4. MTF and run-length, into the BWT block ------------------- */
        int[] unzftab = new int[256];
        byte[] mtf = new byte[256];
        for (int i = 0; i < nInUse; i++) mtf[i] = (byte) i;

        int groupNo = -1, groupPos = 0, gSel = 0, gMinlen = 0;
        int[] gLimit = null, gBase = null, gPerm = null;
        int nblock = 0;

        int nextSym;
        {   /* the first symbol, then the loop reads its own */
            if (groupPos == 0) {
                groupNo++;
                if (groupNo >= nSelectors) throw new Corrupt("ran out of selectors");
                groupPos = GROUP_SIZE;
                gSel = selector[groupNo];
                gMinlen = minLens[gSel]; gLimit = limit[gSel];
                gBase = base[gSel]; gPerm = perm[gSel];
            }
            groupPos--;
            nextSym = decodeSymbol(gMinlen, gLimit, gBase, gPerm);
        }

        while (nextSym != eob) {
            if (nextSym == RUNA || nextSym == RUNB) {
                /* A run length in bijective base 2: RUNA is 1, RUNB is 2, each
                 * weighted by a doubling place value. */
                int es = -1, n = 1;
                do {
                    if (n > maxBlock) throw new Corrupt("run too long");
                    es += (nextSym == RUNA ? 1 : 2) * n;
                    n <<= 1;
                    if (groupPos == 0) {
                        groupNo++;
                        if (groupNo >= nSelectors) throw new Corrupt("ran out of selectors");
                        groupPos = GROUP_SIZE;
                        gSel = selector[groupNo];
                        gMinlen = minLens[gSel]; gLimit = limit[gSel];
                        gBase = base[gSel]; gPerm = perm[gSel];
                    }
                    groupPos--;
                    nextSym = decodeSymbol(gMinlen, gLimit, gBase, gPerm);
                } while (nextSym == RUNA || nextSym == RUNB);
                es++;
                int uc = seqToUnseq[mtf[0] & 0xff];
                unzftab[uc] += es;
                if (nblock + es > maxBlock) throw new Corrupt("block overrun");
                while (es-- > 0) tt[nblock++] = uc;
                continue;
            }
            /* An ordinary symbol: its index into the move-to-front list. */
            int nn = nextSym - 1;
            if (nn >= nInUse) throw new Corrupt("symbol out of range");
            byte tmp = mtf[nn];
            System.arraycopy(mtf, 0, mtf, 1, nn);
            mtf[0] = tmp;
            int uc = seqToUnseq[tmp & 0xff];
            unzftab[uc]++;
            if (nblock >= maxBlock) throw new Corrupt("block overrun");
            tt[nblock++] = uc;

            if (groupPos == 0) {
                groupNo++;
                if (groupNo >= nSelectors) throw new Corrupt("ran out of selectors");
                groupPos = GROUP_SIZE;
                gSel = selector[groupNo];
                gMinlen = minLens[gSel]; gLimit = limit[gSel];
                gBase = base[gSel]; gPerm = perm[gSel];
            }
            groupPos--;
            nextSym = decodeSymbol(gMinlen, gLimit, gBase, gPerm);
        }

        if (origPtr >= nblock) throw new Corrupt("origPtr past the end of the block");

        /* ---- 5. inverse Burrows-Wheeler ---------------------------------- */
        int[] cftab = new int[257];
        for (int i = 0; i < 256; i++) cftab[i + 1] = unzftab[i];
        for (int i = 1; i <= 256; i++) cftab[i] += cftab[i - 1];
        /* THE LINK RIDES IN THE TOP 24 BITS of the same word that holds the
         * byte, which is why the block limit is 900k rather than anything
         * bigger: 24 bits of index is the format's ceiling too. */
        for (int i = 0; i < nblock; i++) {
            int uc = tt[i] & 0xff;
            tt[cftab[uc]] |= (i << 8);
            cftab[uc]++;
        }

        /* ---- 6. and the final run-length layer --------------------------- */
        int tPos = tt[origPtr] >>> 8;
        byte[] obuf = new byte[65536];
        int op = 0;
        int count = 0, prev = -1;
        for (int i = 0; i < nblock; ) {
            int k = tt[tPos];
            int b = k & 0xff;
            tPos = k >>> 8;
            i++;

            if (count == 4) {
                /* Four equal bytes are followed by a count of how many MORE. */
                for (int r = 0; r < b; r++) {
                    obuf[op++] = (byte) prev;
                    if (op == obuf.length) { out.write(obuf); op = 0; }
                }
                count = 0; prev = -1;
                continue;
            }
            count = (b == prev) ? count + 1 : 1;
            prev = b;
            obuf[op++] = (byte) b;
            if (op == obuf.length) { out.write(obuf); op = 0; }
        }
        if (op > 0) out.write(obuf, 0, op);
    }

    /** limit/base/perm, the canonical-Huffman decode tables. */
    private static void makeTable(int[] limit, int[] base, int[] perm, int[] length,
                                  int minLen, int maxLen, int alphaSize) {
        int pp = 0;
        for (int i = minLen; i <= maxLen; i++)
            for (int j = 0; j < alphaSize; j++)
                if (length[j] == i) perm[pp++] = j;
        for (int i = 0; i < MAX_CODE_LEN; i++) base[i] = 0;
        for (int i = 0; i < alphaSize; i++) base[length[i] + 1]++;
        for (int i = 1; i < MAX_CODE_LEN; i++) base[i] += base[i - 1];
        for (int i = 0; i < MAX_CODE_LEN; i++) limit[i] = 0;
        int vec = 0;
        for (int i = minLen; i <= maxLen; i++) {
            vec += base[i + 1] - base[i];
            limit[i] = vec - 1;
            vec <<= 1;
        }
        for (int i = minLen + 1; i <= maxLen; i++)
            base[i] = ((limit[i - 1] + 1) << 1) - base[i];
    }

    private int decodeSymbol(int minLen, int[] limit, int[] base, int[] perm)
            throws IOException {
        int zn = minLen;
        int zvec = readBits(zn);
        while (true) {
            if (zn > 20) throw new Corrupt("code longer than the format allows");
            if (zvec <= limit[zn]) break;
            zn++;
            zvec = (zvec << 1) | readBits(1);
        }
        int idx = zvec - base[zn];
        if (idx < 0 || idx >= perm.length) throw new Corrupt("bad Huffman code");
        return perm[idx];
    }

    /** Big-endian bit order, which is what bzip2 uses throughout. */
    private int readBits(int n) throws IOException {
        while (bitCount < n) {
            int b = in.read();
            if (b < 0) throw new Corrupt("stream ended mid-symbol");
            bitBuf = (bitBuf << 8) | b;
            bitCount += 8;
        }
        int v = (bitBuf >>> (bitCount - n)) & (n == 32 ? -1 : ((1 << n) - 1));
        bitCount -= n;
        bitBuf &= (1 << bitCount) - 1;
        return v;
    }
}
