/*
 * mcstub.c - MICROCOSM self-extracting ELF launcher (freestanding, no libc):
 * its second stage.
 *
 * The launcher is an ordinary ELF executable that carries the demo ELF as an
 * embedded payload, coded with the context-mixing model in tools/cm.h.
 * tools/pack.py searches the model configuration at build time and bakes the
 * winner in through payload_meta.h. The launcher decodes the payload in RAM and
 * runs it straight from an anonymous in-memory file with memfd_create and
 * execveat. Nothing is written to disk, and no shell, external command or temp
 * file is involved.
 *
 * It comes in two stages. The second is that decoder and the memfd glue, linked
 * to run at S2_ADDR (tools/stage2.ld). The file carries it coded for the much
 * smaller first stage, tools/s1dec.S, which is the ELF header plus a
 * hand-written decoder for tools/s1.h. The first stage decodes the second into
 * place and jumps to it with the payload. The second stage is a few KB of x86
 * that the big model would otherwise carry raw, and the first stage's tiny
 * counting coder shrinks it by more than its own size.
 *
 * The shipped second stage is tools/s2dec.S, the cm.h decoder and the memfd glue
 * written by hand, which comes out smaller than the C once the first stage has
 * coded it. This file now has two other uses. First, it is the reference model
 * for the host verification pack.py runs before shipping. Built with
 * -DMC_DECODE_TEST it decodes a coded file to stdout using the same cm.h the
 * packer compressed with, so a wrong model, configuration or code range fails
 * the build. Built with -DMC_S2_TEST it links the asm decoder instead and
 * decodes the same way, so pack.py can check that the shipped decoder agrees
 * with the model and reproduces the payload. -DMC_S1_TEST, with s1dec.S, does
 * the same for the first stage. Second, built with -DMC_STAGE2 it is a reference
 * and fallback second stage in C, linked by stage2.ld the same way as the asm,
 * with the same entry and the same s2_mem_end. The pipeline no longer ships it,
 * but it documents the glue and stays byte-for-byte equivalent to the asm.
 *
 * It is static and freestanding, with four raw system calls (memfd_create,
 * write, execveat, exit) and no libraries. tools/launcher.ld links the whole as
 * a raw image of one RWX PT_LOAD, with no interpreter, dynamic section, notes or
 * unwind tables. The decoders' model tables (a few hundred MB, mostly the shared
 * context table) and the second stage itself live in memory past the file. They
 * cost no file bytes, and the kernel hands them back when execveat replaces the
 * process. memfd execution needs Linux >= 3.19 with memfd exec permitted, which
 * is the default. There is no temp-file fallback, which keeps the executable
 * small.
 */
#if defined MC_S1_TEST
/* The host test of the first stage (tools/s1dec.S, linked in): decode the coded
 * second stage it carries to stdout. */
#include <stdio.h>
#include <stdlib.h>
#include "s1_meta.h"
extern unsigned char s1_out[];
int main(void) {
    __asm__ volatile("call s1_dec" ::: "rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "r8", "r9", "r10",
                     "r11", "r12", "r13", "r14", "r15", "memory", "cc");
    fwrite(s1_out, 1, S1_LEN, stdout);
    return 0;
}
#elif defined MC_S2_TEST
/* The host test of the shipped second stage (tools/s2dec.S, linked in): decode a
 * coded file to stdout with the hand-written asm decoder, so pack.py can compare
 * it against the reference model and the original payload. */
#include <stdio.h>
#include <stdlib.h>
#include "payload_meta.h"
extern void s2_dec(const unsigned char *src, unsigned len, unsigned char *out, unsigned n);
static unsigned char g_out[MC_UNCOMP_LEN];
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);
    s2_dec(buf, (unsigned)sz, g_out, MC_UNCOMP_LEN);
    fwrite(g_out, 1, MC_UNCOMP_LEN, stdout);
    return 0;
}
#else
#include "payload_meta.h"  /* MC_UNCOMP_LEN and the CM_* configuration of the payload */
#include "cm.h"

static cm_u8 g_out[MC_UNCOMP_LEN];      /* .bss, no file bytes */

#ifdef MC_DECODE_TEST
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    cm_u8 *buf = (cm_u8 *)malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) return 2;
    fclose(f);
    cm_decode(buf, (cm_u32)sz, g_out, MC_UNCOMP_LEN);
    fwrite(g_out, 1, MC_UNCOMP_LEN, stdout);
    return 0;
}
#else
/* ---- the second stage: decode the payload and run it ---- */
static long mc_sys(long n, long a, long b, long c, long d, long e) {
    register long r10 __asm__("r10") = d, r8 __asm__("r8") = e;
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
                     : "rcx", "r11", "memory");
    return r;
}

/* The first stage jumps here, to S2_ADDR (tools/stage2.ld puts it first), with
 * the initial stack (argc, argv[], NULL, envp[]) and the coded payload, whose
 * length pack.py has already given us as MC_BLOB_LEN. */
__attribute__((noreturn, used, section(".text.entry"))) void s2_main(long *sp, const cm_u8 *blob) {
    char **argv = (char **)(sp + 1), **envp = argv + sp[0] + 1;
    cm_decode(blob, MC_BLOB_LEN, g_out, MC_UNCOMP_LEN);
    long fd = mc_sys(319, (long)"", 1, 0, 0, 0);                /* memfd_create("", MFD_CLOEXEC) */
    for (long w = 0, r; w < MC_UNCOMP_LEN; w += r)               /* (a bad fd fails the write) */
        if ((r = mc_sys(1, fd, (long)(g_out + w), MC_UNCOMP_LEN - w, 0, 0)) <= 0) goto fail;
    mc_sys(322, fd, (long)"", (long)argv, (long)envp, 0x1000);  /* execveat(AT_EMPTY_PATH) */
fail:
    for (;;) mc_sys(60, 1, 0, 0, 0, 0);                          /* only reached on failure */
}
#endif
#endif
