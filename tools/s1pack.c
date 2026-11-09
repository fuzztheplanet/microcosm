/*
 * s1pack.c - the build-host half of the launcher's first stage (tools/s1.h). It
 * picks the models that code the launcher's second stage smallest, choosing for
 * each one which of the last four bytes it hashes and its log2 weight. Then it
 * writes the coded stream and the configuration the first stage is built with.
 *   s1pack [-q] <input> <output> <config.h>
 * The search is a deterministic greedy walk from a fixed start. It keeps moving
 * one weight up or down, swapping one mask, or adding or dropping one model,
 * until nothing helps. With -q it just codes with the start configuration.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s1.h"

/* s1dec.S stores one .long mask and one .byte weight per model. Include
 * these bytes in every comparison, so a smaller stream cannot buy a larger
 * launcher by adding models. */
enum { MODEL_BYTES = 5 };
static unsigned char *g_in, *g_out;
static unsigned g_n;

static unsigned cost(const unsigned char *m, const unsigned char *w, int nm) {
    unsigned mk[S1_MAXM];
    for (int i = 0; i < nm; ++i) {
        mk[i] = 0;
        for (int j = 0; j < 4; ++j) if (m[i] >> j & 1) mk[i] |= 255u << (24 - 8 * j);
    }
    memset(s1_t, 0, sizeof s1_t);
    return s1_encode(g_in, g_n, g_out, mk, w, nm) + MODEL_BYTES * nm;
}

int main(int argc, char **argv) {
    int quick = argc > 1 && !strcmp(argv[1], "-q");
    argv += quick; argc -= quick;
    if (argc != 4) { fputs("usage: s1pack [-q] <input> <output> <config.h>\n", stderr); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    g_n = (unsigned)ftell(f);
    fseek(f, 0, SEEK_SET);
    g_in = malloc(g_n + 1); g_out = malloc(2 * g_n + 64);
    if (!g_in || !g_out || fread(g_in, 1, g_n, f) != g_n) { fputs("s1pack: read failed\n", stderr); return 2; }
    fclose(f);
    /* the start: a search's result on the shipped second stage */
    unsigned char m[S1_MAXM] = { 0x0, 0x1, 0x3, 0x2, 0x5, 0x4 }, w[S1_MAXM] = { 0, 1, 2, 0, 1, 0 };
    int nm = 6;
    unsigned best = cost(m, w, nm);
    for (int improved = !quick; improved;) {
        improved = 0;
        for (int i = 0; i < nm; ++i)                     /* a weight */
            for (int d = -1; d <= 1; d += 2) {
                if (w[i] + d < 0 || w[i] + d > 7) continue;
                w[i] = (unsigned char)(w[i] + d);
                unsigned c = cost(m, w, nm);
                if (c < best) { best = c; improved = 1; } else w[i] = (unsigned char)(w[i] - d);
            }
        for (int i = 0; i < nm; ++i)                     /* a mask */
            for (int k = 0; k < 16; ++k) {
                int dup = 0;
                for (int j = 0; j < nm; ++j) dup |= m[j] == k;
                if (dup) continue;
                unsigned char o = m[i];
                m[i] = (unsigned char)k;
                unsigned c = cost(m, w, nm);
                if (c < best) { best = c; improved = 1; } else m[i] = o;
            }
        for (int k = 0; k < 16 && nm < S1_MAXM; ++k) {   /* one more model */
            int dup = 0;
            for (int j = 0; j < nm; ++j) dup |= m[j] == k;
            for (int wk = 0; wk < 8 && !dup; ++wk) {
                m[nm] = (unsigned char)k; w[nm] = (unsigned char)wk;
                unsigned c = cost(m, w, nm + 1);
                if (c < best) { best = c; improved = 1; ++nm; break; }
            }
        }
        for (int i = 0; i < nm && nm > 1; ++i) {         /* one fewer */
            unsigned char sm[S1_MAXM], sw[S1_MAXM];
            memcpy(sm, m, sizeof sm); memcpy(sw, w, sizeof sw);
            memmove(m + i, m + i + 1, (size_t)(nm - i - 1)); memmove(w + i, w + i + 1, (size_t)(nm - i - 1));
            unsigned c = cost(m, w, nm - 1);
            if (c < best) { best = c; improved = 1; --nm; break; }
            memcpy(m, sm, sizeof sm); memcpy(w, sw, sizeof sw);
        }
    }
    unsigned len = cost(m, w, nm) - MODEL_BYTES * nm;
    {
        unsigned mk[S1_MAXM];
        unsigned char *back = malloc(g_n + 1);
        for (int i = 0; i < nm; ++i) {
            mk[i] = 0;
            for (int j = 0; j < 4; ++j) if (m[i] >> j & 1) mk[i] |= 255u << (24 - 8 * j);
        }
        memset(s1_t, 0, sizeof s1_t);
        if (!back) return 2;
        s1_decode(g_out, back, g_n, mk, w, nm);
        if (memcmp(back, g_in, g_n)) { fputs("s1pack: round trip failed\n", stderr); return 1; }
        free(back);
    }
    f = fopen(argv[2], "wb");
    if (!f || fwrite(g_out, 1, len, f) != len || fclose(f)) { fputs("s1pack: write failed\n", stderr); return 2; }
    f = fopen(argv[3], "w");
    if (!f) { perror(argv[3]); return 2; }
    fprintf(f, "#define S1_LEN %u\n#define S1_NM %d\n#define S1_MASKS", g_n, nm);
    for (int i = 0; i < nm; ++i) {
        unsigned mk = 0;
        for (int j = 0; j < 4; ++j) if (m[i] >> j & 1) mk |= 255u << (24 - 8 * j);
        fprintf(f, "%s 0x%x", i ? "," : "", mk);
    }
    fputs("\n#define S1_WTS", f);
    for (int i = 0; i < nm; ++i) fprintf(f, "%s %d", i ? "," : "", w[i]);
    fputc('\n', f);
    if (fclose(f)) return 2;
    printf("%u\n", len);
    return 0;
}
