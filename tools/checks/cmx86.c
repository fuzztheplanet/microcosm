/* Tests the packer's x86 transform (cm_x86t in tools/cm.h). Known encodings
 * must be rewritten to the right absolute values, everything else must be left
 * alone, and the inverse must restore the input byte for byte. The edge cases
 * cover prefixes, three-byte opcode maps, RIP-relative and rel32 fields at the
 * ends of the code range, truncated instructions at the end of the buffer and
 * runs of prefixes. Then comes a random fuzz. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define CM_ENCODER
#define CM_CS 16
#define CM_CE 112
#define CM_TB 4                          /* the model's tables are not used here */
#include "../cm.h"

#define N 128
static cm_u32 rd(const cm_u8 *p) { return p[0] | (cm_u32)p[1] << 8 | (cm_u32)p[2] << 16 | (cm_u32)p[3] << 24; }

/* Put the instruction at `at` and transform. The 4-byte field at `field` must
 * become disp + at + end, or stay unchanged when end = 0, and the transform
 * must invert. */
static void one(const cm_u8 *ins, int len, int at, int field, int end) {
    cm_u8 b[N], o[N];
    for (int i = 0; i < N; ++i) b[i] = 0x90;            /* nops around it */
    memcpy(b + at, ins, (size_t)len);
    memcpy(o, b, N);
    cm_x86t(b, N, 1);
    for (int i = 0; i < N; ++i)
        if (!end || i < at + field || i >= at + field + 4) assert(b[i] == o[i]);
    if (end) assert(rd(b + at + field) == rd(o + at + field) + (cm_u32)(at + end));
    cm_x86t(b, N, 0);
    assert(!memcmp(b, o, N));
}

int main(void) {
    static const cm_u8 movss[] = { 0xf3, 0x0f, 0x10, 0x05, 1, 2, 3, 4 };            /* movss xmm0, [rip+d] */
    static const cm_u8 movrax[] = { 0x48, 0x8b, 0x05, 9, 8, 7, 6 };                 /* mov rax, [rip+d] */
    static const cm_u8 movimm[] = { 0xc7, 0x05, 1, 0, 0, 0, 0x2a, 0, 0, 0 };        /* mov dword [rip+d], 42 */
    static const cm_u8 call[] = { 0xe8, 0x10, 0, 0, 0 };                            /* call rel32 */
    static const cm_u8 jcc[] = { 0x0f, 0x84, 0x10, 0, 0, 0 };                       /* je rel32: left alone */
    static const cm_u8 pal[] = { 0x66, 0x0f, 0x3a, 0x0f, 0x05, 1, 2, 3, 4, 8 };    /* palignr xmm0, [rip+d], 8 */
    static const cm_u8 sib[] = { 0x8b, 0x04, 0x25, 1, 2, 3, 4 };                    /* mov eax, [d32] (SIB): absolute */
    static const cm_u8 movabs[] = { 0x48, 0xb8, 0xe8, 1, 2, 3, 4, 5, 6, 7 };        /* imm64 holding an E8 byte */
    one(movss, 8, 20, 4, 8);
    one(movrax, 7, 30, 3, 7);
    one(movimm, 10, 40, 2, 10);              /* RIP-relative ends after the immediate */
    one(call, 5, 16, 1, 5);                  /* at the start of the range */
    one(jcc, 6, 50, 2, 0);
    one(pal, 10, 60, 5, 10);
    one(sib, 7, 70, 3, 0);
    one(movabs, 10, 80, 2, 0);
    one(movss, 8, 108, 4, 8);                /* starts inside, field reaches past CM_CE */
    one(movss, 8, 112, 4, 0);                /* starts at CM_CE, so it's outside */
    one(call, 5, 116, 1, 0);                 /* in the last 15 bytes, which are never walked */
    one(movss, 8, 10, 4, 0);                 /* before CM_CS */
    {   /* more prefixes than an instruction may have, then garbage */
        cm_u8 p[16];
        memset(p, 0x66, 15); p[15] = 0x0f;
        one(p, 16, 40, 0, 0);
    }
    /* fuzz: random bytes salted with RIP-relative and rel32 encodings */
    cm_u32 s = 12345;
    for (int t = 0; t < 200000; ++t) {
        cm_u8 b[N], o[N];
        for (int i = 0; i < N; ++i) { s = s * 1103515245u + 12345u; b[i] = (cm_u8)(s >> 16); }
        for (int k = 0; k < 6; ++k) {
            s = s * 1103515245u + 12345u;
            int at = (int)(s >> 8) % (N - 8);
            b[at] = (s >> 24) & 1 ? 0xe8 : 0x8b; b[at + 1] = 0x05 | (cm_u8)((s >> 20 & 7) << 3);
        }
        memcpy(o, b, N);
        cm_x86t(b, N, 1);
        for (int i = 0; i < CM_CS; ++i) assert(b[i] == o[i]);
        for (int i = CM_CE + 15; i < N; ++i) assert(b[i] == o[i]);
        cm_x86t(b, N, 0);
        assert(!memcmp(b, o, N));
    }
    puts("Packer: the x86 transform rewrites rel32/RIP-relative fields exactly, leaves the rest alone and inverts (edge cases + 200000 fuzzed buffers)");
    return 0;
}
