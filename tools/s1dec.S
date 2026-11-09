/*
 * s1dec.S - the launcher's first stage: its ELF header, then the decoder of
 * tools/s1.h written by hand. The C version in s1.h, which the packer runs, is
 * the reference, and each step below follows one of its lines. The decoder puts
 * the second stage at S2_ADDR, and the header code then jumps there with the
 * initial stack and the coded payload. Built with -DMC_S1_TEST it is just the
 * decoder, a function the host test calls.
 *
 * s1_meta.h (written by tools/s1pack.c and pack.py) gives the models (S1_NM,
 * S1_MASKS, S1_WTS), the second stage's length (S1_LEN) and the coded files.
 * Everything is absolute: the image sits at 0x400000 and the tables in .bss.
 */
#include "s1_meta.h"
#define S1_TB 20                /* tools/s1.h */
#ifdef MC_S1_TEST
#define S2_ADDR s1_out          /* the host test's buffer */
#else
#define S2_ADDR 0x800000        /* tools/stage2.ld */
#endif

#ifndef MC_S1_TEST
/* The ELF header (64 bytes) and its one program header (56 bytes) overlap by 8.
 * e_phnum = 1 and e_shentsize = 0 read as p_type = PT_LOAD, and e_shnum = 7 and
 * e_shstrndx = 0 read as p_flags = RWX. There are no section headers, so e_shoff
 * is 0. The sizes come from tools/launcher.ld. The entry code sits in header
 * fields Linux never reads: e_shoff, e_flags and e_ehsize (A), p_paddr (B), and
 * e_ident's padding (C). Execution runs A, then B, then C. The initial stack
 * holds argc, argv[], NULL and envp[], and the ABI promises it is 16-byte
 * aligned at entry. The code puts its address in rdi, decodes the second stage
 * and jumps to it as s2_main(sp, blob), leaving the stack as a call would. The
 * payload's coded length is the second stage's own constant. */
        .section .elfhdr, "awx", @progbits
        .byte 0x7f, 0x45, 0x4c, 0x46, 2, 1, 1, 0   /* e_ident: ELF64, LSB, v1 */
.Lc:    jmp S2_ADDR                     /* C */
        .org 0x10
        .short 2, 0x3e                  /* e_type EXEC, e_machine x86-64 */
        .long 1                         /* e_version */
        .quad _start, 0x38              /* e_entry, e_phoff */
        .globl _start
_start: mov %rsp, %rdi                  /* A */
        push %rdi
        push %rdi
        call s1_dec
        jmp .Lb
        .org 0x36
        .short 0x38                     /* e_phentsize */
        .short 1, 0                     /* e_phnum, e_shentsize = p_type */
        .long 7                         /* e_shnum, e_shstrndx = p_flags */
        .quad 0, 0x400000               /* p_offset, p_vaddr */
.Lb:    pop %rdi                        /* B */
        mov $mcp_blob, %esi
        jmp .Lc
        .quad mc_file_size, mc_mem_size, 0x1000   /* sizes, align */
#endif

        .text

/* Decode the second stage into S2_ADDR (s1_decode): rsi the coded bytes, rdi
 * the output (the four bytes before it, zeros at first, are the context), ebx
 * and ebp the coder's low and high ends, r12d its code value, r14d the bits of
 * this byte so far, ecx the model. */
        .globl s1_dec
s1_dec:
        mov $s1_blob, %esi
        mov $S2_ADDR, %edi
        xor %ebx, %ebx
        xor %ebp, %ebp
        xor %r12d, %r12d
.Lbyte: xor %ecx, %ecx                  /* s1_ctx: every model's context */
1:      mov s1_mk(,%rcx,4), %eax
        and -4(%rdi), %eax
        add %ecx, %eax
        imul $0x9E3779B1, %eax, %eax
        mov %eax, s1_h(,%rcx,4)
        inc %ecx
        cmp $S1_NM, %ecx
        jne 1b
        push $1
        pop %r14
.Lbit:  mov %ebx, %eax                  /* renormalize: shift out the settled top byte */
        xor %ebp, %eax
        cmp $0xffffff, %eax
        ja 2f
        shl $8, %ebx
        shl $8, %ebp
        or $0xff, %bpl
        shl $8, %r12d
        lodsb
        mov %al, %r12b
        jmp .Lbit
2:      xor %r8d, %r8d                  /* s1_p: n1 */
        xor %r9d, %r9d                  /* nn */
        xor %ecx, %ecx
3:      mov s1_h(,%rcx,4), %eax
        add %r14d, %eax
        imul $0x2C1B3C6D, %eax, %eax
        shr $(32 - S1_TB), %eax
        lea s1_t(%rax,%rax), %rdx       /* q */
        mov %rdx, s1_q(,%rcx,8)
        movzbl (%rdx), %r10d
        movzbl 1(%rdx), %r11d
        mov %r10d, %eax                 /* the weight, doubled while a count is 0 */
        imul %r11d, %eax
        cmp $1, %eax
        movzbl s1_wk(%rcx), %eax
        adc $0, %eax
        xor %edx, %edx
        bts %eax, %edx
        imul %edx, %r11d
        imul %edx, %r10d
        add %r11, %r8
        add %r11, %r9
        add %r10, %r9
        inc %ecx
        cmp $S1_NM, %ecx
        jne 3b
        lea 1(%r8,%r8), %rax            /* p = ((2 n1 + 1) << 16) / (2 nn + 2), at least 1 */
        shl $16, %rax
        lea 2(%r9,%r9), %rcx
        xor %edx, %edx
        div %rcx
        cmp $1, %eax
        adc $0, %eax
        mov %ebp, %edx                  /* the split */
        sub %ebx, %edx
        shr $16, %edx
        imul %eax, %edx
        add %ebx, %edx
        xor %eax, %eax                  /* y = x <= split */
        cmp %edx, %r12d
        setbe %al
        ja 4f
        mov %edx, %ebp
        jmp 5f
4:      lea 1(%rdx), %ebx
5:      xor %ecx, %ecx                  /* s1_upd */
6:      mov s1_q(,%rcx,8), %rdx
        cmpb $0xff, (%rdx,%rax)
        adcb $0, (%rdx,%rax)
        xor $1, %eax
        cmpb $2, (%rdx,%rax)
        jbe 7f
        shrb (%rdx,%rax)
        incb (%rdx,%rax)
7:      xor $1, %eax
        inc %ecx
        cmp $S1_NM, %ecx
        jne 6b
        lea (%rax,%r14,2), %r14d
        cmp $256, %r14d
        jb .Lbit
        xchg %eax, %r14d                /* the byte */
        stosb
        cmp $(S2_ADDR + S1_LEN), %edi
        jne .Lbyte
        ret

/* The models and the coded streams follow the code directly, with no padding or
 * terminators. The payload's length is a link-time constant. */
s1_mk:  .long S1_MASKS
s1_wk:  .byte S1_WTS
s1_blob: .incbin MC_S1_BLOB
#ifndef MC_S1_TEST
mcp_blob: .incbin MC_BLOB
#endif
        .lcomm s1_h, 4 * S1_NM
        .lcomm s1_q, 8 * S1_NM
        .lcomm s1_t, 2 << S1_TB
#ifdef MC_S1_TEST
        .lcomm s1_pad, 4 + S1_LEN       /* zeros before the output, as in the launcher */
        .globl s1_out
        .set s1_out, s1_pad + 4
#endif
