/*
 * platform_common.c - the bits both platform backends need, kept in one place:
 * a monotonic clock, the top-left/bottom-left row flip, and a PNG writer for
 * screenshots.
 *
 * The PNG writer is as simple as it gets: it wraps the pixels in stored
 * (uncompressed) DEFLATE blocks. The files are big, but the code is short and
 * easy to check, and it only runs in dev builds and tests.
 */
#include "microcosm/mc_platform.h"
#include "microcosm/mc.h"

#include <time.h>
#include "microcosm/mc_imports.h"   /* payload calls by hash; keep after system headers */

double mc_time_seconds(void) {
    static int inited = 0;
    static struct timespec t0;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!inited) { t0 = t; inited = 1; }
    return (double)(t.tv_sec - t0.tv_sec) + (double)(t.tv_nsec - t0.tv_nsec) * 1e-9;
}

/* GL hands back pixels bottom row first and images want the top row first, so
 * swap the rows in place. The scratch buffer is a fixed 16 KB, so very wide
 * rows get swapped a chunk at a time instead of needing a full-row
 * allocation. */
void mc_pixels_flip(unsigned char *rgba, int w, int h) {
    int stride = w * 4;
    unsigned char tmp[4096 * 4];
    for (int y = 0; y < h / 2; ++y) {
        unsigned char *a = rgba + (size_t)y * stride;
        unsigned char *b = rgba + (size_t)(h - 1 - y) * stride;
        int x = 0;
        while (x < stride) {
            int n = stride - x;
            if (n > (int)sizeof(tmp)) n = (int)sizeof(tmp);
            for (int i = 0; i < n; ++i) tmp[i] = a[x + i];
            for (int i = 0; i < n; ++i) a[x + i] = b[x + i];
            for (int i = 0; i < n; ++i) b[x + i] = tmp[i];
            x += n;
        }
    }
}

#if defined(MC_DEV)
#include <stdio.h>
#include <stdlib.h>

static mc_u32 crc_table[256];
static int    crc_ready;
static void crc_init(void) {
    for (mc_u32 n = 0; n < 256; ++n) {
        mc_u32 c = n;
        for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xedb88320u ^ (c >> 1) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}
static void put32(FILE *f, mc_u32 v) {
    fputc((int)(v >> 24) & 0xff, f); fputc((int)(v >> 16) & 0xff, f);
    fputc((int)(v >> 8) & 0xff, f);  fputc((int)v & 0xff, f);
}
/* Incremental CRC that can span several buffers (tag, then data). */
static mc_u32 crc32_begin(void) { if (!crc_ready) crc_init(); return 0xffffffffu; }
static mc_u32 crc32_add(mc_u32 c, const unsigned char *p, size_t n) {
    for (size_t i = 0; i < n; ++i) c = crc_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c;
}
static void chunk(FILE *f, const char *tag, const unsigned char *data, size_t len) {
    put32(f, (mc_u32)len);
    fwrite(tag, 1, 4, f);
    if (len) fwrite(data, 1, len, f);
    mc_u32 c = crc32_begin();
    c = crc32_add(c, (const unsigned char *)tag, 4);
    if (len) c = crc32_add(c, data, len);
    put32(f, c ^ 0xffffffffu);
}

static int png_write(const char *path, int w, int h, const unsigned char *rgba) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    static const unsigned char sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);

    unsigned char ihdr[13];
    ihdr[0] = (unsigned char)(w >> 24); ihdr[1] = (unsigned char)(w >> 16);
    ihdr[2] = (unsigned char)(w >> 8);  ihdr[3] = (unsigned char)w;
    ihdr[4] = (unsigned char)(h >> 24); ihdr[5] = (unsigned char)(h >> 16);
    ihdr[6] = (unsigned char)(h >> 8);  ihdr[7] = (unsigned char)h;
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(f, "IHDR", ihdr, 13);

    /* Build the raw (filtered) scanline stream: filter byte 0 + row. */
    size_t raw_len = (size_t)h * (1 + (size_t)w * 4);
    unsigned char *raw = malloc(raw_len);
    if (!raw) { fclose(f); return 0; }
    for (int y = 0; y < h; ++y) {
        unsigned char *dst = raw + (size_t)y * (1 + (size_t)w * 4);
        dst[0] = 0;
        for (int x = 0; x < w * 4; ++x) dst[1 + x] = rgba[(size_t)y * w * 4 + x];
    }

    /* Wrap it in a zlib stream: 2-byte header, the data as stored blocks (each
     * capped at 65535 bytes, with its length and one's-complement), adler32 at
     * the end. No compression, just framing. */
    size_t zcap = raw_len + raw_len / 65535 * 5 + 64;
    unsigned char *z = malloc(zcap);
    if (!z) { free(raw); fclose(f); return 0; }
    size_t zi = 0;
    z[zi++] = 0x78; z[zi++] = 0x01;           /* zlib header */
    size_t off = 0;
    while (off < raw_len) {
        size_t block = raw_len - off;
        if (block > 65535) block = 65535;
        int final = (off + block >= raw_len);
        z[zi++] = (unsigned char)(final ? 1 : 0); /* BFINAL, BTYPE=00 */
        z[zi++] = (unsigned char)(block & 0xff);
        z[zi++] = (unsigned char)((block >> 8) & 0xff);
        z[zi++] = (unsigned char)(~block & 0xff);
        z[zi++] = (unsigned char)((~block >> 8) & 0xff);
        for (size_t i = 0; i < block; ++i) z[zi++] = raw[off + i];
        off += block;
    }
    /* adler32 over raw */
    mc_u32 a = 1, b = 0;
    for (size_t i = 0; i < raw_len; ++i) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    mc_u32 adler = (b << 16) | a;
    z[zi++] = (unsigned char)(adler >> 24); z[zi++] = (unsigned char)(adler >> 16);
    z[zi++] = (unsigned char)(adler >> 8);  z[zi++] = (unsigned char)adler;

    chunk(f, "IDAT", z, zi);
    chunk(f, "IEND", NULL, 0);

    free(z); free(raw);
    fclose(f);
    return 1;
}

int mc_window_save_png(mc_window *w, const char *path) {
    int ww, hh;
    mc_window_size(w, &ww, &hh);
    unsigned char *px = malloc((size_t)ww * hh * 4);
    if (!px) return 0;
    int ok = mc_window_read_pixels(w, px) && png_write(path, ww, hh, px);
    free(px);
    return ok;
}
#else
int mc_window_save_png(mc_window *w, const char *path) { (void)w; (void)path; return 0; }
#endif /* MC_DEV */
