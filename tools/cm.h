/*
 * cm.h - MICROCOSM's context-mixing coder: the model and the binary arithmetic
 * coder. The packer (tools/cmpack.c, on the build host) and the C reference
 * decoder (tools/mcstub.c) both include this file, so the two can never
 * disagree. The launcher ships a hand translation of the decoder,
 * tools/s2dec.S, and pack.py checks that it reproduces the payload before
 * shipping. All the arithmetic is integer, so any host reproduces the
 * launcher's predictions bit for bit, and the code needs no libc.
 *
 * The payload's code first goes through an x86 transform (cm_x86t) that makes
 * relative call/jump targets and RIP-relative displacements absolute, so every
 * call to a given function, and every load of a given constant, reads alike.
 * Each bit is then predicted by a set of hashed context models and a match
 * model. Most of the context models key on a byte mask over the last 8 bytes,
 * covering the dense orders and the sparse contexts that suit x86 code, and the
 * extra models listed below join them. Seven more depend on what is being coded,
 * and cm_ctx builds them. Inside the code they are x86 models, keyed on the
 * instruction being decoded: which field the byte is (prefix/opcode, ModRM, SIB,
 * displacement or immediate), its opcode, the opcodes before it, the instruction
 * so far, the last byte, the instruction before, the last instruction with this
 * opcode, and the last two RIP-relative targets. Outside the code they read the
 * GLSL's shape instead: the call and argument a byte is in, the statement's
 * first word and the one before, the bracket and brace depth, order 3, and the
 * character classes of the last 16 bytes.
 *
 * Small online neural mixers combine the predictions in the logistic domain,
 * each picking its weight set by a different context. Mixer 0 uses the match
 * length and the x86 field (out of the code, the last byte's class) with the
 * bits of this byte so far. Mixers 1-4 each use one of the last four bytes with
 * the bit's position. Mixer 5 uses the opcode with the bit's position, or out
 * of the code the classes of the last three bytes and whether a statement and a
 * bracket are open. A second mixer combines the first layer's outputs, keyed by
 * the bits of this byte so far and the x86 field or the last byte's class. Two
 * APMs (adaptive probability maps) then refine the result, one by the bits of
 * this byte so far and one by those plus the last byte, and a 32-bit carry-less
 * binary arithmetic coder codes the bit.
 *
 * Context slots are grouped per nibble, 16 to a block (32 bytes), in one shared
 * table, and slot 0 of each block holds a check. A context takes whichever of
 * its block and the neighbouring one (64 bytes together) already holds it,
 * or else the less used of the two, cleared first, so colliding contexts don't
 * pollute each other's statistics.
 *
 * The configuration is compile-time, so the launcher only carries what it uses.
 * pack.py searches it per payload and builds the launcher with the winner:
 *   CM_MASKS  byte masks (bit j = the byte j + 1 back), one context model each
 *   CM_XSET   extra models: 1 word, 2 b1 + 4-byte column, 4 b1 -> what followed
 *             it last, 8 b2b1 -> what followed it last
 *   CM_LIM    counter adaptation limit (1-15)  CM_LR, CM_LR2  mixer rate shifts
 *   CM_NM     first-layer mixers (1-6)         CM_TB  log2 of the slot table
 *   CM_APM    the APMs (0/1)                   CM_AR  their rate shift
 *   CM_WARM   the first layer learns 4x faster over the first 2^CM_WARM bytes,
 *             2x up to 4 * 2^CM_WARM (0: no warm-up)
 *   CM_MM     the match model's shortest match (the last 8 bytes find it, the
 *             candidate must agree on CM_MM)
 *   CM_WI     the first-layer mixers' initial weights, in 1/64ths
 *   CM_CS, CM_CE  the payload's code (.text) as a file range; none by default
 * All tables live in .bss, so they cost no file bytes.
 */
#ifndef MICROCOSM_CM_H
#define MICROCOSM_CM_H

#ifndef CM_MASKS
#define CM_MASKS 0x00, 0x03, 0x05, 0x06, 0x0c, 0x19, 0x32, 0x61, 0x8a, 0xab, 0xc5
#endif
#ifndef CM_XSET
#define CM_XSET 13
#endif
#ifndef CM_LIM
#define CM_LIM 14
#endif
#ifndef CM_LR
#define CM_LR 9
#endif
#ifndef CM_LR2
#define CM_LR2 13
#endif
#ifndef CM_NM
#define CM_NM 5
#endif
#ifndef CM_APM
#define CM_APM 1
#endif
#ifndef CM_AR
#define CM_AR 6
#endif
#ifndef CM_WARM
#define CM_WARM 12
#endif
#ifndef CM_MM
#define CM_MM 12
#endif
#ifndef CM_WI
#define CM_WI 12
#endif
#ifndef CM_TB
#define CM_TB 26
#endif
#ifndef CM_CS
#define CM_CS 0
#endif
#ifndef CM_CE
#define CM_CE 0
#endif

typedef unsigned int cm_u32;
typedef unsigned short cm_u16;
typedef unsigned char cm_u8;

static const cm_u8 cm_mask[] = { CM_MASKS };
#define CM_NK ((int)sizeof cm_mask)
#define CM_NE (CM_NK + __builtin_popcount(CM_XSET) + 7)  /* context models: masks, extras, x86 (7; the text's shape outside the code) */
#define CM_NI (CM_NE + 2)                                /* mixer inputs: models, match, bias */
#define CM_WS (5 * 11 * 256 + (CM_NM - 1) * 2048 + (CM_NM > 5) * 8192)   /* first-layer weight sets */
#define CM_MB 22                                         /* match-model hash bits */

static int cm_sq[4096];                  /* squash: logistic domain (d + 2048) -> 12-bit p */
static int cm_st[4096];                  /* stretch: its inverse */
static int cm_dt[16];                    /* counter adaptation rates, 1/(n + 1.5) */
static cm_u16 cm_t[1 << CM_TB];          /* context slots: p (12 b) | count (4 b), ^ 1<<15 */
static cm_u32 cm_cx[CM_NE], cm_base[CM_NE], cm_ix[CM_NE];
static int cm_in[CM_NI];
static int cm_w[CM_WS * CM_NI];          /* mixer weights: (match bucket, x86 field or text class, c0), (b1..b4, bit), (opcode or classes, bit) */
static int cm_w2[11 * 256 * CM_NM];      /* second layer, by x86 field or text class, and c0 */
static int cm_ms[CM_NM], cm_mp[CM_NM], cm_md[CM_NM], cm_s2;
static cm_u32 cm_mt[1 << CM_MB];         /* match model: hash of the last 8 bytes -> position */
static cm_u16 cm_msm[32];                /* match model counters, by (length, expected bit) */
static cm_u32 cm_i1[256], cm_i2[65536];  /* indirect: the bytes that last followed b1, b2b1 */
static cm_u32 cm_x1, cm_x2 = 0xffffffffu, cm_pos, cm_c4, cm_c8, cm_word, cm_cls;
static int cm_c0 = 1, cm_nib = 1, cm_bit, cm_pm, cm_mlen, cm_mptr, cm_mi;
static cm_u8 *cm_buf;                    /* the payload so far (the decoder writes it) */
static cm_u32 cm_dep, cm_bd, cm_sw;      /* the text: bracket and brace depth, the statement's first word */
static cm_u32 cm_psw;                    /* the first word of the statement before (the last that had one) */
static cm_u32 cm_fc[256];                /* per bracket depth: the word before the bracket + its commas */
static cm_u16 cm_a[(256 + 65536) * 33];  /* APMs: by c0, then by c0 | b1 << 8; 33 buckets each */
static int cm_aj[2];                     /* each APM's lower bucket for this bit */

/* ---- x86-64 instructions ---- */

/* The field the next byte of the code falls in. CM_F_NEW means the instruction
 * is complete, and CM_F_DATA that the byte is outside the code. */
enum { CM_F_OP, CM_F_OP2, CM_F_MODRM, CM_F_SIB, CM_F_DISP, CM_F_IMM, CM_F_NEW, CM_F_DATA };
static cm_u32 cm_xs, cm_xo, cm_xh, cm_pi; /* instruction start, opcode, opcode history, the last instruction */
static cm_u32 cm_xl[1024];               /* per opcode: a hash of the last such instruction */
static cm_u32 cm_xp, cm_rt, cm_rt2;      /* the last instruction's opcode << 8 | ModRM; the last two RIP-relative targets */
static int cm_xf = CM_F_DATA;            /* the field of the next byte */

/* Opcode attributes, generated from the opcode map. One-byte opcodes get 4 bits
 * each: bit 0 is ModRM, bits 1-2 are the immediate (none, imm8, imm16/32 or
 * special) and bit 3 marks a prefix. The 0F opcodes follow, 2 bits each: bit 0
 * ModRM, bit 1 imm8. */
static const cm_u8 cm_xt[192] = {
    0x11, 0x11, 0x42, 0x00, 0x11, 0x11, 0x42, 0x00, 0x11, 0x11, 0x42, 0x00, 0x11, 0x11, 0x42, 0x00,
    0x11, 0x11, 0x42, 0x08, 0x11, 0x11, 0x42, 0x08, 0x11, 0x11, 0x42, 0x08, 0x11, 0x11, 0x42, 0x08,
    0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x11, 0x88, 0x88, 0x54, 0x32, 0x00, 0x00, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22,
    0x53, 0x33, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x66, 0x66, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x22, 0x22, 0x22, 0x22, 0x66, 0x66, 0x66, 0x66,
    0x33, 0x06, 0x11, 0x53, 0x06, 0x06, 0x20, 0x00, 0x11, 0x11, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11,
    0x22, 0x22, 0x22, 0x22, 0x66, 0x20, 0x00, 0x00, 0x08, 0x88, 0x00, 0x77, 0x00, 0x00, 0x00, 0x11,
    0x55, 0x00, 0x00, 0x45, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x00, 0x00, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xff, 0x15, 0x55, 0x55,
    0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x40, 0x57, 0x40, 0x57, 0x55, 0x55, 0x75, 0x55,
    0xf5, 0x7f, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
};

/* Walk the instruction at p, with n bytes of it known. Returns the field of byte
 * p[n], or CM_F_NEW once the instruction is complete. It also sets the opcode
 * and ModRM, the length (*len, at least 1, or the length so far if the walk is
 * cut short), and where a rel32 or RIP-relative disp32 sits (*rel, or -1). It
 * only reads prefixes, opcodes and ModRM/SIB, never a displacement or immediate,
 * so it walks the same instructions before and after cm_x86t has rewritten
 * them. */
static int cm_xfield(const cm_u8 *p, int n, cm_u32 *opc, cm_u32 *mrm, int *rel, int *len) {
    int i = 0, w = 0, z = 4, m = 0, d = 0, im, e;
    *rel = -1; *opc = 0; *mrm = 0; *len = n > 0 ? n : 1;
    while (i < n && i < 14 && cm_xt[p[i] >> 1] >> (p[i] & 1) * 4 & 8) {   /* prefixes, REX */
        w |= (p[i] & 0xf8) == 0x48;
        if (p[i] == 0x66) z = 2;
        ++i;
    }
    if (i >= n) return CM_F_OP;
    cm_u32 o = p[i++];
    if (o == 0x0f) {
        if (i >= n) return CM_F_OP2;
        o = p[i++];
        e = cm_xt[128 + (o >> 2)] >> (o & 3) * 2 & 3;
        im = e >> 1;
        if (o == 0x38 || o == 0x3a) {           /* three-byte maps: ModRM, imm8 for 0F 3A */
            if (i >= n) return CM_F_OP2;
            im = o & 2 ? 1 : 0;
            o = (o + 0xc8) << 8 | p[i++];
            e = 1;
        } else {
            if ((o & 0xf0) == 0x80) im = 4;     /* jcc rel32 */
            o |= 0x100;
        }
    } else {
        e = cm_xt[o >> 1] >> (o & 1) * 4;
        im = e >> 1 & 3;
        if (im == 2) im = z;
    }
    *opc = o;
    if (e & 1) {
        if (i >= n) return CM_F_MODRM;
        m = p[i++];
        *mrm = (cm_u32)m;
        int mod = m >> 6, rm = m & 7;
        if (mod != 3 && rm == 4) {
            if (i >= n) return CM_F_SIB;
            if (mod == 0 && (p[i] & 7) == 5) d = 4;
            ++i;
        }
        if (mod == 1) d = 1;
        else if (mod == 2) d = 4;
        else if (mod == 0 && rm == 5) { d = 4; *rel = i; }   /* [rip + disp32] */
    }
    if (im == 3)   /* the specials: mov r, imm32/64; call/jmp rel32; enter; ret imm16; moffs; test imm */
        im = o >= 0xb8 && o <= 0xbf ? (w ? 8 : z) : o == 0xe8 || o == 0xe9 ? (*rel = i + d, 4) : o == 0xc8 ? 3 :
             o == 0xc2 || o == 0xca ? 2 : o < 0xa4 ? 8 : (m >> 3 & 7) < 2 ? (o == 0xf6 ? 1 : z) : 0;
    *len = i + d + im;
    if (n < i + d) return CM_F_DISP;
    if (n < i + d + im) return CM_F_IMM;
    return CM_F_NEW;
}

/* The x86 transform over the code makes rel32 call/jump targets and
 * RIP-relative disp32 absolute (adding the offset of the instruction's end), so
 * repeated calls to one function and loads of one constant look alike. The
 * decoder runs it with enc = 0 to undo it. Instructions are walked from CM_CS
 * while they start before CM_CE and their 15-byte window lies inside the
 * buffer. A field is rewritten only when it lies inside its instruction, so the
 * walk itself never changes. */
static void cm_x86t(cm_u8 *b, cm_u32 n, int enc) {
    for (cm_u32 i = CM_CS; i < CM_CE && i + 15 < n;) {
        cm_u32 o, m;
        int r, l;
        cm_xfield(b + i, 15, &o, &m, &r, &l);
        if (r >= 0 && r + 4 <= l) {
            cm_u8 *q = b + i + r;
            cm_u32 a = q[0] | (cm_u32)q[1] << 8 | (cm_u32)q[2] << 16 | (cm_u32)q[3] << 24;
            a = enc ? a + i + (cm_u32)l : a - (i + (cm_u32)l);
            q[0] = (cm_u8)a; q[1] = (cm_u8)(a >> 8); q[2] = (cm_u8)(a >> 16); q[3] = (cm_u8)(a >> 24);
        }
        i += (cm_u32)l;
    }
}

/* ---- the model ---- */

static void cm_ctx(void);
static void cm_init(cm_u8 *buf) {
    cm_buf = buf;
    /* squash(d) = 4096 / (1 + e^(-d/256)), via an integer e^(-1/256) recurrence */
    unsigned long long v = 1ull << 31;
    for (int d = 0; d < 2048; ++d) {
        int p = (int)((4096ull << 31) / ((1ull << 31) + v));
        cm_sq[2048 + d] = p > 4095 ? 4095 : p;
        cm_sq[2048 - d] = 4096 - cm_sq[2048 + d];
        v = v * 4278222805ull >> 32;
    }
    cm_sq[0] = 1;
    int pi = 0;
    for (int x = -2047; x <= 2047; ++x)
        for (int i = cm_sq[x + 2048]; pi <= i; ++pi) cm_st[pi] = x;
    for (; pi < 4096; ++pi) cm_st[pi] = 2047;
    for (int i = 0; i < 16; ++i) cm_dt[i] = 16384 / (i + i + 3);
    for (int i = 0; i < CM_WS * CM_NI; ++i) cm_w[i] = CM_WI << 10;
    for (int i = 0; i < 11 * 256 * CM_NM; ++i) cm_w2[i] = 65536 / CM_NM;
    for (int i = 0; i < (256 + 65536) * 33; ++i) {   /* each APM bucket j starts at squash((j - 16) * 128) */
        int j = i % 33 * 128;
        cm_a[i] = (cm_u16)(cm_sq[j < 4096 ? j : 4095] * 16);
    }
    cm_ctx();
}

/* A counter slot: probability in the top 12 bits, hit count in the low 4, stored
 * XOR 1<<15 so the zeroed .bss starts every slot at p = 1/2. The step is rounded
 * to nearest, because truncating it would let a counter reach 0 but stop 14/4096
 * short of 1, biasing the model toward zeros. It cannot overflow: a step of 1 at
 * p = 4095 would need the first update's rate, and the first update starts from
 * 2048. */
static int cm_p(cm_u16 s) { return (s ^ 0x8000) >> 4; }
static void cm_upd(cm_u16 *s, int y) {
    int v = *s ^ 0x8000, n = v & 15, p = v >> 4;
    p += (((y << 12) - p) * cm_dt[n] + 4096) >> 13;
    *s = (cm_u16)((p << 4 | (n < CM_LIM ? n + 1 : n)) ^ 0x8000);
}
static cm_u32 cm_hash(cm_u32 x) { x *= 0x9E3779B1u; x ^= x >> 15; x *= 0x2C1B3C6Du; return x ^ x >> 13; }
static int cm_clamp(int d) { return d < -2047 ? -2047 : d > 2047 ? 2047 : d; }

/* An APM: refine p in context cx. Each context has 33 buckets, spaced 128 apart
 * in the logistic domain. Interpolate between the two that bracket stretch(p),
 * and both of them learn the bit. */
static int cm_apm(int k, cm_u32 cx, int p) {
    int s = cm_st[p] + 2048, w = s & 127, i = cm_aj[k] = (int)cx * 33 + (s >> 7);
    return (cm_a[i] * (128 - w) + cm_a[i + 1] * w) >> 11;
}

/* P(next bit = 1), 12 bits */
static int cm_predict(void) {
    int n = 0;
    for (int i = 0; i < CM_NE; ++i) {
        cm_ix[i] = cm_base[i] + (cm_u32)cm_nib;
        cm_in[n++] = cm_st[cm_p(cm_t[cm_ix[i]])];
    }
    cm_mi = 0;
    if (cm_mlen && ((cm_buf[cm_mptr] + 256) >> (8 - cm_bit)) == cm_c0)
        cm_mi = (cm_mlen > 15 ? 15 : cm_mlen) * 2 + (cm_buf[cm_mptr] >> (7 - cm_bit) & 1);
    cm_in[n++] = cm_mi ? cm_st[cm_p(cm_msm[cm_mi])] : 0;
    cm_in[n] = 256;
    /* mixer 0 keys on the match length, the x86 field (out of the code, the last
     * byte's class) and the byte so far. The second layer keys on the last two
     * of those. Keying it on the byte so far alone made the stream longer. */
    int fc = (cm_xf + (cm_xf == CM_F_DATA ? (int)(cm_cls & 3) : 0)) * 256 + cm_c0;
    cm_ms[0] = ((cm_mlen ? (cm_mlen > 15 ? 15 : cm_mlen) / 4 + 1 : 0) * 11 * 256 + fc) * CM_NI;
    cm_s2 = fc * CM_NM;
    /* mixers 1-4 key on one of the last bytes and the bit's position only.
     * Keying them on the byte so far as well left each weight set too few bits
     * to learn from, and made the stream longer. Mixer 5 keys on the opcode, or
     * out of the code on the classes of the last three bytes, whether the
     * statement's first word is known, and whether a bracket is open. */
    for (int m = 1; m < CM_NM; ++m) {
        int s = m < 5 ? (int)(cm_c4 >> (m - 1) * 8 & 255) :
                cm_xf == CM_F_DATA ? 1024 + (int)(cm_cls & 63) * 4 + (cm_sw != 0) * 2 + (cm_dep != 0) : (int)(cm_xo & 1023);
        cm_ms[m] = (5 * 11 * 256 + ((m - 1) * 256 + s) * 8 + cm_bit) * CM_NI;
    }
    int dot2 = 0;
    for (int m = 0; m < CM_NM; ++m) {
        int dot = 0;
        for (int j = 0; j < CM_NI; ++j) dot += cm_in[j] * (cm_w[cm_ms[m] + j] >> 8);
        int d = cm_md[m] = cm_clamp(dot >> 8);
        cm_mp[m] = cm_sq[d + 2048];
        dot2 += d * (cm_w2[cm_s2 + m] >> 8);
    }
    int p = cm_pm = cm_sq[cm_clamp(dot2 >> 8) + 2048];
#if CM_APM
    p = (p + cm_apm(0, (cm_u32)cm_c0, p) + 2 * cm_apm(1, 256 + (cm_u32)cm_c0 + (cm_c4 & 255) * 256, p)) >> 2;
#endif
    return p < 1 ? 1 : p > 4095 ? 4095 : p;
}

/* At a nibble boundary, pick each model's 16-slot block (32 bytes). Of a
 * neighbouring pair, take the one whose check (slot 0) matches, or else the less
 * used one by its first counter's hit count, and clear it for this context. */
static void cm_nibble(void) {
    for (int i = 0; i < CM_NE; ++i) {
        cm_u32 h = cm_hash(cm_cx[i] + (cm_u32)cm_c0 * 0x1F3D5B79u), b = h >> (32 - CM_TB) & ~15u, b2 = b ^ 16;
        cm_u16 c = (cm_u16)(h | 1);
        if (cm_t[b] != c) {
            if (cm_t[b2] == c) b = b2;
            else {
                if (((cm_t[b2 + 1] ^ 0x8000) & 15) < ((cm_t[b + 1] ^ 0x8000) & 15)) b = b2;
                for (int j = 0; j < 16; ++j) cm_t[b + j] = 0;
                cm_t[b] = c;
            }
        }
        cm_base[i] = b;
    }
    cm_nib = 1;
}

/* At a byte boundary: hash every model's context. */
static void cm_ctx(void) {
    int k = 0;
    for (int i = 0; i < CM_NK; ++i) {
        cm_u32 lo = 0, hi = 0;
        for (int j = 0; j < 4; ++j) {
            if (cm_mask[i] >> j & 1) lo |= cm_c4 & 255u << 8 * j;
            if (cm_mask[i] >> (j + 4) & 1) hi |= cm_c8 & 255u << 8 * j;
        }
        cm_cx[k++] = cm_hash(lo ^ cm_hash(hi + cm_mask[i] * 0x3C6EF372u));
    }
    cm_u32 b1 = cm_c4 & 255, b2 = cm_c4 & 0xffff;
    cm_u32 x[4] = { cm_word ^ b1 << 24, b1 | (cm_pos & 3) << 8, b1 | cm_i1[b1] << 8, b2 | cm_i2[b2] << 16 };
    for (int i = 0; i < 4; ++i)
        if (CM_XSET >> i & 1) cm_cx[k++] = cm_hash(x[i] + (cm_u32)(i + 1) * 0x5851F42Du);
    /* the x86 models. The first six are: the field; the field, byte index and
     * opcode; the instruction so far together with the opcodes before it; the
     * field, byte index and last byte; the same with the instruction before;
     * and the instruction so far with the last instruction that had this opcode.
     * The seventh switches on the field. A displacement or immediate byte keys
     * on the last two RIP-relative (or call) targets, with the field and byte
     * index. These are the absolute addresses cm_x86t made, so a run of loads
     * from one table, or calls to one function, predicts the next. A ModRM byte
     * keys on the opcode with the last instruction's opcode and ModRM. Any other
     * byte keys on the instruction so far with the last opcode. */
    cm_u32 f = (cm_u32)cm_xf, ix = cm_pos - cm_xs, ins = f, fi = f | ix << 3, fb = fi | b1 << 8;
    for (cm_u32 j = cm_xs; j < cm_pos && f < CM_F_DATA; ++j) ins = cm_hash(ins + cm_buf[j]);
    cm_u32 y[7] = { f, fi | cm_xo << 8, ins ^ cm_xh * 0x9E3779B1u, fb, cm_pi * 3 + fb,
                    cm_xl[cm_xo & 1023] * 3 + ins,
                    f == CM_F_MODRM ? cm_xo << 16 ^ cm_xp : f >= CM_F_DISP ? (cm_rt * 3 + cm_rt2) * 5 + fi :
                    ins * 5 + (cm_xp >> 8) };
    if (f == CM_F_DATA) {   /* outside the code (the GLSL, the tables) they see the text's shape instead: */
        cm_u32 g = cm_fc[cm_dep & 255];
        y[0] = g;                                   /* the call and argument the byte is in */
        y[1] = cm_sw * 3 + (cm_dep << 8 | b1);      /* the statement's first word, the depth, the last byte */
        y[2] = cm_bd << 16 | cm_dep << 8 | b1;      /* brace and bracket depth, the last byte */
        y[3] = (cm_c4 & 0xffffff) ^ cm_dep << 24;   /* order 3 and the depth */
        y[4] = g * 3 << 8 | b1;                     /* the call and argument, the last byte */
        y[5] = cm_cls;                              /* the classes (letter, digit, other printable, other) of 16 bytes */
        y[6] = (cm_psw * 3 + cm_sw) * 5 + cm_word + cm_bd * 11;   /* the first words of this statement and the one
                                                                  * before, the word so far, the brace depth */
    }
    for (int i = 0; i < 7; ++i) cm_cx[k++] = cm_hash(y[i] + (cm_u32)(i + 6) * 0x6C8E9CF5u);
    cm_nibble();
}

/* Learn from the coded bit, and at a byte boundary roll the contexts forward. */
static void cm_update(int y) {
    for (int i = 0; i < CM_NE; ++i) cm_upd(&cm_t[cm_ix[i]], y);
    if (cm_mi) cm_upd(&cm_msm[cm_mi], y);
#if CM_APM
    for (int k = 0; k < 4; ++k) {
        cm_u16 *a = &cm_a[cm_aj[k >> 1] + (k & 1)];
        *a += ((y << 16) - *a - y) >> CM_AR;
    }
#endif
    /* the first layer learns faster over the first 4 << CM_WARM bytes */
    int lr = CM_LR - (CM_WARM && cm_pos < 1u << CM_WARM) - (CM_WARM && cm_pos < 4u << CM_WARM);
    for (int m = 0; m < CM_NM; ++m) {
        int err = (y << 12) - cm_mp[m];
        for (int j = 0; j < CM_NI; ++j) cm_w[cm_ms[m] + j] += (cm_in[j] * err) >> lr;
        cm_w2[cm_s2 + m] += (cm_md[m] * ((y << 12) - cm_pm)) >> CM_LR2;
    }
    cm_c0 = cm_c0 * 2 + y;
    cm_nib = cm_nib * 2 + y;
    if (++cm_bit == 8) {
        cm_u32 c = (cm_u32)cm_c0 & 255;
        int al = (c | 32) - 'a' < 26u || c == '_', dg = c - '0' < 10u;
        cm_buf[cm_pos++] = (cm_u8)c;
        cm_i1[cm_c4 & 255] = cm_i1[cm_c4 & 255] << 8 | c;
        cm_i2[cm_c4 & 0xffff] = cm_i2[cm_c4 & 0xffff] << 8 | c;
        cm_c8 = cm_c8 << 8 | cm_c4 >> 24;
        cm_c4 = cm_c4 << 8 | c;
        cm_cls = cm_cls << 2 | (al ? 1u : dg ? 2u : c - 32 < 96u ? 3u : 0u);
        /* the GLSL's shape. A bracket notes the word before it (the function
         * called, or 0), and each comma bumps that entry. A statement's first
         * word is the first word that ends after a ; { or }. A NUL ends the
         * statement and zeroes both depths. There is one between shaders and
         * one before the text, and since brackets balance, nothing else resets
         * the depth. The GLSL is about half the stream, and these contexts code
         * it noticeably smaller than the classes of 8 bytes, the word and
         * orders 0-1 did. */
        if (c == '(') cm_fc[++cm_dep & 255] = cm_word;
        if (c == ')') --cm_dep;
        if (c == ',') ++cm_fc[cm_dep & 255];
        if (c == '{') ++cm_bd;
        if (c == '}') --cm_bd;
        if (c == ';' || c == '{' || c == '}' || !c) {
            if (cm_sw) cm_psw = cm_sw;
            cm_sw = 0;
        }
        else if (!cm_sw && !(al || dg)) cm_sw = cm_word;
        if (!c) cm_bd = cm_dep = 0;
        cm_word = al || dg ? (cm_word + c + 1) * 0x2F0F1A3Bu : 0;
        /* match model: extend the current match, or look the last 8 bytes up
         * and take what they find if it agrees on CM_MM bytes. A longer minimum
         * match helps, the text most of all: short matches in minified GLSL
         * mislead more often than they help. */
        if (cm_mlen && cm_buf[cm_mptr] == c) { ++cm_mlen; ++cm_mptr; } else cm_mlen = 0;
        if (cm_pos >= 6) {
            cm_u32 h = cm_hash(cm_c4 ^ cm_hash(cm_c8)) >> (32 - CM_MB);
            if (!cm_mlen && cm_mt[h]) {
                int l = 0;
                cm_mptr = (int)cm_mt[h];
                while (l < 32 && l < cm_mptr && cm_buf[cm_mptr - 1 - l] == cm_buf[cm_pos - 1 - (cm_u32)l]) ++l;
                cm_mlen = l >= CM_MM ? l : 0;
            }
            cm_mt[h] = cm_pos;
        }
        /* the x86 walk: where in its instruction the next byte falls (up to 15
         * bytes an instruction; longer runs are data and restart the walk) */
        if (cm_pos >= CM_CS && cm_pos < CM_CE) {
            int r, l;
            cm_u32 m;
            if (cm_pos == CM_CS) cm_xs = cm_pos;
            cm_xf = cm_xfield(cm_buf + cm_xs, (int)(cm_pos - cm_xs), &cm_xo, &m, &r, &l);
            if (cm_xf == CM_F_NEW || cm_pos - cm_xs >= 15) {
                cm_u32 h = 0;
                for (cm_u32 j = cm_xs; j < cm_pos; ++j) h = cm_hash(h + cm_buf[j]);
                cm_xl[cm_xo & 1023] = cm_pi = h;
                cm_xh = (cm_xh << 10 | cm_xo) & 0x3ffff;
                cm_xp = cm_xo << 8 | m;
                if (cm_xf == CM_F_NEW && r >= 0) {   /* a RIP-relative or call target (complete, not a walk cut short) */
                    const cm_u8 *q = cm_buf + cm_xs + r;
                    cm_rt2 = cm_rt;
                    cm_rt = q[0] | (cm_u32)q[1] << 8 | (cm_u32)q[2] << 16 | (cm_u32)q[3] << 24;
                }
                cm_xs = cm_pos; cm_xf = CM_F_OP; cm_xo = 0;
            }
        } else cm_xf = CM_F_DATA;
        cm_c0 = 1; cm_bit = 0;
        cm_ctx();
    } else if (cm_bit == 4) cm_nibble();
}

/* The coder's split point for a bit with P(1) = p/4096. */
static cm_u32 cm_mid(int p) { return cm_x1 + ((cm_x2 - cm_x1) >> 12) * (cm_u32)p; }

#ifndef CM_ENCODER
/* Decode n bytes from src[0..len) into out (bytes past the end of src read as
 * 0xff, which is what the encoder's one-byte flush assumes). */
static const cm_u8 *cm_src;
static cm_u32 cm_sl, cm_sp, cm_x;
static cm_u32 cm_in8(void) { return cm_sp < cm_sl ? cm_src[cm_sp++] : 255; }
static void cm_decode(const cm_u8 *src, cm_u32 len, cm_u8 *out, cm_u32 n) {
    cm_src = src; cm_sl = len;
    cm_init(out);
    for (int i = 0; i < 4; ++i) cm_x = cm_x << 8 | cm_in8();
    for (cm_u32 i = 0; i < n * 8; ++i) {
        cm_u32 xm = cm_mid(cm_predict());
        int y = cm_x <= xm;
        if (y) cm_x2 = xm; else cm_x1 = xm + 1;
        cm_update(y);
        while (((cm_x1 ^ cm_x2) & 0xff000000u) == 0) {
            cm_x1 <<= 8; cm_x2 = cm_x2 << 8 | 255; cm_x = cm_x << 8 | cm_in8();
        }
    }
    cm_x86t(out, n, 0);
}
#endif

#endif /* MICROCOSM_CM_H */
