/*
 * s2dec.S - MICROCOSM's launcher second stage, written by hand: the
 * context-mixing decoder of tools/cm.h, plus the memfd glue that runs the
 * decoded payload. The C version in cm.h, which the packer runs through
 * cmpack.c, is the reference, and each step below follows one of its lines. It
 * replaces the -Oz C of tools/mcstub.c built with -DMC_STAGE2, because it comes
 * out smaller once the first stage (tools/s1dec.S) has coded it, and the coded
 * form is what the file carries.
 *
 * The configuration is compile-time, as in cm.h. payload_meta.h (written by
 * pack.py) gives the masks, the extra-model set, the counter limit, the mixer
 * rates and count, the APM and warm-up knobs, the code range and the payload's
 * lengths. The mask count and the model, input and weight counts are derived
 * from those here, so the same loops serve every configuration pack.py can
 * choose. All model tables are .bss, so they cost no file bytes.
 *
 * Built with -DMC_S2_TEST it is instead a function s2_dec(src, len, out, n) that
 * the host test calls (tools/mcstub.c with -DMC_S2_TEST). This lets pack.py
 * check, before shipping, that the shipped decoder reproduces the payload and
 * agrees with the C model, as -DMC_S1_TEST does for the first stage.
 *
 * Registers: only XF (cm_xfield, which UP and X86T call with live registers)
 * saves the callee-saved ones, and no other routine saves any. Elsewhere a value
 * kept across a call sits in a register the callee doesn't touch, and the driver
 * keeps its state in memory, looping until the model holds all n bytes. The
 * host test entry saves the C ABI's registers for its caller.
 */
#include "payload_meta.h"

/* ---- the configuration, derived as in cm.h ---- */
#ifndef CM_TB
#define CM_TB 26
#endif
#ifndef CM_MM
#define CM_MM 12
#endif
#ifndef CM_WI
#define CM_WI 12
#endif
#define CM_MB 22
#define F_MODRM 2
#define F_SIB 3
#define F_DISP 4
#define F_IMM 5
#define F_NEW 6
#define F_DATA 7

        .section .rodata
cm_mask:.byte CM_MASKS                  /* the byte masks: one context model each */
        .set NK, . - cm_mask            /* CM_NK */
        /* popcount of CM_XSET's low 4 bits */
        .set XP, (CM_XSET&1)+((CM_XSET>>1)&1)+((CM_XSET>>2)&1)+((CM_XSET>>3)&1)
        .set NE, NK + XP + 7            /* CM_NE: masks, extras, x86 (7; the text's shape outside the code) */
        .set NI, NE + 2                 /* CM_NI: models, match, bias */
        .set WS, 5*11*256 + (CM_NM-1)*2048 + CM_NM/6*8192   /* CM_WS (CM_NM <= 6; GAS's > is -1) */

/* Opcode attributes (cm.h cm_xt): one-byte opcodes 4 bits each (bit 0 ModRM,
 * bits 1-2 immediate, bit 3 prefix), then the 0F opcodes 2 bits each. */
cm_xt:  .byte 0x11, 0x11, 0x42, 0x00, 0x11, 0x11, 0x42, 0x00, 0x11, 0x11, 0x42, 0x00, 0x11, 0x11, 0x42, 0x00
        .byte 0x11, 0x11, 0x42, 0x08, 0x11, 0x11, 0x42, 0x08, 0x11, 0x11, 0x42, 0x08, 0x11, 0x11, 0x42, 0x08
        .byte 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        .byte 0x00, 0x11, 0x88, 0x88, 0x54, 0x32, 0x00, 0x00, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22
        .byte 0x53, 0x33, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        .byte 0x66, 0x66, 0x00, 0x00, 0x42, 0x00, 0x00, 0x00, 0x22, 0x22, 0x22, 0x22, 0x66, 0x66, 0x66, 0x66
        .byte 0x33, 0x06, 0x11, 0x53, 0x06, 0x06, 0x20, 0x00, 0x11, 0x11, 0x00, 0x00, 0x11, 0x11, 0x11, 0x11
        .byte 0x22, 0x22, 0x22, 0x22, 0x66, 0x20, 0x00, 0x00, 0x08, 0x88, 0x00, 0x77, 0x00, 0x00, 0x00, 0x11
        .byte 0x55, 0x00, 0x00, 0x45, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x00, 0x00, 0x55, 0x55
        .byte 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0xff, 0x15, 0x55, 0x55
        .byte 0x00, 0x00, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x40, 0x57, 0x40, 0x57, 0x55, 0x55, 0x75, 0x55
        .byte 0xf5, 0x7f, 0x00, 0x00, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55

        .text

/* cm_hash: eax -> eax (clobbers edx). */
H:      imul $0x9E3779B1, %eax, %eax
        mov %eax, %edx
        shr $15, %edx
        xor %edx, %eax
        imul $0x2C1B3C6D, %eax, %eax
        mov %eax, %edx
        shr $13, %edx
        xor %edx, %eax
        ret

/* cm_upd(rdi -> a u16 slot, eax = y): adapt the counter toward y. */
U:      movzwl (%rdi), %edx
        xor $0x8000, %edx               /* v */
        mov %edx, %ecx
        and $15, %ecx                   /* n */
        shr $4, %edx                    /* p */
        mov %eax, %esi
        shl $12, %esi
        sub %edx, %esi                  /* (y<<12) - p */
        imul cm_dt(,%rcx,4), %esi
        add $4096, %esi                 /* rounded to nearest */
        sar $13, %esi
        add %esi, %edx                  /* p += ... */
        cmp $CM_LIM, %ecx               /* n < CM_LIM ? n+1 : n */
        jge 1f
        inc %ecx
1:      shl $4, %edx
        or %ecx, %edx
        xor $0x8000, %edx
        mov %dx, (%rdi)
        ret

#if CM_APM
/* cm_apm(k in r8d, cx in eax, p in esi) -> refined p in eax; records the lower
 * bucket in cm_aj[k]. Context cx has 33 buckets, spaced 128 apart in the
 * logistic domain; this interpolates between the two that bracket stretch(p). */
A:      mov cm_st(,%rsi,4), %ecx
        add $2048, %ecx                 /* s = st[p] + 2048 */
        mov %ecx, %edi
        and $127, %edi                  /* w = s & 127 */
        sar $7, %ecx                    /* s >> 7 */
        imul $33, %eax, %eax
        add %ecx, %eax                  /* i = cx*33 + (s>>7) */
        mov %eax, cm_aj(,%r8,4)
        movzwl cm_a(,%rax,2), %ecx      /* a[i] */
        movzwl cm_a+2(,%rax,2), %eax    /* a[i+1] */
        imul %edi, %eax                 /* a[i+1]*w */
        mov $128, %edx
        sub %edi, %edx
        imul %edx, %ecx                 /* a[i]*(128-w) */
        add %ecx, %eax
        shr $11, %eax
        ret
#endif

/* ---- cm_predict: P(next bit = 1), 12 bits, left in eax ---- */
P:      /* for i in 0..NE-1: ix[i]=base[i]+nib; in[i]=st[cm_p(t[ix[i]])] */
        xor %ebx, %ebx
.Ppre:  mov cm_base(,%rbx,4), %eax
        add nib, %eax
        mov %eax, cm_ix(,%rbx,4)
        movzwl cm_t(,%rax,2), %edx
        xor $0x8000, %edx
        shr $4, %edx
        mov cm_st(,%rdx,4), %eax
        mov %eax, cm_in(,%rbx,4)
        inc %ebx
        cmp $NE, %ebx
        jne .Ppre
        /* match input: cm_mi */
        movl $0, mi
        mov mlen, %eax
        test %eax, %eax
        jz .Pmi0
        mov obuf, %r9
        mov mptr, %r8d
        movzbl (%r9,%r8), %r10d          /* buf[mptr] */
        lea 256(%r10), %edx
        mov $8, %ecx
        sub bit, %ecx                    /* 8 - bit */
        shr %cl, %edx
        cmp c0, %edx
        jne .Pmi0
        mov mlen, %eax                   /* (mlen>15?15:mlen)*2 */
        cmp $15, %eax
        jle 1f
        mov $15, %eax
1:      add %eax, %eax
        mov $7, %ecx
        sub bit, %ecx                    /* buf[mptr]>>(7-bit) & 1 */
        shr %cl, %r10d
        and $1, %r10d
        add %r10d, %eax
        mov %eax, mi
.Pmi0:  mov mi, %eax                     /* in[NE] = mi? st[cm_p(msm[mi])] : 0 */
        test %eax, %eax
        jz 2f
        movzwl cm_msm(,%rax,2), %edx
        xor $0x8000, %edx
        shr $4, %edx
        mov cm_st(,%rdx,4), %eax
        jmp 3f
2:      xor %eax, %eax
3:      mov %eax, cm_in+4*NE
        movl $256, cm_in+4*(NE+1)        /* bias */
        /* fc = (xf + (xf==F_DATA? cls&3 :0)) * 256 + c0; s2 = fc * NM;
         * cm_ms[0] = ((mlen? (mlen>15?15:mlen)/4+1 :0)*11*256 + fc) * NI */
        mov xf, %edx
        cmp $F_DATA, %edx
        jne 6f
        mov cls, %ecx
        and $3, %ecx
        add %ecx, %edx
6:      shl $8, %edx
        add c0, %edx                     /* fc */
        imul $CM_NM, %edx, %ecx
        mov %ecx, s2
        mov mlen, %eax
        test %eax, %eax
        jz 4f
        cmp $15, %eax
        jle 1f
        mov $15, %eax
1:      shr $2, %eax
        inc %eax
        jmp 5f
4:      xor %eax, %eax
5:      imul $(11*256), %eax, %eax
        add %edx, %eax
        imul $NI, %eax, %eax
        mov %eax, cm_ms
        /* cm_ms[m] = (5*11*256 + ((m-1)*256 + s)*8 + bit)*NI, s = (c4>>(m-1)*8 & 255)
         * for m < 5, else xo & 1023 (out of the code 1024 + (cls & 63)*4 + (sw != 0)*2 + (dep != 0)) */
        mov $1, %ebx
.Pms:   cmp $CM_NM, %ebx
        jge .Pdot
        lea -1(%rbx), %ecx               /* m-1 */
        mov %ecx, %r8d
        shl $3, %ecx                     /* (m-1)*8 */
        mov c4, %eax
        shr %cl, %eax
        movzbl %al, %eax
#if CM_NM > 5
        cmp $5, %ebx
        jne 1f
        mov xo, %eax                     /* mixer 5: the opcode */
        and $1023, %eax
        cmpl $F_DATA, xf
        jne 1f
        mov cls, %eax                    /* out of the code: 1024 + (cls & 63)*4 + (sw != 0)*2 + (dep != 0) */
        and $63, %eax
        cmpl $1, sw                      /* CF = (sw == 0); cmc: sw != 0 */
        cmc
        adc %eax, %eax
        cmpl $1, dep
        cmc
        adc %eax, %eax
        add $1024, %eax
1:
#endif
        shl $8, %r8d                     /* (m-1)*256 */
        add %r8d, %eax
        shl $3, %eax                     /* *8 */
        add $(5*11*256), %eax
        add bit, %eax
        imul $NI, %eax, %eax
        mov %eax, cm_ms(,%rbx,4)
        inc %ebx
        jmp .Pms
.Pdot:  /* dot2=0; for m: dot = sum_j in[j]*(w[ms[m]+j]>>8); d=md[m]=clamp(dot>>8);
         *          mp[m]=sq[d+2048]; dot2 += d*(w2[s2+m]>>8) */
        xor %r13d, %r13d                 /* dot2 */
        xor %ebx, %ebx                   /* m */
.Pm:    mov cm_ms(,%rbx,4), %eax
        lea cm_w(,%rax,4), %r12          /* &cm_w[ms[m]] */
        xor %ebp, %ebp                   /* dot */
        xor %ecx, %ecx                   /* j */
.Pj:    mov (%r12,%rcx,4), %eax
        sar $8, %eax
        imul cm_in(,%rcx,4), %eax
        add %eax, %ebp
        inc %ecx
        cmp $NI, %ecx
        jne .Pj
        sar $8, %ebp                     /* dot>>8 */
        mov %ebp, %eax                   /* clamp */
        cmp $-2047, %eax
        jge 1f
        mov $-2047, %eax
1:      cmp $2047, %eax
        jle 2f
        mov $2047, %eax
2:      mov %eax, cm_md(,%rbx,4)         /* d */
        mov %eax, %r14d                  /* save d */
        add $2048, %eax
        mov cm_sq(,%rax,4), %eax
        mov %eax, cm_mp(,%rbx,4)
        mov s2, %eax                     /* w2[s2+m] */
        add %ebx, %eax
        mov cm_w2(,%rax,4), %eax
        sar $8, %eax
        imul %r14d, %eax
        add %eax, %r13d
        inc %ebx
        cmp $CM_NM, %ebx
        jne .Pm
        mov %r13d, %eax                  /* clamp(dot2>>8) */
        sar $8, %eax
        cmp $-2047, %eax
        jge 1f
        mov $-2047, %eax
1:      cmp $2047, %eax
        jle 2f
        mov $2047, %eax
2:      add $2048, %eax
        mov cm_sq(,%rax,4), %eax
        mov %eax, pm                     /* cm_pm */
#if CM_APM
        mov %eax, %esi                   /* p */
        mov c0, %eax
        xor %r8d, %r8d                   /* APM 0: context c0 */
        call A
        mov %eax, %r15d                  /* a1 */
        mov c4, %eax
        movzbl %al, %eax
        shl $8, %eax
        add c0, %eax
        add $256, %eax                   /* 256 + c0 + (c4&255)*256 */
        mov $1, %r8d
        call A
        lea (%rax,%rax), %edx            /* 2*a2 */
        add %r15d, %edx                  /* a1 + 2*a2 */
        add pm, %edx                     /* p + a1 + 2*a2 */
        sar $2, %edx                     /* >> 2 */
        mov %edx, %eax
#else
        mov pm, %eax
#endif
        cmp $1, %eax                     /* p<1?1:p>4095?4095:p */
        jge 1f
        mov $1, %eax
1:      cmp $4095, %eax
        jle 2f
        mov $4095, %eax
2:      ret

/* ---- cm_update(eax = y): learn from the bit; at a byte boundary roll on ---- */
UP:     mov %eax, %r15d                  /* y */
        xor %ebx, %ebx                   /* for i: cm_upd(&t[ix[i]], y) */
.Utbl:  mov cm_ix(,%rbx,4), %eax
        lea cm_t(,%rax,2), %rdi
        mov %r15d, %eax
        call U
        inc %ebx
        cmp $NE, %ebx
        jne .Utbl
        mov mi, %eax                     /* if (mi) cm_upd(&msm[mi], y) */
        test %eax, %eax
        jz 1f
        lea cm_msm(,%rax,2), %rdi
        mov %r15d, %eax
        call U
1:
#if CM_APM
        /* both buckets of each APM learn */
        xor %ebx, %ebx
.Uapm:  mov %ebx, %eax
        shr $1, %eax
        mov cm_aj(,%rax,4), %eax
        mov %ebx, %ecx
        and $1, %ecx
        add %ecx, %eax
        lea cm_a(,%rax,2), %r12          /* a = &cm_a[aj[k>>1] + (k&1)] */
        mov %r15d, %eax                  /* (y<<16) - *a - y */
        shl $16, %eax
        movzwl (%r12), %edx
        sub %edx, %eax
        sub %r15d, %eax
        sar $CM_AR, %eax
        add %edx, %eax                   /* *a += ... */
        mov %ax, (%r12)
        inc %ebx
        cmp $4, %ebx
        jne .Uapm
#endif
        /* lr = CM_LR - (pos<1<<WARM) - (pos<4<<WARM) */
        mov $CM_LR, %r14d
#if CM_WARM
        mov pos, %eax
        cmp $(1<<CM_WARM), %eax
        jae 1f
        dec %r14d
1:      cmp $(4<<CM_WARM), %eax
        jae 2f
        dec %r14d
2:
#endif
        /* for m: err=(y<<12)-mp[m]; w[ms[m]+j]+=(in[j]*err)>>lr;
         *        w2[s2+m] += (md[m]*((y<<12)-pm))>>CM_LR2 */
        xor %ebx, %ebx                   /* m */
.Um:    mov %r15d, %r13d
        shl $12, %r13d                   /* y<<12 */
        mov %r13d, %ebp
        sub cm_mp(,%rbx,4), %ebp         /* err */
        mov cm_ms(,%rbx,4), %eax
        lea cm_w(,%rax,4), %r12          /* &cm_w[ms[m]] */
        xor %esi, %esi                   /* j */
.Uj:    mov cm_in(,%rsi,4), %eax
        imul %ebp, %eax
        mov %r14d, %ecx
        sar %cl, %eax                    /* >> lr */
        add %eax, (%r12,%rsi,4)
        inc %esi
        cmp $NI, %esi
        jne .Uj
        sub pm, %r13d                    /* (y<<12) - pm */
        mov cm_md(,%rbx,4), %eax
        imul %r13d, %eax
        sar $CM_LR2, %eax
        mov s2, %edx
        add %ebx, %edx
        add %eax, cm_w2(,%rdx,4)
        inc %ebx
        cmp $CM_NM, %ebx
        jne .Um
        /* c0 = c0*2+y; nib = nib*2+y */
        mov c0, %eax
        lea (%rax,%rax), %eax
        add %r15d, %eax
        mov %eax, c0
        mov nib, %eax
        lea (%rax,%rax), %eax
        add %r15d, %eax
        mov %eax, nib
        /* if (++bit == 8) { ... } else if (bit==4) nibble() */
        mov bit, %eax
        inc %eax
        mov %eax, bit
        cmp $8, %eax
        je .Ubyte
        cmp $4, %eax
        jne .Udone
        call NB
        jmp .Udone
.Ubyte: /* a completed byte */
        mov c0, %eax
        movzbl %al, %r13d                /* c = c0 & 255 */
        mov obuf, %r12
        mov pos, %ebp                    /* pos */
        mov %r13b, (%r12,%rbp)           /* buf[pos] = c */
        /* cm_i1[c4&255] = <<8 | c ; cm_i2[c4&0xffff] = <<8 | c */
        mov c4, %eax
        movzbl %al, %ecx
        mov cm_i1(,%rcx,4), %edx
        shl $8, %edx
        or %r13d, %edx
        mov %edx, cm_i1(,%rcx,4)
        mov c4, %eax
        movzwl %ax, %ecx
        mov cm_i2(,%rcx,4), %edx
        shl $8, %edx
        or %r13d, %edx
        mov %edx, cm_i2(,%rcx,4)
        /* c8 = c8<<8 | c4>>24 ; c4 = c4<<8 | c */
        mov c4, %eax
        shr $24, %eax
        mov c8, %edx
        shl $8, %edx
        or %eax, %edx
        mov %edx, c8
        mov c4, %eax
        shl $8, %eax
        or %r13d, %eax
        mov %eax, c4
        /* cls = cls<<2 | class(c) ; word = (al||dg)? (word+c+1)*K : 0 */
        mov %r13d, %eax                  /* al = ((c|32)-'a'<26) || c=='_' */
        or $32, %eax
        sub $'a', %eax
        cmp $26, %eax
        setb %al
        movzbl %al, %r8d
        cmp $'_', %r13d
        sete %al
        movzbl %al, %eax
        or %eax, %r8d                    /* r8d = al */
        lea -'0'(%r13), %eax             /* dg = (c-'0' < 10) */
        cmp $10, %eax
        setb %al
        movzbl %al, %r9d                 /* r9d = dg */
        mov $0, %edx                     /* class: al?1:dg?2:printable?3:0 */
        test %r8d, %r8d
        jz 1f
        mov $1, %edx
        jmp 4f
1:      test %r9d, %r9d
        jz 2f
        mov $2, %edx
        jmp 4f
2:      lea -32(%r13), %eax              /* c-32 < 96 ? printable */
        cmp $96, %eax
        jae 4f
        mov $3, %edx
4:      mov cls, %eax
        shl $2, %eax
        or %edx, %eax
        mov %eax, cls
        /* the GLSL's shape (cm.h): a bracket notes the word before it and each
         * comma bumps that entry, and braces are counted. A ; { } or NUL ends a
         * statement, whose first word is the first word to end after it, and a
         * NUL also resets the depths. */
        mov %r13d, %eax                  /* c */
        mov word, %esi                   /* the word before c */
        mov dep, %ecx
        cmp $'(', %al                    /* ( : fc[++dep & 255] = word */
        jne 1f
        inc %ecx
        movzbl %cl, %edx
        mov %esi, cm_fc(,%rdx,4)
1:      cmp $')', %al                    /* ) : --dep */
        jne 2f
        dec %ecx
2:      mov %ecx, dep
        cmp $',', %al                    /* , : ++fc[dep & 255] */
        jne 3f
        movzbl %cl, %edx
        incl cm_fc(,%rdx,4)
3:      cmp $'{', %al                    /* { : ++bd; a new statement */
        jne 1f
        incl bd
        jmp 7f
1:      cmp $'}', %al                    /* } : --bd; a new statement */
        jne 2f
        decl bd
        jmp 7f
2:      cmp $';', %al                    /* ; : a new statement */
        je 7f
        test %al, %al                    /* NUL: bd = dep = 0; a new statement */
        jnz 3f
        mov %eax, bd
        mov %eax, dep
        jmp 7f
3:      cmpl $0, sw                      /* else if (!sw && !(al || dg)) sw = word */
        jne 8f
        mov %r8d, %edx
        or %r9d, %edx
        jz 6f
        jmp 8f
7:      mov sw, %edx                     /* a new statement: if (sw) psw = sw; sw = 0 */
        test %edx, %edx
        jz 9f
        mov %edx, psw
9:      xor %esi, %esi
6:      mov %esi, sw
8:      mov %r8d, %eax                   /* al || dg */
        or %r9d, %eax
        jz 5f
        mov word, %eax
        add %r13d, %eax
        inc %eax
        imul $0x2F0F1A3B, %eax, %eax
        jmp 6f
5:      xor %eax, %eax
6:      mov %eax, word
        inc %ebp                         /* pos++ */
        mov %ebp, pos
        /* match model */
        mov mlen, %eax
        test %eax, %eax
        jz 7f
        mov mptr, %ecx
        movzbl (%r12,%rcx), %edx         /* buf[mptr] */
        cmp %r13d, %edx                  /* == c ? */
        jne 7f
        inc %eax
        mov %eax, mlen
        inc %ecx
        mov %ecx, mptr
        jmp 8f
7:      movl $0, mlen
8:      cmp $6, %ebp                     /* if (pos >= 6) */
        jb .Uwalk
        /* h = cm_hash(c4 ^ cm_hash(c8)) >> (32-CM_MB) */
        mov c8, %eax
        call H
        xor c4, %eax
        call H
        shr $(32-CM_MB), %eax
        mov %eax, %r14d                  /* h */
        mov mlen, %eax                   /* if (!mlen && mt[h]) */
        test %eax, %eax
        jnz .Umt
        mov cm_mt(,%r14,4), %ecx
        test %ecx, %ecx
        jz .Umt
        mov %ecx, mptr                   /* mptr = mt[h] */
        xor %r8d, %r8d                   /* l = 0 */
.Uext:  cmp $32, %r8d                    /* l<32 && l<mptr && buf[mptr-1-l]==buf[pos-1-l] */
        jge .Uextd
        cmp %r8d, %ecx                   /* l < mptr */
        jle .Uextd
        mov %ecx, %eax                   /* mptr-1-l */
        sub %r8d, %eax
        dec %eax
        movzbl (%r12,%rax), %edx
        mov %ebp, %eax                   /* pos-1-l */
        sub %r8d, %eax
        dec %eax
        movzbl (%r12,%rax), %eax
        cmp %al, %dl
        jne .Uextd
        inc %r8d
        jmp .Uext
.Uextd: cmp $CM_MM, %r8d                 /* mlen = l>=CM_MM? l : 0 */
        jl 1f
        mov %r8d, mlen
        jmp .Umt
1:      movl $0, mlen
.Umt:   mov %ebp, cm_mt(,%r14,4)         /* mt[h] = pos */
.Uwalk: /* the x86 walk: if (pos>=CS && pos<CE) ... else xf=F_DATA */
        cmp $CM_CS, %ebp
        jb .Udata
        cmp $CM_CE, %ebp
        jae .Udata
        cmp $CM_CS, %ebp                 /* if (pos==CS) xs=pos */
        jne 1f
        mov %ebp, xs
1:      mov xs, %eax                     /* cm_xf = cm_xfield(buf+xs, pos-xs, &cm_xo, &cm_xm, ...) */
        lea (%r12,%rax), %rdi
        mov %ebp, %esi
        sub %eax, %esi
        call XF
        mov %eax, xf
        mov xf_opc, %edx                 /* opc -> cm_xo */
        mov %edx, xo
        cmp $F_NEW, %eax                 /* if (xf==F_NEW || pos-xs>=15) */
        je 2f
        mov %ebp, %eax
        sub xs, %eax
        cmp $15, %eax
        jl .Udone
2:      xor %eax, %eax                   /* h=0; for j=xs..pos-1: h=cm_hash(h+buf[j]) */
        mov xs, %ecx
.Uxlh:  cmp %ebp, %ecx
        jge 3f
        movzbl (%r12,%rcx), %edx
        add %edx, %eax
        push %rcx
        call H
        pop %rcx
        inc %ecx
        jmp .Uxlh
3:      mov xo, %ecx                     /* cm_xl[xo & 1023] = pi = h */
        and $1023, %ecx
        mov %eax, cm_xl(,%rcx,4)
        mov %eax, pi
        mov xh, %eax                     /* xh = (xh<<10 | xo) & 0x3ffff */
        shl $10, %eax
        or xo, %eax
        and $0x3ffff, %eax
        mov %eax, xh
        mov xo, %eax                     /* xp = xo << 8 | ModRM */
        shl $8, %eax
        or xf_mrm, %eax
        mov %eax, xp
        cmpl $F_NEW, xf                  /* if (xf == F_NEW && r >= 0): rt2 = rt; rt = the target */
        jne 4f
        mov xf_rel, %ecx
        test %ecx, %ecx
        js 4f
        add xs, %ecx
        mov (%r12,%rcx), %eax
        mov rt, %edx
        mov %edx, rt2
        mov %eax, rt
4:      mov %ebp, xs                     /* xs=pos; xf=F_OP(0); xo=0 */
        movl $0, xf
        movl $0, xo
        jmp .Udone
.Udata: movl $F_DATA, xf
.Udone: cmpl $8, bit                      /* if it was a completed byte: reset, cm_ctx */
        jne 9f
        movl $1, c0
        movl $0, bit
        call CTX
9:      ret

/* ---- cm_nibble: pick each model's 16-slot block ---- */
NB:             xor %ebx, %ebx                   /* i */
.Ni:    mov cm_cx(,%rbx,4), %eax         /* h = cm_hash(cx[i] + c0*K) */
        mov c0, %edx
        imul $0x1F3D5B79, %edx, %edx
        add %edx, %eax
        call H
        mov %eax, %r13d                  /* c = (u16)(h|1) */
        or $1, %r13d
        movzwl %r13w, %r13d
        shr $(32-CM_TB), %eax            /* b = (h>>(32-TB)) & ~15 */
        and $0xFFFFFFF0, %eax
        mov %eax, %r12d                  /* b */
        mov %eax, %edx                   /* b2 = b ^ 16 */
        xor $16, %edx
        cmp %r13w, cm_t(,%rax,2)         /* if (t[b] != c) */
        je .Nhave
        cmp %r13w, cm_t(,%rdx,2)         /* if (t[b2] == c) b = b2 */
        jne 1f
        mov %edx, %r12d
        jmp .Nhave
1:      movzwl cm_t+2(,%rdx,2), %eax     /* (t[b2+1]^0x8000)&15 < (t[b+1]^0x8000)&15 ? */
        xor $0x8000, %eax
        and $15, %eax
        mov %r12d, %ecx
        movzwl cm_t+2(,%rcx,2), %esi
        xor $0x8000, %esi
        and $15, %esi
        cmp %esi, %eax
        jge 2f
        mov %edx, %r12d                  /* b = b2 */
2:      mov %r12d, %ecx                  /* for j: t[b+j]=0 */
        lea 16(%r12), %esi
.Nclr:  movw $0, cm_t(,%rcx,2)
        inc %ecx
        cmp %esi, %ecx
        jne .Nclr
        mov %r12d, %ecx                  /* t[b] = c */
        mov %r13w, cm_t(,%rcx,2)
.Nhave: mov %r12d, cm_base(,%rbx,4)      /* base[i] = b */
        inc %ebx
        cmp $NE, %ebx
        jne .Ni
        movl $1, nib
        ret

/* ---- cm_ctx: hash every model's context, then pick blocks ---- */
CTX:    xor %r14d, %r14d                 /* k */
        xor %ebx, %ebx                   /* i over the masks */
.Ck:    movzbl cm_mask(,%rbx), %r15d     /* mk */
        xor %ebp, %ebp                   /* lo */
        xor %r13d, %r13d                 /* hi */
        xor %r9d, %r9d                   /* j */
.Cj:    mov %r9d, %ecx
        shl $3, %ecx                     /* 8*j */
        bt %r9d, %r15d                   /* lo: mask bit j */
        jnc 1f
        mov c4, %eax
        mov $0xFF, %edx
        shl %cl, %edx
        and %edx, %eax
        or %eax, %ebp
1:      lea 4(%r9), %edx                 /* hi: mask bit j+4 */
        bt %edx, %r15d
        jnc 2f
        mov c8, %eax
        mov $0xFF, %edx
        shl %cl, %edx
        and %edx, %eax
        or %eax, %r13d
2:      inc %r9d
        cmp $4, %r9d
        jne .Cj
        mov %r15d, %eax                  /* cm_hash(lo ^ cm_hash(hi + mk*K)) */
        imul $0x3C6EF372, %eax, %eax
        add %r13d, %eax
        call H
        xor %ebp, %eax
        call H
        mov %eax, cm_cx(,%r14,4)
        inc %r14d
        inc %ebx
        cmp $NK, %ebx
        jne .Ck
        /* the extra models (compiled in only when CM_XSET selects them) */
        mov c4, %ebp
        movzbl %bpl, %ebp                /* b1 = c4 & 255 */
        mov c4, %r13d
        movzwl %r13w, %r13d              /* b2 = c4 & 0xffff */
#if (CM_XSET & 1)                        /* the word so far, with b1 on top */
        mov word, %eax
        mov %ebp, %edx
        shl $24, %edx
        xor %edx, %eax
        add $((1*0x5851F42D) & 0xFFFFFFFF), %eax
        call H
        mov %eax, cm_cx(,%r14,4)
        inc %r14d
#endif
#if (CM_XSET & 2)                        /* b1 and the 4-byte column (pos & 3) */
        mov pos, %eax
        and $3, %eax
        shl $8, %eax
        or %ebp, %eax
        add $((2*0x5851F42D) & 0xFFFFFFFF), %eax
        call H
        mov %eax, cm_cx(,%r14,4)
        inc %r14d
#endif
#if (CM_XSET & 4)                        /* b1 -> what last followed it */
        mov cm_i1(,%rbp,4), %eax
        shl $8, %eax
        or %ebp, %eax
        add $((3*0x5851F42D) & 0xFFFFFFFF), %eax
        call H
        mov %eax, cm_cx(,%r14,4)
        inc %r14d
#endif
#if (CM_XSET & 8)                        /* b2b1 -> what last followed it */
        mov cm_i2(,%r13,4), %eax
        shl $16, %eax
        or %r13d, %eax
        add $((4*0x5851F42D) & 0xFFFFFFFF), %eax
        call H
        mov %eax, cm_cx(,%r14,4)
        inc %r14d
#endif
        /* the x86 models: f, ix, ins; then the seven contexts */
        mov xf, %r12d                    /* f */
        mov pos, %ebp
        sub xs, %ebp                     /* ix */
        mov %r12d, %r13d                 /* ins = f */
        cmp $F_DATA, %r12d
        jge .Cinsd
        mov xs, %ecx
.Cins:  cmp pos, %ecx
        jge .Cinsd
        mov obuf, %rax
        movzbl (%rax,%rcx), %edx
        mov %r13d, %eax
        add %edx, %eax
        push %rcx
        call H
        pop %rcx
        mov %eax, %r13d
        inc %ecx
        jmp .Cins
.Cinsd: mov %r12d, yA+0                  /* y0 = f */
        mov %ebp, %ecx                   /* f | ix<<3 */
        shl $3, %ecx
        or %r12d, %ecx
        mov xo, %eax                     /* y1 = f | ix<<3 | xo<<8 */
        shl $8, %eax
        or %ecx, %eax
        mov %eax, yA+4
        imul $0x9E3779B1, xh, %eax       /* y2 = ins ^ xh*K */
        xor %r13d, %eax
        mov %eax, yA+8
        mov c4, %eax                     /* y3 = fb = f | ix<<3 | b1<<8 */
        movzbl %al, %eax
        shl $8, %eax
        or %ecx, %eax
        mov %eax, yA+12
        imul $3, pi, %edx                /* y4 = pi*3 + fb */
        add %edx, %eax
        mov %eax, yA+16
        mov xo, %eax                     /* y5 = xl[xo&1023]*3 + ins */
        and $1023, %eax
        imul $3, cm_xl(,%rax,4), %eax
        add %r13d, %eax
        mov %eax, yA+20
        mov xp, %eax                     /* y6, by the field: */
        cmp $F_MODRM, %r12d
        jne 1f
        mov xo, %edx                     /* ModRM: xo<<16 ^ xp */
        shl $16, %edx
        xor %edx, %eax
        jmp 3f
1:      shr $8, %eax                     /* prefix, opcode, SIB: ins*5 + (xp>>8) */
        imul $5, %r13d, %edx
        add %edx, %eax
        cmp $F_DISP, %r12d
        jb 3f
        imul $3, rt, %eax                /* displacement, immediate: (rt*3 + rt2)*5 + f | ix<<3 */
        add rt2, %eax
        imul $5, %eax, %eax
        add %ecx, %eax
3:      mov %eax, yA+24
        cmp $F_DATA, %r12d               /* outside the code: the text's shape instead */
        jne .Cnd
        mov c4, %esi
        movzbl %sil, %edi                /* b1 */
        mov dep, %ecx
        movzbl %cl, %eax
        mov cm_fc(,%rax,4), %eax         /* g = fc[dep & 255] */
        mov %eax, yA+0                   /* y0 = g: the call and argument */
        imul $3, %eax, %eax
        shl $8, %eax
        or %edi, %eax
        mov %eax, yA+16                  /* y4 = g*3 << 8 | b1 */
        shl $8, %ecx
        or %ecx, %edi                    /* dep<<8 | b1 */
        mov sw, %eax
        imul $3, %eax, %eax
        add %edi, %eax
        mov %eax, yA+4                   /* y1 = sw*3 + (dep<<8 | b1) */
        mov bd, %eax
        shl $16, %eax
        or %edi, %eax
        mov %eax, yA+8                   /* y2 = bd<<16 | dep<<8 | b1 */
        shl $16, %ecx                    /* dep << 24 */
        and $0xffffff, %esi
        xor %ecx, %esi
        mov %esi, yA+12                  /* y3 = (c4 & 0xffffff) ^ dep<<24 */
        mov cls, %eax
        mov %eax, yA+20                  /* y5 = the classes of 16 bytes */
        imul $3, psw, %eax               /* y6 = (psw*3 + sw)*5 + word + bd*11 */
        add sw, %eax
        imul $5, %eax, %eax
        add word, %eax
        imul $11, bd, %edx
        add %edx, %eax
        mov %eax, yA+24
.Cnd:   xor %ecx, %ecx                   /* for i in 0..6: cx[k++]=cm_hash(y[i]+yk[i]) */
.Cy:    mov yA(,%rcx,4), %eax
        add yk(,%rcx,4), %eax
        push %rcx
        call H
        pop %rcx
        mov %eax, cm_cx(,%r14,4)
        inc %r14d
        inc %ecx
        cmp $7, %ecx
        jne .Cy
        jmp NB

/* ---- cm_xfield: the field of byte p[n] (rdi=p, esi=n) in eax, with *opc, *mrm,
 * *rel, *len in the scratch globals ---- */
XF:     push %rbx
        push %rbp
        push %r12
        push %r13
        push %r14
        push %r15
        mov %rdi, %r12                   /* p */
        mov %esi, %r13d                  /* n */
        xor %ebx, %ebx                   /* i = 0 */
        xor %r14d, %r14d                 /* w = 0 */
        mov $4, %r15d                    /* z = 4 */
        xor %r10d, %r10d                 /* d = 0 */
        movl $0, xf_mrm                  /* m = 0 / *mrm = 0 */
        movl $-1, xf_rel
        movl $0, xf_opc
        mov %r13d, %eax                  /* *len = n>0?n:1 */
        test %eax, %eax
        jg 1f
        mov $1, %eax
1:      mov %eax, xf_len
.Xpre:  cmp %r13d, %ebx                  /* prefixes/REX */
        jge .Xpd
        cmp $14, %ebx
        jge .Xpd
        movzbl (%r12,%rbx), %edx         /* p[i] */
        mov %edx, %ecx
        shr $1, %ecx
        movzbl cm_xt(,%rcx), %eax
        mov %edx, %ecx
        and $1, %ecx
        shl $2, %ecx
        shr %cl, %eax
        test $8, %eax
        jz .Xpd
        mov %edx, %eax                   /* w |= (p[i]&0xf8)==0x48 */
        and $0xf8, %eax
        cmp $0x48, %eax
        jne 2f
        or $1, %r14d
2:      cmp $0x66, %edx                  /* if (p[i]==0x66) z=2 */
        jne 3f
        mov $2, %r15d
3:      inc %ebx
        jmp .Xpre
.Xpd:   cmp %r13d, %ebx                  /* if (i>=n) return F_OP */
        jl 1f
        xor %eax, %eax
        jmp .Xret
1:      movzbl (%r12,%rbx), %ebp         /* o = p[i++] */
        inc %ebx
        cmp $0x0f, %ebp
        jne .Xone
        cmp %r13d, %ebx                  /* 0F: if (i>=n) return F_OP2 */
        jl 1f
        mov $1, %eax
        jmp .Xret
1:      movzbl (%r12,%rbx), %ebp         /* o = p[i++] */
        inc %ebx
        mov %ebp, %ecx                   /* e = cm_xt[128+(o>>2)] >> (o&3)*2 & 3 */
        shr $2, %ecx
        add $128, %ecx
        movzbl cm_xt(,%rcx), %r8d
        mov %ebp, %ecx
        and $3, %ecx
        shl $1, %ecx
        shr %cl, %r8d
        and $3, %r8d
        mov %r8d, %r9d                   /* im = e>>1 */
        shr $1, %r9d
        cmp $0x38, %ebp
        je .X3b
        cmp $0x3a, %ebp
        je .X3b
        mov %ebp, %eax                   /* if ((o&0xf0)==0x80) im=4 (jcc rel32) */
        and $0xf0, %eax
        cmp $0x80, %eax
        jne 2f
        mov $4, %r9d
2:      or $0x100, %ebp                  /* o |= 0x100 */
        jmp .Xaf
.X3b:   cmp %r13d, %ebx                  /* three-byte: if (i>=n) return F_OP2 */
        jl 1f
        mov $1, %eax
        jmp .Xret
1:      mov %ebp, %eax                   /* im = o&2 ? 1 : 0 */
        and $2, %eax
        jz 2f
        mov $1, %r9d
        jmp 3f
2:      xor %r9d, %r9d
3:      add $0xc8, %ebp                  /* o = (o+0xc8)<<8 | p[i++] */
        shl $8, %ebp
        movzbl (%r12,%rbx), %eax
        or %eax, %ebp
        inc %ebx
        mov $1, %r8d                     /* e = 1 */
        jmp .Xaf
.Xone:  mov %ebp, %ecx                   /* e = cm_xt[o>>1] >> (o&1)*4 */
        shr $1, %ecx
        movzbl cm_xt(,%rcx), %r8d
        mov %ebp, %ecx
        and $1, %ecx
        shl $2, %ecx
        shr %cl, %r8d
        mov %r8d, %r9d                   /* im = e>>1 & 3 */
        shr $1, %r9d
        and $3, %r9d
        cmp $2, %r9d                     /* if (im==2) im=z */
        jne .Xaf
        mov %r15d, %r9d
.Xaf:   mov %ebp, xf_opc                 /* *opc = o */
        test $1, %r8d                    /* if (e & 1): ModRM */
        jz .Xnm
        cmp %r13d, %ebx
        jl 1f
        mov $F_MODRM, %eax
        jmp .Xret
1:      movzbl (%r12,%rbx), %eax         /* m = p[i++] */
        mov %eax, xf_mrm
        inc %ebx
        mov %eax, %ecx                   /* mod = m>>6 */
        shr $6, %ecx
        mov %eax, %edx                   /* rm = m&7 */
        and $7, %edx
        cmp $3, %ecx                     /* if (mod!=3 && rm==4): SIB */
        je .Xns
        cmp $4, %edx
        jne .Xns
        cmp %r13d, %ebx
        jl 1f
        mov $F_SIB, %eax
        jmp .Xret
1:      test %ecx, %ecx                  /* if (mod==0 && (p[i]&7)==5) d=4 */
        jnz 2f
        movzbl (%r12,%rbx), %eax
        and $7, %eax
        cmp $5, %eax
        jne 2f
        mov $4, %r10d
2:      inc %ebx
.Xns:   cmp $1, %ecx                     /* mod==1? d=1 : mod==2? d=4 : (mod==0&&rm==5)? d=4,rel=i */
        jne 1f
        mov $1, %r10d
        jmp .Xnm
1:      cmp $2, %ecx
        jne 2f
        mov $4, %r10d
        jmp .Xnm
2:      test %ecx, %ecx
        jnz .Xnm
        cmp $5, %edx
        jne .Xnm
        mov $4, %r10d
        mov %ebx, xf_rel
.Xnm:   cmp $3, %r9d                     /* if (im==3): the specials */
        jne .Xln
        cmp $0xb8, %ebp                  /* mov r, imm: o in 0xb8..0xbf ? (w?8:z) */
        jl 1f
        cmp $0xbf, %ebp
        jg 1f
        test %r14d, %r14d
        jz 11f
        mov $8, %r9d
        jmp .Xln
11:     mov %r15d, %r9d
        jmp .Xln
1:      cmp $0xe8, %ebp                  /* call/jmp rel32: rel=i+d, 4 */
        je 12f
        cmp $0xe9, %ebp
        jne 2f
12:     mov %ebx, %eax
        add %r10d, %eax
        mov %eax, xf_rel
        mov $4, %r9d
        jmp .Xln
2:      cmp $0xc8, %ebp                  /* enter: 3 */
        jne 3f
        mov $3, %r9d
        jmp .Xln
3:      cmp $0xc2, %ebp                  /* ret imm16: 2 */
        je 31f
        cmp $0xca, %ebp
        jne 4f
31:     mov $2, %r9d
        jmp .Xln
4:      cmp $0xa4, %ebp                  /* moffs etc (o<0xa4): 8 */
        jge 5f
        mov $8, %r9d
        jmp .Xln
5:      mov xf_mrm, %eax                 /* test/not/...: (m>>3&7)<2 ? (o==0xf6?1:z) : 0 */
        shr $3, %eax
        and $7, %eax
        cmp $2, %eax
        jge 6f
        cmp $0xf6, %ebp
        jne 51f
        mov $1, %r9d
        jmp .Xln
51:     mov %r15d, %r9d
        jmp .Xln
6:      xor %r9d, %r9d
.Xln:   mov %ebx, %eax                   /* *len = i + d + im */
        add %r10d, %eax
        add %r9d, %eax
        mov %eax, xf_len
        mov %ebx, %eax                   /* if (n < i+d) return F_DISP */
        add %r10d, %eax
        cmp %eax, %r13d
        jl .Xds
        add %r9d, %eax                   /* if (n < i+d+im) return F_IMM */
        cmp %eax, %r13d
        jl .Xim
        mov $F_NEW, %eax
        jmp .Xret
.Xds:   mov $F_DISP, %eax
        jmp .Xret
.Xim:   mov $F_IMM, %eax
.Xret:  pop %r15
        pop %r14
        pop %r13
        pop %r12
        pop %rbp
        pop %rbx
        ret

/* ---- cm_x86t over the code (enc = 0, the inverse) ---- */
X86T:           mov obuf, %r12
        mov onum, %r13d                  /* n */
        mov $CM_CS, %ebx                 /* i */
.Ti:    cmp $CM_CE, %ebx
        jge .Tdn
        lea 15(%rbx), %eax               /* i + 15 < n */
        cmp %r13d, %eax
        jge .Tdn
        lea (%r12,%rbx), %rdi
        mov $15, %esi
        call XF
        mov xf_rel, %ecx                 /* if (r>=0 && r+4<=l) */
        test %ecx, %ecx
        js .Tad
        lea 4(%rcx), %eax
        cmp xf_len, %eax
        jg .Tad
        mov %ebx, %edx                   /* q = b + i + r */
        add %ecx, %edx
        lea (%r12,%rdx), %rsi
        mov (%rsi), %eax                 /* a = read32(q) */
        mov %ebx, %edx                   /* a -= i + l */
        add xf_len, %edx
        sub %edx, %eax
        mov %eax, (%rsi)
.Tad:   add xf_len, %ebx                 /* i += l */
        jmp .Ti
.Tdn:   ret

/* ---- cm_in8, shifted into cm_x: x = x<<8 | the next coded byte (0xff past the end) ---- */
IN8:    mov sp_, %eax
        mov $255, %edx
        cmp slen, %eax
        jae 1f
        mov src, %rdx
        movzbl (%rdx,%rax), %edx
        incl sp_
1:      shll $8, xval
        or %edx, xval
        ret

/* ---- cm_init: build the tables, then cm_ctx ---- */
INIT:           movl $1, c0                      /* the non-zero static inits of cm.h */
        movl $1, nib
        movl $F_DATA, xf
        /* squash: cm_sq[2048+-d] from the integer e^(-1/256) recurrence */
        mov $0x80000000, %r8             /* v = 1 << 31 (64-bit) */
        xor %ecx, %ecx                   /* d */
.Isq:   mov $0x80000000, %rax            /* p = (4096<<31) / (2^31 + v) */
        add %r8, %rax
        mov %rax, %rsi
        mov $0x80000000000, %rax         /* 4096 << 31 = 2^43 */
        xor %edx, %edx
        div %rsi
        cmp $4095, %eax
        jbe 1f
        mov $4095, %eax
1:      lea 2048(%rcx), %rdx
        mov %eax, cm_sq(,%rdx,4)         /* cm_sq[2048+d] = p */
        mov $4096, %edx
        sub %eax, %edx
        mov $2048, %edi
        sub %ecx, %edi
        mov %edx, cm_sq(,%rdi,4)         /* cm_sq[2048-d] = 4096 - p */
        mov %r8, %rax                    /* v = v * 4278222805 >> 32 */
        mov $4278222805, %esi
        mul %rsi
        shrd $32, %rdx, %rax
        mov %rax, %r8
        inc %ecx
        cmp $2048, %ecx
        jne .Isq
        movl $1, cm_sq                   /* cm_sq[0] = 1 */
        xor %esi, %esi                   /* stretch: its inverse. pi = 0 */
        mov $-2047, %ecx                 /* x */
.Istx:  lea 2048(%ecx), %eax             /* x + 2048 (32-bit wrap for negative x) */
        mov cm_sq(,%rax,4), %edi         /* i = cm_sq[x+2048] */
.Isti:  cmp %edi, %esi                   /* for (; pi<=i; ++pi) cm_st[pi]=x */
        jg 1f
        mov %ecx, cm_st(,%rsi,4)
        inc %esi
        jmp .Isti
1:      inc %ecx
        cmp $2047, %ecx
        jle .Istx
.Istt:  cmp $4096, %esi                  /* for (; pi<4096; ++pi) cm_st[pi]=2047 */
        jge .Idt
        movl $2047, cm_st(,%rsi,4)
        inc %esi
        jmp .Istt
.Idt:   xor %ecx, %ecx                   /* cm_dt[i] = 16384 / (2i+3) */
.Idtl:  lea 3(%rcx,%rcx), %edi
        mov $16384, %eax
        xor %edx, %edx
        div %edi
        mov %eax, cm_dt(,%rcx,4)
        inc %ecx
        cmp $16, %ecx
        jne .Idtl
        xor %rcx, %rcx                   /* cm_w[i] = CM_WI << 10 */
.Iwl:   movl $(CM_WI<<10), cm_w(,%rcx,4)
        inc %rcx
        cmp $(WS*NI), %rcx
        jne .Iwl
        mov $(65536/CM_NM), %eax         /* cm_w2[i] = 65536 / CM_NM */
        xor %rcx, %rcx
.Iw2:   mov %eax, cm_w2(,%rcx,4)
        inc %rcx
        cmp $(11*256*CM_NM), %rcx
        jne .Iw2
#if CM_APM
        xor %rcx, %rcx                   /* cm_a[i] = cm_sq[min(i%33*128,4095)] * 16 */
        xor %r8d, %r8d                   /* k = i % 33 */
.Iap:   mov %r8d, %eax
        shl $7, %eax
        cmp $4096, %eax
        jb 1f
        mov $4095, %eax
1:      mov cm_sq(,%rax,4), %eax
        shl $4, %eax
        mov %ax, cm_a(,%rcx,2)
        inc %r8d
        cmp $33, %r8d
        jne 2f
        xor %r8d, %r8d
2:      inc %rcx
        cmp $((256+65536)*33), %rcx
        jne .Iap
#endif
        jmp CTX

/* ---- cm_decode: the driver (src/slen/obuf/onum already set) ---- */
DEC:    call INIT                        /* (cm_x, its read position and x1 start at 0: .bss) */
        mov $4, %ebx                     /* for i<4: x = x<<8 | in8() */
.Dx0:   call IN8
        dec %ebx
        jnz .Dx0
        notl x2                          /* x2 = ~0 */
.Dbit:  mov pos, %eax                    /* until the model holds all n bytes */
        cmp onum, %eax
        jae .Dfl
        call P                           /* p = cm_predict() */
        mov x2, %edx                     /* xm = x1 + ((x2-x1)>>12)*p */
        sub x1, %edx
        shr $12, %edx
        imul %eax, %edx
        add x1, %edx
        xor %eax, %eax                   /* y = cm_x <= xm: then x2 = xm, else x1 = xm+1 */
        cmp %edx, xval
        ja 1f
        inc %eax
        mov %edx, x2
        jmp 2f
1:      inc %edx
        mov %edx, x1
2:      call UP                          /* cm_update(y) */
.Drn:   mov x1, %eax                     /* renormalize while the top bytes agree */
        xor x2, %eax
        shr $24, %eax
        jnz .Dbit
        shll $8, x1
        shll $8, x2
        orb $0xff, x2
        call IN8
        jmp .Drn
.Dfl:   jmp X86T                         /* cm_x86t(out, n, 0) */

#ifdef MC_S2_TEST
/* The host test entry: s2_dec(src, len, out, n). */
        .globl s2_dec
s2_dec: push %rbx                        /* the decoder doesn't preserve callee-saved registers */
        push %rbp
        push %r12
        push %r13
        push %r14
        push %r15
        mov %rdi, src
        mov %esi, slen
        mov %rdx, obuf
        mov %ecx, onum
        call DEC
        pop %r15
        pop %r14
        pop %r13
        pop %r12
        pop %rbp
        pop %rbx
        ret
/* A host hook mirroring cm_xfield: s2_xfield(p, n, *opc, *mrm, *rel, *len). */
        .globl s2_xfield
s2_xfield:
        push %rbx
        push %r13
        push %r14
        push %r15
        mov %rdx, %rbx                   /* *opc */
        mov %rcx, %r13                    /* *mrm */
        mov %r8, %r14                     /* *rel */
        mov %r9, %r15                     /* *len */
        call XF
        push %rax
        mov xf_opc, %edx
        mov %edx, (%rbx)
        mov xf_mrm, %edx
        mov %edx, (%r13)
        mov xf_rel, %edx
        mov %edx, (%r14)
        mov xf_len, %edx
        mov %edx, (%r15)
        pop %rax
        pop %r15
        pop %r14
        pop %r13
        pop %rbx
        ret
#else
/* ---- the second stage: decode the payload and run it ----
 * The first stage jumps here, to S2_ADDR (tools/stage2.ld puts it first), with
 * the initial stack (argc, argv[], NULL, envp[]) in rdi and the coded payload
 * in rsi. The payload's length is our own constant, MC_BLOB_LEN. */
        .section .text.entry
        .globl s2_main
s2_main:
        mov %rsi, src                    /* the coded payload */
        movl $MC_BLOB_LEN, slen
        movq $g_out, obuf
        movl $MC_UNCOMP_LEN, onum
        push %rdi                        /* keep the stack pointer */
        call DEC
        pop %rbx
        mov $319, %eax                   /* memfd_create("", MFD_CLOEXEC) */
        mov $estr, %edi
        mov $1, %esi
        xor %edx, %edx
        xor %r10d, %r10d
        xor %r8d, %r8d
        syscall
        mov %eax, %r12d                  /* fd */
        xor %r13d, %r13d                 /* w = 0 (a bad fd fails the write) */
.Mw:    cmp $MC_UNCOMP_LEN, %r13d
        jae .Mex
        mov $1, %eax                     /* write(fd, g_out+w, len-w) */
        mov %r12d, %edi
        mov $g_out, %esi
        add %r13d, %esi
        mov $MC_UNCOMP_LEN, %edx
        sub %r13d, %edx
        xor %r10d, %r10d
        xor %r8d, %r8d
        syscall
        test %rax, %rax
        jle .Mfl
        add %eax, %r13d
        jmp .Mw
.Mex:   mov %r12d, %edi                  /* execveat(fd, "", argv, envp, AT_EMPTY_PATH) */
        mov $estr, %esi
        lea 8(%rbx), %rdx                /* argv = sp + 1 */
        mov (%rbx), %rax                 /* argc */
        lea 16(%rbx,%rax,8), %r10        /* envp = sp + argc + 2 */
        mov $0x1000, %r8d
        mov $322, %eax
        syscall
.Mfl:   mov $60, %eax                    /* exit(1), only reached on failure */
        mov $1, %edi
        syscall
        jmp .Mfl
        .section .rodata
estr:   .byte 0
#endif

/* The per-model hash salts for the x86/text contexts (cm.h). */
        .section .rodata
yk:     .long (6*0x6C8E9CF5) & 0xFFFFFFFF
        .long (7*0x6C8E9CF5) & 0xFFFFFFFF
        .long (8*0x6C8E9CF5) & 0xFFFFFFFF
        .long (9*0x6C8E9CF5) & 0xFFFFFFFF
        .long (10*0x6C8E9CF5) & 0xFFFFFFFF
        .long (11*0x6C8E9CF5) & 0xFFFFFFFF
        .long (12*0x6C8E9CF5) & 0xFFFFFFFF

/* ---- the model tables and scalars, all .bss ---- */
        .lcomm cm_sq, 4096*4
        .lcomm cm_st, 4096*4
        .lcomm cm_dt, 16*4
        .lcomm cm_t, 2 << CM_TB
        .lcomm cm_cx, NE*4
        .lcomm cm_base, NE*4
        .lcomm cm_ix, NE*4
        .lcomm cm_in, NI*4
        .lcomm cm_w, WS*NI*4
        .lcomm cm_w2, 11*256*CM_NM*4
        .lcomm cm_ms, CM_NM*4
        .lcomm cm_mp, CM_NM*4
        .lcomm cm_md, CM_NM*4
        .lcomm cm_mt, 4 << CM_MB
        .lcomm cm_msm, 32*2
        .lcomm cm_i1, 256*4
        .lcomm cm_i2, 65536*4
#if CM_APM
        .lcomm cm_a, (256+65536)*33*2
        .lcomm cm_aj, 2*4
#endif
        .lcomm cm_xl, 1024*4
        .lcomm yA, 7*4
        .lcomm src, 8
        .lcomm obuf, 8
        .lcomm slen, 4
        .lcomm onum, 4
        .lcomm sp_, 4
        .lcomm xval, 4
        .lcomm x1, 4
        .lcomm x2, 4
        .lcomm nib, 4
        .lcomm c0, 4
        .lcomm bit, 4
        .lcomm mi, 4
        .lcomm mlen, 4
        .lcomm mptr, 4
        .lcomm pos, 4
        .lcomm c4, 4
        .lcomm c8, 4
        .lcomm word, 4
        .lcomm cls, 4
        .lcomm dep, 4
        .lcomm bd, 4
        .lcomm sw, 4
        .lcomm psw, 4
        .lcomm cm_fc, 256*4
        .lcomm pm, 4
        .lcomm s2, 4
        .lcomm xf, 4
        .lcomm xs, 4
        .lcomm xo, 4
        .lcomm pi, 4
        .lcomm xh, 4
        .lcomm xp, 4
        .lcomm rt, 4
        .lcomm rt2, 4
        .lcomm xf_opc, 4
        .lcomm xf_mrm, 4
        .lcomm xf_rel, 4
        .lcomm xf_len, 4
#ifndef MC_S2_TEST
        .lcomm g_out, MC_UNCOMP_LEN
#endif
