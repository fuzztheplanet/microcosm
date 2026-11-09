#!/usr/bin/env python3
"""Check that the shipped asm second stage (tools/s2dec.S) decodes like the
reference model (tools/cm.h) on more than the one payload pack.py already checks.

The inputs are a real ELF, structured text, GLSL-shaped text, incompressible
random bytes and a tiny buffer, each with and without a code range. For each
of a small set of coder configurations, every input is coded with cmpack (the
host half of cm.h) and then decoded twice: with the C reference (mcstub.c
-DMC_DECODE_TEST, which replays cm.h exactly) and with the hand-written asm
(mcstub.c -DMC_S2_TEST + s2dec.S). Every decode must give back the input, so
that the asm and the model can't disagree for any configuration pack.py can
choose.

Run it directly or through tools/checks/tooling.py. It only needs a C toolchain.
"""
import os
import shlex
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / 'tools'

# A spread of configurations. The APMs, the warm-up and the extra-model set are
# only compiled in when used, and the configurations cover both cases. The mask
# count, mixer count, rates, counter limit, shortest match and initial weights
# vary too. LIM 1 drives the counters hardest.
CONFIGS = [
    dict(MASKS='0x00,0x02,0x03,0x05,0x06,0x0c,0x19,0x32,0x61,0x8a,0xab,0xc5',
         XSET=13, LIM=14, LR=9, LR2=13, NM=5, APM=1, AR=6, WARM=12, MM=12, WI=12),
    dict(MASKS='0x00,0x03,0x05,0x06', XSET=13, LIM=14, LR=9, LR2=13, NM=6, APM=0, AR=6, WARM=0, MM=6, WI=4),
    dict(MASKS='0x00,0x01,0x03', XSET=0, LIM=1, LR=7, LR2=12, NM=1, APM=1, AR=7, WARM=10, MM=32, WI=24),
    dict(MASKS='0x00,0x02,0x06,0x0c,0x19', XSET=15, LIM=12, LR=6, LR2=16, NM=4, APM=1, AR=8, WARM=16, MM=9, WI=16),
]


def inputs(d):
    out = []
    # A real ELF: the small argv/env fixture.
    elf = d / 'fixture'
    subprocess.run([*CC, TOOLS / 'checks/launcher.c', '-o', elf], check=True)
    out.append(elf)
    # Structured text, incompressible random, a tiny buffer.
    (d / 'text.bin').write_bytes(b'the quick brown fox 12345 _ident foo(bar);\n' * 400)
    out.append(d / 'text.bin')
    import random
    r = random.Random(20260501)
    # Text shaped like minified GLSL, since the text models track brackets,
    # commas, braces, statements and NULs. The tail is left unbalanced.
    words = ['vec3', 'float', 'mix', 'x', 'y', 'smoothstep', 'return', 'if', 'a', '.5', '1.', 'dot']
    glsl = []
    for i in range(300):
        w = [r.choice(words) for _ in range(8)]
        glsl.append(f'{w[0]} {w[1]}({w[2]} {w[3]},{w[4]} {w[5]}){{{w[6]}={w[7]}({w[3]},{w[5]}*({w[0]}+{w[2]}));'
                    f'return {w[6]};}}' + ('\0#version 450 core\n' if i % 37 == 0 else ''))
    (d / 'glsl.bin').write_bytes(''.join(glsl).encode() + b'((,,{{;' + bytes(range(256)))
    out.append(d / 'glsl.bin')
    (d / 'rand.bin').write_bytes(bytes(r.randrange(256) for _ in range(9000)))
    out.append(d / 'rand.bin')
    (d / 'tiny.bin').write_bytes(bytes(range(256)) * 2)
    out.append(d / 'tiny.bin')
    return out


def defs(c, cs, ce):
    return [f'-DCM_MASKS={c["MASKS"]}', *(f'-DCM_{k}={c[k]}' for k in ('XSET', 'LIM', 'LR', 'LR2', 'NM', 'APM', 'AR', 'WARM', 'MM', 'WI')),
            f'-DCM_CS={cs}', f'-DCM_CE={ce}']


def main():
    global CC
    CC = shlex.split(os.environ.get('CC', 'cc'))
    cases = 0
    with tempfile.TemporaryDirectory(prefix='microcosm-s2equiv-') as dirname:
        d = Path(dirname)
        ins = inputs(d)
        for src in ins:
            data = src.read_bytes()
            n = len(data)
            for cs, ce in ((0, 0), (16, n // 2)):
                for i, c in enumerate(CONFIGS):
                    meta = d / 'payload_meta.h'
                    # cmpack for this configuration, then encode.
                    cmp_exe = d / 'cmpack'
                    subprocess.run([*CC, '-std=gnu11', '-O2', '-I', TOOLS, *defs(c, cs, ce),
                                    TOOLS / 'cmpack.c', '-o', cmp_exe], check=True)
                    blob = d / 'blob'
                    subprocess.run([cmp_exe, src, blob], check=True, capture_output=True)
                    meta.write_text(f'#define MC_UNCOMP_LEN {n}u\n#define MC_BLOB_LEN {blob.stat().st_size}u\n' +
                                    ''.join(f'#define CM_{k} {c[k]}\n' for k in ('XSET', 'LIM', 'LR', 'LR2', 'NM', 'APM', 'AR', 'WARM', 'MM', 'WI')) +
                                    f'#define CM_MASKS {c["MASKS"]}\n#define CM_CS {cs}\n#define CM_CE {ce}\n')
                    # Build the C reference model and the asm decoder, both for the host.
                    cdec, adec = d / 'cdec', d / 'adec'
                    subprocess.run([*CC, '-std=gnu11', '-Os', '-I', d, '-I', TOOLS, '-DMC_DECODE_TEST',
                                    TOOLS / 'mcstub.c', '-o', cdec], check=True)
                    subprocess.run([*CC, '-std=gnu11', '-Os', '-mno-red-zone', '-fno-pie', '-no-pie', '-I', d, '-I', TOOLS,
                                    '-DMC_S2_TEST', TOOLS / 'mcstub.c', TOOLS / 's2dec.S', '-o', adec], check=True)
                    cout = subprocess.run([cdec, blob], check=True, capture_output=True).stdout
                    aout = subprocess.run([adec, blob], check=True, capture_output=True).stdout
                    tag = f'{src.name} cfg{i} cs={cs} ce={ce}'
                    assert cout == data, f'C reference model does not round-trip: {tag}'
                    assert aout == data, f'asm decoder does not round-trip: {tag}'
                    assert aout == cout, f'asm decoder differs from the model: {tag}'
                    cases += 1
    print(f'Second stage: the asm decoder reproduces every input and agrees with the cm.h model '
          f'({cases} cases: {len(CONFIGS)} configs x {len(ins)} inputs x 2 code ranges)')


if __name__ == '__main__':
    main()
