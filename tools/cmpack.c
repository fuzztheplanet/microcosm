/*
 * cmpack.c - the build-host half of MICROCOSM's context-mixing packer: compress
 * a file with the model in cm.h (configured by the same -D flags as the
 * launcher), writing the raw coded stream. pack.py compiles it once per
 * candidate configuration and keeps the smallest result.
 *   cmpack <input> <output>
 */
#include <stdio.h>
#include <stdlib.h>
#define CM_ENCODER
#include "cm.h"

int main(int argc, char **argv) {
    if (argc != 3) { fputs("usage: cmpack <input> <output>\n", stderr); return 2; }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    /* out holds the coded stream. Incompressible input can expand it a little,
     * so size it generously. A real payload compresses, but the equivalence gate
     * also packs random data. */
    cm_u8 *buf = malloc((size_t)n + 1), *out = malloc((size_t)n * 2 + 64);
    if (!buf || !out || fread(buf, 1, (size_t)n, f) != (size_t)n) { fputs("cmpack: read failed\n", stderr); return 2; }
    fclose(f);
    cm_x86t(buf, (cm_u32)n, 1);         /* the decoder undoes it after decoding */
    cm_init(buf);                       /* the model rewrites each byte as it completes */
    size_t op = 0;
    for (long i = 0; i < n; ++i)
        for (int b = 7; b >= 0; --b) {
            int y = buf[i] >> b & 1;
            cm_u32 xm = cm_mid(cm_predict());
            if (y) cm_x2 = xm; else cm_x1 = xm + 1;
            cm_update(y);
            while (((cm_x1 ^ cm_x2) & 0xff000000u) == 0) {
                out[op++] = (cm_u8)(cm_x2 >> 24);
                cm_x1 <<= 8; cm_x2 = cm_x2 << 8 | 255;
            }
        }
    out[op++] = (cm_u8)(cm_x1 >> 24);   /* the decoder reads 0xff past the end */
    f = fopen(argv[2], "wb");
    if (!f || fwrite(out, 1, op, f) != op || fclose(f)) { fputs("cmpack: write failed\n", stderr); return 2; }
    printf("%zu\n", op);
    return 0;
}
