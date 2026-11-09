/*
 * s1.h - the launcher's first stage: a very small counting context-mixing coder
 * for the launcher's own second stage (the cm.h decoder and its glue). This is
 * the reference: the packer (tools/s1pack.c) codes with it while picking the
 * models, and decodes the result back. tools/s1dec.S, the first stage, is its
 * decoder written by hand.
 *
 * Each model hashes some of the last four bytes with the bits of the current
 * byte so far, into a table of count pairs. A pair counts the zeros and ones
 * seen, and each bit halves the count of the other value. A model's bytes come
 * from a byte mask, stored already widened to a 32-bit mask over the last four
 * bytes as they lie in memory (the latest on top), so the first stage just
 * loads it.
 * The prediction is the weighted share of ones over the models, each weighted
 * 2^w and doubled while its context has only ever seen one value, so the
 * decoder needs no logistic tables, only a division. A 32-bit carry-less binary
 * arithmetic coder codes the bit with a 16-bit probability. The decoder
 * renormalizes before each bit, starting empty, so that is where it reads its
 * first four bytes. The encoder flushes all four bytes of its low end, so
 * nothing past the stream is ever read.
 * The table is .bss, so it costs no file bytes.
 */
#ifndef MICROCOSM_S1_H
#define MICROCOSM_S1_H

#define S1_TB 20                         /* log2 of the count-pair table */
#define S1_MAXM 12

static unsigned char s1_t[2u << S1_TB];  /* count pairs: zeros, ones */
static unsigned char *s1_q[S1_MAXM];     /* each model's pair for this bit */
static unsigned s1_h[S1_MAXM];           /* each model's context for this byte */

/* Every model's context for the next byte, from the last four (hist, the
 * latest in the top byte). */
static void s1_ctx(unsigned hist, const unsigned *mk, int nm) {
    for (int m = 0; m < nm; ++m) s1_h[m] = ((hist & mk[m]) + (unsigned)m) * 0x9E3779B1u;
}

/* P(next bit = 1), 16 bits, given the byte's bits so far (c0, from 1). */
static unsigned s1_p(unsigned c0, const unsigned char *wt, int nm) {
    unsigned long long n1 = 0, nn = 0;
    for (int m = 0; m < nm; ++m) {
        unsigned char *q = s1_q[m] = s1_t + (((s1_h[m] + c0) * 0x2C1B3C6Du) >> (32 - S1_TB) << 1);
        unsigned w = 1u << wt[m] << (!q[0] | !q[1]);
        n1 += w * q[1];
        nn += w * (q[0] + q[1]);
    }
    unsigned p = (unsigned)(((2 * n1 + 1) << 16) / (2 * nn + 2));
    return p + !p;
}

/* Learn bit y. */
static void s1_upd(int y, int nm) {
    for (int m = 0; m < nm; ++m) {
        unsigned char *q = s1_q[m];
        q[y] += q[y] < 255;
        if (q[!y] > 2) q[!y] = (unsigned char)(q[!y] / 2 + 1);
    }
}

/* Code n bytes of buf into out; returns the coded length. */
static unsigned s1_encode(const unsigned char *buf, unsigned n, unsigned char *out,
                          const unsigned *mk, const unsigned char *wt, int nm) {
    unsigned x1 = 0, x2 = ~0u, op = 0, hist = 0;
    for (unsigned i = 0; i < n; ++i) {
        s1_ctx(hist, mk, nm);
        for (int b = 7; b >= 0; --b) {
            int y = buf[i] >> b & 1;
            unsigned xm = x1 + ((x2 - x1) >> 16) * s1_p((buf[i] | 256u) >> (b + 1), wt, nm);
            if (y) x2 = xm; else x1 = xm + 1;
            s1_upd(y, nm);
            while (((x1 ^ x2) & 0xff000000u) == 0) {
                out[op++] = (unsigned char)(x2 >> 24);
                x1 <<= 8; x2 = x2 << 8 | 255;
            }
        }
        hist = hist >> 8 | (unsigned)buf[i] << 24;
    }
    for (int i = 0; i < 4; ++i) out[op++] = (unsigned char)(x1 >> (24 - 8 * i));
    return op;
}
/* Decode n bytes from src into out (tools/s1dec.S is this, by hand). */
static void s1_decode(const unsigned char *src, unsigned char *out, unsigned n,
                      const unsigned *mk, const unsigned char *wt, int nm) {
    unsigned x1 = 0, x2 = 0, x = 0, hist = 0;
    for (unsigned i = 0; i < n; ++i) {
        unsigned c0 = 1;
        s1_ctx(hist, mk, nm);
        while (c0 < 256) {
            while (((x1 ^ x2) & 0xff000000u) == 0) {
                x1 <<= 8; x2 = x2 << 8 | 255; x = x << 8 | *src++;
            }
            unsigned xm = x1 + ((x2 - x1) >> 16) * s1_p(c0, wt, nm);
            int y = x <= xm;
            if (y) x2 = xm; else x1 = xm + 1;
            s1_upd(y, nm);
            c0 = c0 * 2 + (unsigned)y;
        }
        hist = hist >> 8 | (unsigned)(out[i] = (unsigned char)c0) << 24;
    }
}

#endif /* MICROCOSM_S1_H */
