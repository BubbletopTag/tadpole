package org.tadpole.view;

/**
 * LZO1X decompression, which is what a LeapPad2 UBIFS volume is compressed
 * with — measured on the stock 4.6.0.784 image: 15453 data nodes LZO, 2793
 * stored, and not one zlib.
 *
 * <p>Java has no LZO and Android bundles none, so this is the algorithm
 * written out. It is a faithful transcription of the reference decompressor
 * (lzo1x_d.ch, the same shape the Linux kernel's
 * lzo1x_decompress_safe.c has), with the gotos turned into an explicit program
 * counter rather than restructured into something that looks tidier and is
 * harder to check against the original. Every label below has the same name it
 * has there.
 *
 * <p>WHY IT IS WRITTEN THAT WAY. A subtly wrong LZO does not fail: it produces
 * plausible-looking bytes, and what you get is a firmware that extracts
 * without error and then behaves strangely for reasons nothing will ever
 * connect back to a decompressor. Keeping the control flow identical to the
 * reference means the comparison is line by line, and it was then checked
 * against lzallright over every compressed node in a real image — see the note
 * on the test in the commit.
 *
 * <p>Bounds are checked on the way out rather than trusted: this reads an
 * image the user supplied, and a corrupt length field otherwise walks off the
 * end of an array with an exception that says nothing about what was wrong.
 */
final class Lzo {

    private Lzo() {}

    private static final int L_INIT = 0, L_MAIN = 1, L_FIRST_LITERAL_RUN = 2,
                             L_MATCH = 3, L_MATCH_DONE = 4, L_MATCH_NEXT = 5;

    static final class Corrupt extends Exception {
        Corrupt(String m) { super(m); }
    }

    /**
     * Decompress `inLen` bytes at `in[inOff]` into `out`, which must be big
     * enough for the whole result — UBIFS records the uncompressed size in the
     * data node, so the caller always knows it.
     *
     * @return the number of bytes written.
     */
    static int decompress(byte[] in, int inOff, int inLen, byte[] out, int outLen)
            throws Corrupt {
        final int ipEnd = inOff + inLen;
        int ip = inOff, op = 0, t = 0, m = 0;
        int pc = L_INIT;

        if (inLen < 3) throw new Corrupt("input too short");

        for (;;) {
            switch (pc) {

            case L_INIT:
                if ((in[ip] & 0xff) > 17) {
                    t = (in[ip++] & 0xff) - 17;
                    if (t < 4) { pc = L_MATCH_NEXT; break; }
                    need(ip, t, ipEnd); room(op, t, outLen);
                    while (t-- > 0) out[op++] = in[ip++];
                    pc = L_FIRST_LITERAL_RUN;
                    break;
                }
                pc = L_MAIN;
                break;

            case L_MAIN:
                need(ip, 1, ipEnd);
                t = in[ip++] & 0xff;
                if (t >= 16) { pc = L_MATCH; break; }
                if (t == 0) {
                    while (in[ip] == 0) { t += 255; ip++; need(ip, 1, ipEnd); }
                    t += 15 + (in[ip++] & 0xff);
                }
                need(ip, t + 3, ipEnd); room(op, t + 3, outLen);
                { int n = t + 3; while (n-- > 0) out[op++] = in[ip++]; }
                pc = L_FIRST_LITERAL_RUN;
                break;

            case L_FIRST_LITERAL_RUN:
                need(ip, 1, ipEnd);
                t = in[ip++] & 0xff;
                if (t >= 16) { pc = L_MATCH; break; }
                need(ip, 1, ipEnd);
                m = op - (1 + 0x0800) - (t >> 2) - ((in[ip++] & 0xff) << 2);
                back(m); room(op, 3, outLen);
                out[op++] = out[m++]; out[op++] = out[m++]; out[op++] = out[m];
                pc = L_MATCH_DONE;
                break;

            case L_MATCH:
                if (t >= 64) {
                    need(ip, 1, ipEnd);
                    m = op - 1 - ((t >> 2) & 7) - ((in[ip++] & 0xff) << 3);
                    t = (t >> 5) - 1;
                } else if (t >= 32) {
                    t &= 31;
                    if (t == 0) {
                        need(ip, 1, ipEnd);
                        while (in[ip] == 0) { t += 255; ip++; need(ip, 1, ipEnd); }
                        t += 31 + (in[ip++] & 0xff);
                    }
                    need(ip, 2, ipEnd);
                    m = op - 1 - (((in[ip] & 0xff) | ((in[ip + 1] & 0xff) << 8)) >> 2);
                    ip += 2;
                } else if (t >= 16) {
                    m = op - ((t & 8) << 11);
                    t &= 7;
                    if (t == 0) {
                        need(ip, 1, ipEnd);
                        while (in[ip] == 0) { t += 255; ip++; need(ip, 1, ipEnd); }
                        t += 7 + (in[ip++] & 0xff);
                    }
                    need(ip, 2, ipEnd);
                    m -= ((in[ip] & 0xff) | ((in[ip + 1] & 0xff) << 8)) >> 2;
                    ip += 2;
                    /* THE END MARKER, and it is a position rather than a code:
                     * a match that would start exactly where the output ends
                     * cannot be a real one. */
                    if (m == op) return op;
                    m -= 0x4000;
                } else {
                    need(ip, 1, ipEnd);
                    m = op - 1 - (t >> 2) - ((in[ip++] & 0xff) << 2);
                    back(m); room(op, 2, outLen);
                    out[op++] = out[m++]; out[op++] = out[m];
                    pc = L_MATCH_DONE;
                    break;
                }
                back(m); room(op, t + 2, outLen);
                /* BYTE AT A TIME, and it must stay that way: an LZO match is
                 * allowed to overlap its own output — that is how it encodes a
                 * run — so a block copy would read bytes this loop has not
                 * written yet. */
                { int n = t + 2; while (n-- > 0) out[op++] = out[m++]; }
                pc = L_MATCH_DONE;
                break;

            case L_MATCH_DONE:
                /* The literal count rides in the low two bits of the opcode,
                 * which is two bytes back whichever branch above ran. */
                t = in[ip - 2] & 3;
                pc = (t == 0) ? L_MAIN : L_MATCH_NEXT;
                break;

            case L_MATCH_NEXT:
                need(ip, t, ipEnd); room(op, t, outLen);
                while (t-- > 0) out[op++] = in[ip++];
                need(ip, 1, ipEnd);
                t = in[ip++] & 0xff;
                pc = L_MATCH;
                break;

            default:
                throw new Corrupt("bad state");
            }
        }
    }

    private static void need(int ip, int n, int end) throws Corrupt {
        if (ip + n > end) throw new Corrupt("input overrun");
    }
    private static void room(int op, int n, int outLen) throws Corrupt {
        if (op + n > outLen) throw new Corrupt("output overrun");
    }
    private static void back(int m) throws Corrupt {
        if (m < 0) throw new Corrupt("match before start of output");
    }
}
