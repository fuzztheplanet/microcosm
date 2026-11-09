#!/usr/bin/env python3
"""Pack a verified ELF candidate, then atomically publish it within the budget.

The payload is coded with MICROCOSM's context-mixing coder (tools/cm.h), whose
model is configured at compile time. --code-range names a file holding the
payload's .text as a file range, which tools/textrange.py writes before elfshrink
drops the section headers. The coder's x86 transform and instruction models run
over that range only, and it is compiled into the launcher too.

--quick packs with the default configuration. The full mode hill-climbs from it,
varying the context masks, extra models, counter limit, mixer rates and count,
the APMs, the mixers' warm-up and initial weights, and the match model's
shortest match. It compiles tools/cmpack.c once per candidate and keeps the
smallest stream. The search is deterministic (fixed candidate order and batch
size) and its result is cached by input and tool hash. The freestanding launcher
is then built with the winning configuration. Before anything is published, the
reference model (tools/cm.h via tools/mcstub.c) and the shipped decoder itself
(the hand-written asm in tools/s2dec.S) are compiled for the host, and each must
reproduce the input.

The launcher comes in two stages. The second, tools/s2dec.S, is the cm.h decoder
and the memfd glue written by hand in asm, linked to run at S2_ADDR. The file
carries it coded for the much smaller first stage, tools/s1dec.S, a hand-written
decoder for tools/s1.h whose models tools/s1pack.c picks (from a fixed start
with --quick). The first stage must reproduce the second on the host too.
tools/mcstub.c is the C reference model (-DMC_DECODE_TEST) and a
reference/fallback second stage.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import shlex
import struct
import subprocess
import sys
import tempfile
import time
sys.dont_write_bytecode = True
from elfshrink import verify, ElfError

HERE = Path(__file__).resolve().parent
TOOLS = ('pack.py', 'cm.h', 'cmpack.c', 'mcstub.c', 's2dec.S', 'launcher.ld', 'stage2.ld', 's1.h', 's1pack.c',
         's1dec.S', 'elfshrink.py')
CFLAGS = shlex.split('-std=gnu11 -Oz -ffreestanding -fno-builtin -fno-tree-loop-distribute-patterns '
    '-fcf-protection=none -fno-asynchronous-unwind-tables -fno-unwind-tables '
    '-fno-stack-protector -fomit-frame-pointer -fno-pic -fno-pie -malign-data=abi')
# The shipped second stage (tools/s2dec.S) is assembled with these too. The
# C-only flags do nothing to the assembler, and -fno-pic/-fno-pie keep its
# absolute addressing. The old C second stage had its GCC flags searched, but
# the asm is optimised directly for its coded size, so there's no search now.
LDFLAGS = shlex.split('-nostdlib -static -no-pie -Wl,--build-id=none -Wl,--no-warn-rwx-segments '
    '-Wl,-z,noexecstack')
RAW = '-Wl,--oformat=binary'   # a raw image: s2dec.S / the header writes its own ELF header
# The coder configuration (see cm.h): context masks, then the scalar knobs and their ranges.
# DEFAULT is the result of a long search on the current payload and model.
# --quick uses it as is, and the full search starts from it.
DEFAULT = {'masks': [0x00, 0x02, 0x03, 0x06, 0x07, 0x0c, 0x0d, 0x19, 0x32, 0x75, 0x80, 0x99, 0xa4, 0xab, 0xca],
           'xset': 15, 'lim': 15, 'lr': 9, 'lr2': 14, 'nm': 6, 'apm': 1, 'ar': 6, 'warm': 1, 'mm': 13, 'wi': 8}
LIMITS = {'xset': (0, 15), 'lim': (1, 15), 'lr': (5, 12), 'lr2': (8, 16), 'nm': (1, 6), 'apm': (0, 1),
          'ar': (4, 8), 'warm': (0, 16), 'mm': (6, 32), 'wi': (4, 24)}
MAX_MASKS = 24              # bounds the launcher's .bss (mixer weights) and decode time
BATCH, MAX_EVALS = 8, 200   # search: candidates per step, total compressions

def run(argv, **kw):
    return subprocess.run([str(x) for x in argv], check=True, **kw)

def atomic_json(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as f: json.dump(data, f, indent=2); f.write('\n')
        os.replace(temp, path)
    finally:
        if os.path.exists(temp): os.unlink(temp)

def valid(c):
    return (type(c) is dict and set(c) == {'masks', *LIMITS} and
            all(type(c[k]) is int and lo <= c[k] <= hi for k, (lo, hi) in LIMITS.items()) and
            type(c['masks']) is list and 0 < len(c['masks']) <= MAX_MASKS and
            all(type(m) is int and 0 <= m <= 255 for m in c['masks']) and
            c['masks'] == sorted(set(c['masks'])))

def defines(c, code):
    """The -D flags for configuration c and the code range [start, end)."""
    return {'CM_MASKS': ','.join(f'0x{m:02x}' for m in c['masks']), 'CM_XSET': c['xset'],
            'CM_LIM': c['lim'], 'CM_LR': c['lr'], 'CM_LR2': c['lr2'], 'CM_NM': c['nm'],
            'CM_APM': c['apm'], 'CM_AR': c['ar'], 'CM_WARM': c['warm'], 'CM_MM': c['mm'],
            'CM_WI': c['wi'],
            'CM_CS': f'0x{code[0]:x}', 'CM_CE': f'0x{code[1]:x}'}

def neighbours(c):
    """Every configuration one step from c, in a fixed order: scalar knobs, extra
    models, mask removals, then mask additions."""
    out = []
    for k, (lo, hi) in LIMITS.items():
        if k == 'xset': out += [{**c, k: c[k] ^ b} for b in (1, 2, 4, 8)]
        else: out += [{**c, k: c[k] + d} for d in (-1, 1) if lo <= c[k] + d <= hi]
    if len(c['masks']) > 1:
        out += [{**c, 'masks': [x for x in c['masks'] if x != m]} for m in c['masks']]
    if len(c['masks']) < MAX_MASKS:
        out += [{**c, 'masks': sorted(c['masks'] + [m])} for m in range(256) if m not in c['masks']]
    return out

def search(size, start, jobs):
    """Hill-climb from start: take the best of the first batch that improves on
    the current stream, until a whole neighbourhood fails or the budget is spent."""
    best, best_size = start, size(start)
    with ThreadPoolExecutor(jobs) as pool:
        moved = True
        while moved and len(size.memo) < MAX_EVALS:
            moved = False
            cands = neighbours(best)
            for i in range(0, len(cands), BATCH):
                batch = cands[i:i + BATCH]
                sizes = list(pool.map(size, batch))
                j = min(range(len(batch)), key=sizes.__getitem__)
                if sizes[j] < best_size:
                    best, best_size, moved = batch[j], sizes[j], True
                    break
                if len(size.memo) >= MAX_EVALS: break
    return best

class Coder:
    """Compress the input under a configuration (compiling cmpack for it), memoized."""
    def __init__(self, cc, work, source, code):
        self.cc, self.work, self.source, self.code, self.memo = cc, work, source, code, {}
    def stream(self, c):
        return self.work / ('stream-' + hashlib.sha256(json.dumps(c, sort_keys=True).encode()).hexdigest()[:16])
    def __call__(self, c):
        key = json.dumps(c, sort_keys=True)
        if key not in self.memo:
            exe = self.stream(c).with_name(self.stream(c).name.replace('stream', 'cmpack'))
            run([*self.cc, '-std=gnu11', '-O2', *(f'-D{k}={v}' for k, v in defines(c, self.code).items()),
                 HERE/'cmpack.c', '-o', exe])
            run([exe, self.source, self.stream(c)], capture_output=True)
            exe.unlink()
            self.memo[key] = self.stream(c).stat().st_size
        return self.memo[key]

def symbol(cc, elf, name):
    """The value of symbol name in elf, from nm (set $NM to match the compiler)."""
    nm = os.environ.get('NM', 'nm')
    for line in run([nm, elf], capture_output=True, text=True).stdout.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == name: return int(f[0], 16)
    raise RuntimeError(f'{name} missing from {elf.name}')

def check_static(data):
    """The launcher must be one self-contained ELF64 image the kernel will load:
    a single RWX PT_LOAD of the whole file, with no interpreter and no dynamic
    section, and the entry point inside it. The first stage, tools/s1dec.S, keeps
    its entry code in the ELF header itself, and launcher.ld links it as that
    image."""
    if data[:7] != b'\x7fELF\x02\x01\x01' or struct.unpack_from('<HH', data, 16) != (2, 0x3e):
        raise RuntimeError('launcher is not an x86-64 ELF64 executable')
    entry, phoff = struct.unpack_from('<QQ', data, 24)
    phentsize, phnum = struct.unpack_from('<HH', data, 54)
    if phentsize != 56 or phnum != 1: raise RuntimeError('launcher needs exactly one program header')
    kind, flags, offset, vaddr, _, filesz, memsz, align = struct.unpack_from('<IIQQQQQQ', data, phoff)
    if (kind, flags, offset, filesz) != (1, 7, 0, len(data)) or memsz < filesz or vaddr % align:
        raise RuntimeError('launcher is not one RWX PT_LOAD of the whole file')
    if not vaddr <= entry < vaddr + filesz: raise RuntimeError('launcher entry point outside its image')

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('input', type=Path)
    ap.add_argument('output', type=Path)
    ap.add_argument('--budget', type=int, default=65536)
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--cache-dir', type=Path)
    ap.add_argument('--manifest', type=Path)
    ap.add_argument('--code-range', type=Path, help='file holding the payload\'s .text range "start end"')
    a = ap.parse_args()
    if a.budget <= 0: ap.error('budget must be positive')
    cc = shlex.split(os.environ.get('CC', 'cc'))
    python = shlex.split(os.environ.get('PYTHON', sys.executable))
    source = a.input.read_bytes()
    verify(a.input)
    code = (0, 0)
    if a.code_range:
        code = tuple(int(x, 0) for x in a.code_range.read_text().split())
        if len(code) != 2 or not 0 <= code[0] < code[1] <= len(source):
            raise RuntimeError(f'bad code range {code} for a {len(source)}-byte input')
    compiler = run([*cc, '--version'], capture_output=True, text=True).stdout
    key = hashlib.sha256(source + compiler.encode() + repr(code).encode() +
                         b''.join((HERE/name).read_bytes() for name in TOOLS)).hexdigest()
    cache = a.cache_dir / (key + '.json') if a.cache_dir else None
    config = None
    if cache and cache.exists():
        try:
            cached = json.loads(cache.read_text())
            if cached.get('searched') and valid(cached['config']): config = cached['config']
        except (ValueError, KeyError, TypeError): pass
    searched = not a.quick or config is not None
    a.output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='.pack-', dir=a.output.parent) as dirname:
        work = Path(dirname).resolve()
        (work/'input').write_bytes(source)
        coder = Coder(cc, work, work/'input', code)
        run([*cc, '-std=gnu11', '-O2', HERE/'s1pack.c', '-o', work/'s1pack'])
        if any(ch in str(work) for ch in '"\\\n'): raise RuntimeError(f'unusable work path {work}')

        def build(cfg, gates):
            """Compress with cfg and build the whole launcher around the stream,
            returning (candidate bytes, stream, asm decode seconds). The shipped
            second stage is the asm (tools/s2dec.S). With gates it also checks
            that the reference model and the asm decoder reproduce the payload and
            that the first stage reproduces the second. Without gates it only
            sizes the file, for the comparison of totals below."""
            coder(cfg)
            s = coder.stream(cfg).read_bytes()
            (work/'blob').write_bytes(s)
            (work/'payload_meta.h').write_text(
                f'#define MC_UNCOMP_LEN {len(source)}u\n#define MC_BLOB_LEN {len(s)}u\n' +
                ''.join(f'#define {k} {v}\n' for k, v in defines(cfg, code).items()))
            stage2 = [*cc, *CFLAGS, '-I'+str(work), HERE/'s2dec.S', '-T', HERE/'stage2.ld', *LDFLAGS]
            run([*stage2, '-o', work/'stage2.elf'])
            run([*stage2, RAW, '-o', work/'stage2'])
            secs = 0.0
            if gates:
                # The reference model (the same cm.h the packer compressed with)
                # and the shipped asm decoder, each compiled for the host, must
                # give the payload back. A wrong model, configuration or code
                # range then fails the build, and the two can't disagree.
                run([*cc, '-std=gnu11', '-Os', '-DMC_DECODE_TEST', '-I'+str(work), HERE/'mcstub.c', '-o', work/'decode'])
                if run([work/'decode', work/'blob'], capture_output=True).stdout != source:
                    raise RuntimeError('payload decompression differs from input (reference model)')
                run([*cc, '-std=gnu11', '-Os', '-mno-red-zone', '-fno-pie', '-no-pie', '-DMC_S2_TEST',
                     '-I'+str(work), HERE/'mcstub.c', HERE/'s2dec.S', '-o', work/'s2decode'])
                start = time.monotonic()
                if run([work/'s2decode', work/'blob'], capture_output=True).stdout != source:
                    raise RuntimeError('asm second-stage decode differs from input')
                secs = time.monotonic() - start
            run([work/'s1pack', *(['-q'] if a.quick else []), work/'stage2', work/'stage1', work/'s1_meta.h'],
                capture_output=True)
            with open(work/'s1_meta.h', 'a') as f:
                f.write(f'#define MC_S1_BLOB "{work/"stage1"}"\n#define MC_BLOB "{work/"blob"}"\n')
            if gates:
                run([*cc, '-std=gnu11', '-O2', '-mno-red-zone', '-fno-pie', '-no-pie', '-DMC_S1_TEST',
                     '-I'+str(work), HERE/'mcstub.c', HERE/'s1dec.S', '-o', work/'s1test'])
                if run([work/'s1test'], capture_output=True).stdout != (work/'stage2').read_bytes():
                    raise RuntimeError('launcher second stage decompression differs')
            run([*cc, *CFLAGS, '-I'+str(work), HERE/'s1dec.S', '-T', HERE/'launcher.ld', *LDFLAGS, RAW,
                 f'-Wl,--defsym=s2_mem_end={symbol(cc, work/"stage2.elf", "s2_mem_end"):#x}', '-o', work/'launcher'])
            cand = (work/'launcher').read_bytes()
            if cand[:4] != b'\x7fELF': raise RuntimeError('candidate is not ELF')
            if gates:
                check_static(cand)
            return cand, s, secs

        if config is None:
            config = DEFAULT if a.quick else search(coder, DEFAULT, min(BATCH, os.cpu_count() or 1))
        # The search above minimises the stream, but a few knobs also change the
        # decoder's own size. The APMs, the mixer warm-up, the sixth mixer and
        # the extra-model set (CM_XSET) are compiled in only when used, so their
        # best value for the stream may not be best for the whole file. Those
        # are settled by building the launcher for the search winner and for its
        # neighbours in those knobs, and keeping the smallest. The other knobs
        # (masks, limit, mixer rates and count) cost the decoder nothing, so for
        # them the smallest stream gives the smallest file.
        if not a.quick:
            variants = [config, {**config, 'apm': config['apm'] ^ 1},
                        {**config, 'warm': 0 if config['warm'] else 12}, {**config, 'nm': min(config['nm'], 5)}]
            variants += [{**config, 'xset': config['xset'] ^ (1 << b)} for b in range(4)]
            seen, totals = set(), {}
            for cfg in variants:
                key = json.dumps(cfg, sort_keys=True)
                if key in seen or not valid(cfg): continue
                seen.add(key)
                totals[key] = (len(build(cfg, gates=False)[0]), cfg)
            config = min(totals.values(), key=lambda t: t[0])[1]
        candidate, stream, seconds = build(config, gates=True)
        if len(candidate) > a.budget:
            raise RuntimeError(f'{len(candidate)} bytes exceeds {a.budget}-byte budget')
        (work/'launcher').chmod(0o755)
        os.replace(work/'launcher', work/'candidate')
        manifest = dict(input_sha256=hashlib.sha256(source).hexdigest(),
            packed_sha256=hashlib.sha256(candidate).hexdigest(), input_bytes=len(source),
            payload_bytes=len(stream), packed_bytes=len(candidate),
            launcher_bytes=len(candidate) - len(stream), budget=a.budget, packer='cm',
            config=config, code_range=list(code), searched=searched, candidates=len(coder.memo),
            decode_seconds=round(seconds, 3), compiler=compiler.strip())
        # Metadata and cache failures also leave the existing deliverable intact.
        if cache and searched: atomic_json(cache, {'config': config, 'searched': True})
        if a.manifest: atomic_json(a.manifest, manifest)
        os.replace(work/'candidate', a.output)
        print(f'MICROCOSM: {len(candidate)} / {a.budget} bytes; {a.budget-len(candidate)} free; '
              f'round-trip OK ({seconds:.2f} s decode); stream {len(stream)} + launcher '
              f'{len(candidate)-len(stream)} bytes; {len(config["masks"])} masks'
              f'{", searched " + str(len(coder.memo)) + " candidates" if len(coder.memo) > 1 else ""}')

if __name__ == '__main__':
    try: main()
    except (OSError, RuntimeError, ElfError, subprocess.CalledProcessError) as e:
        print(f'pack failed: {e}; no candidate published (any previous output is unchanged)', file=sys.stderr)
        sys.exit(1)
