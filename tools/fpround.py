#!/usr/bin/env python3
"""fpround.py: round the float and double constants in GCC's x86-64 assembly.

GCC keeps the C code's float and double literals, and the constants it folds from
them, in an anonymous pool: .LC labels in .rodata.cst4/8/16, each carrying the
noisy mantissa of its decimal literal. Rounded to the fewest mantissa bits that
keep it within a tolerance, a constant ends in zero bits the packer predicts.

Only a constant used as floating-point everywhere is touched. Every .LC
reference to it must be an SSE scalar float or double instruction, an x87 load or
operation, or a load whose value goes nowhere else. Such a value is followed
through the function's control flow until it is overwritten, and it may only
meet those instructions, register copies, stores of its own width, call
arguments and the return value. A bitwise use (andps/xorps, the sign and abs
masks) or an integer use pins a constant, leaving it as written. So does a
taken address, a packed access, an unknown instruction, a width that is not the
entry's, or no typed use at all. NaN, infinities, zero and denormals are pinned
too. Where GCC's pool sharing makes two constants share bytes (.set A, B+4), A
gets its own copy if either changes.

The tolerance is |rounded - x| <= 2^-N |x|. N comes from --rel (default 16),
raised by --double N for double constants and by --tight ROOTS:N for the
constants of the functions ROOTS and everything they call. A constant used in
more than one class takes the strictest. Within the tolerance a constant takes
the coarsest rounding (the most trailing zero bits), with ties away from zero,
so the result depends on the input alone. The 4- and 8-byte entries, rounded or
not, are then laid out in ascending order of their bit patterns, so neighbours
share sign, exponent and top mantissa bits. Only their addresses change.
--no-sort keeps GCC's order.

  python3 tools/fpround.py [POLICY] IN.s -o OUT.s [--report FILE]
  python3 tools/fpround.py --install-as DIR [--off] [POLICY] [--report FILE]
      writes DIR/as (only when its text changes) for `gcc -BDIR/`. That script
      rewrites the .s it is handed (in an LTO link, the LTRANS unit) and runs
      the real assembler on the result. With --off it runs the real assembler
      alone.
  python3 tools/fpround.py --selftest
"""
import argparse
import fnmatch
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

# ------------------------------------------------------------ registers, operands

GPR = {}
for _n in ('ax', 'bx', 'cx', 'dx', 'si', 'di', 'bp', 'sp'):
    for _a in ('r' + _n, 'e' + _n, _n): GPR[_a] = 'r' + _n
for _n in ('a', 'b', 'c', 'd'): GPR[_n + 'l'] = GPR[_n + 'h'] = 'r' + _n + 'x'
for _n in ('si', 'di', 'bp', 'sp'): GPR[_n + 'l'] = 'r' + _n
for _i in range(8, 16):
    for _s in ('', 'd', 'w', 'b'): GPR[f'r{_i}{_s}'] = f'r{_i}'
CALLEE_SAVED = {'rbx', 'rbp', 'r12', 'r13', 'r14', 'r15'}
ARG_GPRS = {'rdi', 'rsi', 'rdx', 'rcx', 'r8', 'r9', 'rax'}      # rax: a varargs call's vector count
XMM_ARGS = {f'xmm{i}' for i in range(8)}
XMM_RETURN = {'xmm0', 'xmm1'}                                    # a struct of floats comes back in both

def reg(op):
    """'%xmm3' -> 'xmm3', '%eax' -> 'rax'; None for an operand that is not a register."""
    if not op.startswith('%'): return None
    r = op[1:]
    return r if re.fullmatch(r'[xyz]mm\d+', r) else GPR.get(r, r)

def reg_bytes(op):
    r = op[1:]
    if r[:3] in ('xmm', 'ymm', 'zmm'): return 16
    if re.fullmatch(r'[abcd][lh]|(si|di|bp|sp)l|r\d+b', r): return 1
    if re.fullmatch(r'[abcd]x|si|di|bp|sp|r\d+w', r): return 2
    if re.fullmatch(r'e..|r\d+d', r): return 4
    return 8

def split_operands(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        depth += (ch == '(') - (ch == ')')
        if ch == ',' and depth == 0: out.append(cur.strip()); cur = ''
        else: cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

def strip_comment(line):
    quoted = False
    for i, ch in enumerate(line):
        if ch == '"' and (i == 0 or line[i - 1] != '\\'): quoted = not quoted
        elif ch == '#' and not quoted: return line[:i]
    return line

LC = re.compile(r'\.LC\d+\b')
LABEL = re.compile(r'^([A-Za-z_.$][\w.$]*):\s*(.*)$')
DATA_SIZE = {'.byte': 1, '.value': 2, '.short': 2, '.word': 2, '.long': 4, '.int': 4, '.quad': 8}
PREFIXES = ('rep', 'repz', 'repe', 'repnz', 'repne', 'lock', 'notrack', 'data16')

class Insn:
    __slots__ = ('line', 'mn', 'ops')
    def __init__(self, line, mn, ops): self.line, self.mn, self.ops = line, mn, ops

class Entry:
    """A constant-pool entry: its labels and its data directives (line, directive, size)."""
    def __init__(self, section): self.section, self.labels, self.lines, self.data = section, [], [], []
    def raw(self, lines):
        out = b''
        for no, directive, size in self.data:
            if directive == '.zero': out += bytes(size); continue
            for v in split_operands(strip_comment(lines[no]).split(None, 1)[1]):
                out += (int(v, 0) & ((1 << 8 * size) - 1)).to_bytes(size, 'little')
        return out

def parse(lines):
    """-> functions (name -> [Insn], labels as Insn(':')), pool entries
    (label -> Entry), `.set` aliases (label -> (target, offset)) and the .LC
    labels referenced from anywhere but an instruction (these stay)."""
    funcs, entries, aliases, elsewhere = {}, {}, {}, set()
    is_func, func, section, previous, stack, entry, align = set(), None, '.text', '.text', [], None, None
    for no, line in enumerate(lines):
        text = strip_comment(line).strip()
        m = LABEL.match(text)
        if m:
            name, text = m.groups()
            if name in is_func: func = name; funcs[func] = []
            if section.startswith('.rodata.cst') and name.startswith('.LC'):
                if entry is None or entry.data:
                    entry = Entry(section)
                    if align is not None: entry.lines.append(align)
                entry.labels.append(name); entry.lines.append(no); entries[name] = entry
            elif func is not None and section.startswith('.text'):
                funcs[func].append(Insn(no, ':', [name]))
        if not text: continue
        head, tail = (re.split(r'\s+', text, maxsplit=1) + [''])[:2]
        align = no if head in ('.align', '.p2align', '.balign') and not m else None   # an entry's own alignment
        if head.startswith('.'):
            if head in ('.section', '.pushsection'):
                if head == '.pushsection': stack.append(section)
                previous, section, entry = section, split_operands(tail)[0], None
            elif head in ('.text', '.data', '.bss'):
                previous, section, entry = section, head, None
            elif head == '.popsection':
                section, entry = stack.pop(), None
            elif head == '.previous':
                section, previous, entry = previous, section, None
            elif head == '.type':
                parts = split_operands(tail)
                if len(parts) == 2 and parts[1] in ('@function', '%function'): is_func.add(parts[0])
            elif head == '.size' and func is not None and split_operands(tail)[0] == func:
                func = None
            elif head in ('.set', '.equ') and tail.startswith('.LC'):
                name, value = split_operands(tail)
                m = re.fullmatch(r'(\.LC\d+)\s*(?:\+\s*(\d+))?', value)
                if m: aliases[name] = (m.group(1), int(m.group(2) or 0))
                else: elsewhere.update(LC.findall(value)); elsewhere.add(name)
            elif entry is not None and head in DATA_SIZE:
                entry.data.append((no, head, DATA_SIZE[head]))
            elif entry is not None and head == '.zero':
                entry.data.append((no, head, int(tail, 0)))
            else:
                if head in ('.align', '.p2align', '.balign'): entry = None
                elsewhere.update(LC.findall(tail))
            continue
        while head in PREFIXES and tail:
            head, tail = (re.split(r'\s+', tail, maxsplit=1) + [''])[:2]
        if func is None or not section.startswith('.text'):
            elsewhere.update(LC.findall(tail)); continue
        funcs[func].append(Insn(no, head, split_operands(tail)))
    return funcs, entries, aliases, elsewhere

# ---------------------------------------------------------------- what is a float use

TYPES = {'ss': 'SF', 'sd': 'DF'}
WIDTH = {'SF': 4, 'DF': 8, 'RAW4': 4, 'RAW8': 8}
ARITH = ('add', 'sub', 'mul', 'div', 'min', 'max')
CMP = ('eq', 'lt', 'le', 'unord', 'neq', 'nlt', 'nle', 'ord')
BITWISE = {'andps', 'andnps', 'orps', 'xorps', 'andpd', 'andnpd', 'orpd', 'xorpd', 'pand', 'pandn', 'por', 'pxor'}
VECTOR_MOVES = {'movaps', 'movapd', 'movups', 'movupd', 'movdqa', 'movdqu'}
X87 = re.compile(r'f(ld|add|sub|subr|mul|div|divr|com|comp)([sl])')

def scalar(mn):
    """An SSE scalar float instruction as (form, type): 'arith' (reads both,
    writes the second), 'unary' and 'cvt' (read the first, write the second),
    'flags' (reads both), 'mask' (reads both, the second becomes a mask) and
    'toint' (reads the first, writes a general register); None otherwise."""
    if mn == 'cvtss2sd': return 'cvt', 'SF'
    if mn == 'cvtsd2ss': return 'cvt', 'DF'
    for t, ty in TYPES.items():
        if mn in [a + t for a in ARITH]: return 'arith', ty
        if mn in ('sqrt' + t, 'rcp' + t, 'rsqrt' + t): return 'unary', ty
        if mn in ('comi' + t, 'ucomi' + t): return 'flags', ty
        if mn == 'cmp' + t or mn in [f'cmp{p}{t}' for p in CMP]: return 'mask', ty
        if re.fullmatch(f'cvtt?{t}2si[lq]?', mn): return 'toint', ty
    return None

def fits(kind, ty):
    return kind == ty or WIDTH[kind] == WIDTH[ty] and kind.startswith('RAW')

def implicit_gpr(mn, ops):
    """Might the instruction read a general register it does not name?"""
    if not ops: return mn not in ('nop', 'ud2', 'hlt')
    if len(ops) == 1 and mn.rstrip('bwlq') in ('mul', 'div', 'idiv', 'imul'): return True
    return mn.startswith(('cmpxchg', 'xchg', 'xadd', 'loop', 'jrcxz', 'jecxz', 'enter', 'leave', 'syscall', 'cpuid', 'rdtsc', 'push', 'pop'))

def implicit_xmm(mn):
    return mn.startswith(('blendv', 'pblendv', 'pcmpestr', 'pcmpistr', 'vzero', 'fxsave', 'xsave', 'sha256rnds2'))

class Pin(Exception):
    pass

def show(insn):
    return f'{insn.mn} {",".join(insn.ops)}'

def access(insn, label):
    """What an instruction does with `label(%rip)`: ('use', type) for a float
    read, ('load', register, kind) for a load whose value is to be followed."""
    ops = insn.ops
    at = [i for i, o in enumerate(ops) if LC.search(o)]
    if len(at) != 1 or ops[at[0]] != label + '(%rip)': raise Pin(show(insn))
    mn = insn.mn
    form = scalar(mn)
    if form and len(ops) == 2 and at[0] == 0: return 'use', form[1]
    m = X87.fullmatch(mn)
    if m and len(ops) == 1: return 'use', 'SF' if m.group(2) == 's' else 'DF'
    if len(ops) == 2 and at[0] == 0 and ops[1].startswith('%'):
        dst, xmm = reg(ops[1]), ops[1].startswith('%xmm')
        if mn == 'movss' and xmm: return 'load', dst, 'SF'
        if mn in ('movsd', 'movlpd') and xmm: return 'load', dst, 'DF'
        if mn == 'movd' and xmm: return 'load', dst, 'RAW4'
        if mn == 'movq' and (xmm or reg_bytes(ops[1]) == 8): return 'load', dst, 'RAW8'
        if mn == 'movl' and not xmm and reg_bytes(ops[1]) == 4: return 'load', dst, 'RAW4'
    raise Pin(show(insn))

def follow(insns, labels, start, register, kind):
    """The float types the value loaded into `register` at insns[start] meets,
    following every path until the register is overwritten. Raises Pin on any
    use that is not floating-point or not understood."""
    types, seen, todo = set(), set(), [(start + 1, frozenset({register}))]
    while todo:
        n, live = todo.pop()
        while live and (n, live) not in seen:
            seen.add((n, live))
            if n >= len(insns): raise Pin('runs off the function')
            insn = insns[n]; mn, ops = insn.mn, insn.ops
            n += 1
            if mn == ':': continue
            gprs = {r for r in live if not r.startswith('xmm')}
            named = {reg(r) for r in re.findall(r'%[a-z0-9]+', ' '.join(ops))}
            if mn in ('call', 'callq') or mn in ('jmp', 'jmpq') and not ops[0].startswith('.L'):   # a call, a tail call
                if named & live: raise Pin(f'{show(insn)}: through the value')
                if live & XMM_ARGS:
                    if kind.startswith('RAW'): raise Pin('raw bits as an argument')
                    types.add(kind)
                if gprs & ARG_GPRS: raise Pin('bits in an argument register')
                live = frozenset(live & CALLEE_SAVED)
                if mn.startswith('jmp'): break
                continue
            if mn in ('ret', 'retq'):
                if live & XMM_RETURN:
                    if kind.startswith('RAW'): raise Pin('raw bits returned')
                    types.add(kind)
                if gprs: raise Pin('bits returned in a register')
                break
            if mn.startswith('j'):
                if len(ops) != 1 or ops[0] not in labels: raise Pin(show(insn))
                todo.append((labels[ops[0]], live))
                if mn == 'jmp': break
                continue
            if mn in ('ud2', 'hlt'): break
            if not named & live:
                if gprs and implicit_gpr(mn, ops): raise Pin(f'{show(insn)}: implicit registers')
                if implicit_xmm(mn): raise Pin(f'{show(insn)}: implicit registers')
                continue
            if any(reg(r) in live for o in ops if '(' in o for r in re.findall(r'%[a-z0-9]+', o)):
                raise Pin(f'{show(insn)}: the bits as an address')
            if len(ops) < 2: raise Pin(show(insn))
            src, dst = reg(ops[-2]), reg(ops[-1])
            src_live, dst_live = src in live, dst in live
            if (mn in BITWISE or mn.startswith(('xor', 'sub'))) and len(ops) == 2 and ops[0] == ops[1]:
                live = live - {dst}; continue              # the zeroing idiom: written, not read
            if mn in BITWISE: raise Pin(f'{show(insn)}: a bitwise use')
            form = scalar(mn)
            if form:
                if src_live or dst_live and form[0] in ('arith', 'flags', 'mask'):
                    if not fits(kind, form[1]): raise Pin(f'{show(insn)} reads a {kind}')
                    types.add(form[1])
                if form[0] != 'flags': live = live - {dst}
                continue
            if re.fullmatch(r'cvtsi2s[sd][lq]?', mn) and not src_live:
                live = live - {dst}; continue              # lane 0 overwritten
            moves = VECTOR_MOVES | {'movss', 'movsd', 'movlpd', 'movd', 'movq', 'movl', 'movabsq'}
            if mn not in moves or len(ops) != 2 or ops[0].startswith('$') and src_live:
                raise Pin(show(insn))
            if not src_live:                               # something else loaded over it
                if not dst.startswith('xmm') and reg_bytes(ops[1]) < 4: raise Pin(f'{show(insn)}: a partial write')
                live = live - {dst}; continue
            if dst is None:                                # a store of the value
                width = {'movss': 4, 'movd': 4, 'movl': 4, 'movsd': 8, 'movlpd': 8, 'movq': 8}.get(mn)
                if width != WIDTH[kind]: raise Pin(f'{show(insn)}: stores part of a {kind}')
                continue
            # a copy into another register
            xs, xd = src.startswith('xmm'), dst.startswith('xmm')
            if xs and xd and (mn in VECTOR_MOVES or mn == 'movq'
                              or mn == 'movss' and WIDTH[kind] == 4 or mn == 'movsd' and WIDTH[kind] == 8):
                live = live | {dst}; continue
            width = {'movd': 4, 'movl': 4, 'movq': 8}.get(mn)
            if width == WIDTH[kind] and all(o.startswith('%xmm') or reg_bytes(o) == width for o in ops):
                live = live | {dst}; continue
            raise Pin(show(insn))
    return types

def classify(funcs, entries, aliases, elsewhere):
    """label -> (type or None, why it is pinned or '', the functions using it)."""
    refs = {}
    for name, insns in funcs.items():
        for n, insn in enumerate(insns):
            if insn.mn != ':':
                for label in set(LC.findall(' '.join(insn.ops))): refs.setdefault(label, []).append((name, n))
    result = {}
    for label in list(entries) + list(aliases):
        uses = refs.get(label, [])
        users = {f for f, _ in uses}
        types = set()
        try:
            if label in elsewhere: raise Pin('referenced from data')
            for name, n in uses:
                a = access(funcs[name][n], label)
                if a[0] == 'use': types.add(a[1]); continue
                insns = funcs[name]
                labels = {i.ops[0]: k for k, i in enumerate(insns) if i.mn == ':'}
                if not a[2].startswith('RAW'): types.add(a[2])
                types |= follow(insns, labels, n, a[1], a[2])
            if len(types) != 1: raise Pin('no float use' if not types else 'used as ' + ' and '.join(sorted(types)))
            result[label] = (types.pop(), '', users)
        except Pin as e:
            result[label] = (None, str(e), users)
    return result

# ------------------------------------------------------------------ the rounding

FORMAT = {'SF': (23, 32, '<f', '<I'), 'DF': (52, 64, '<d', '<Q')}

def value(bits, ty):
    _, _, f, i = FORMAT[ty]
    return struct.unpack(f, struct.pack(i, bits))[0]

def round_bits(bits, ty, rel):
    """The coarsest rounding of a finite, normal, nonzero float or double (its
    bit pattern) within |error| <= 2^-rel |x|: (bits, low bits now zero)."""
    mant, total, _, _ = FORMAT[ty]
    sign = bits >> (total - 1) << (total - 1)
    mag, top = bits ^ sign, (1 << (total - 1 - mant)) - 1
    if not 0 < mag >> mant < top: return bits, 0
    x = value(bits, ty)
    best = bits
    for k in range(1, mant + 1):
        r = (mag + (1 << (k - 1))) >> k << k                    # nearest, ties away from zero
        if r >> mant < top and abs(value(sign | r, ty) - x) <= abs(x) * 2.0 ** -rel: best = sign | r
    tz = (best & ((1 << mant) - 1) | 1 << mant)
    return best, (tz & -tz).bit_length() - 1

def reachable(funcs, patterns):
    """The functions the patterns name and every function they call, tail-call
    or take the address of."""
    graph = {}
    for name, insns in funcs.items():
        graph[name] = {s for i in insns if i.mn != ':' for o in i.ops
                       for s in re.findall(r'[A-Za-z_][\w.$]*', o.replace('%', ' %')) if s in funcs}
    found = {f for f in funcs if any(fnmatch.fnmatchcase(f, p) for p in patterns)}
    todo = list(found)
    while todo:
        for callee in graph[todo.pop()] - found: found.add(callee); todo.append(callee)
    return found

class Policy:
    def __init__(self, rel=16, double=None, tight=(), sort=True):
        self.rel, self.double, self.tight, self.sort = rel, double, list(tight), sort   # tight: [([patterns], rel)]
    def __str__(self):
        return (f'--rel {self.rel}' + (f' --double {self.double}' if self.double is not None else '')
                + ''.join(f' --tight {",".join(p)}:{r}' for p, r in self.tight) + ('' if self.sort else ' --no-sort'))

def words(raw):
    return [f'\t.long\t{int.from_bytes(raw[i:i + 4], "little", signed=True)}' for i in range(0, len(raw), 4)]

def rewrite(text, policy):
    """-> (the new assembly, report lines, counts)."""
    if re.search(r'^\s*\.intel_syntax', text, re.M):        # operands the other way round: leave it all
        return text, ['fpround: Intel syntax, left as written'], {'SF': [0, 0], 'DF': [0, 0], 'pinned': 0}
    lines = text.split('\n')
    funcs, entries, aliases, elsewhere = parse(lines)
    info = classify(funcs, entries, aliases, elsewhere)
    classes = [(reachable(funcs, p), r) for p, r in policy.tight]

    def tolerance(labels, ty):
        users = set().union(*(info[l][2] for l in labels))
        rel = max([policy.rel] + ([policy.double] if ty == 'DF' and policy.double is not None else [])
                  + [r for found, r in classes if users & found])
        return rel, ' '.join(sorted(users))

    def report(names, ty, rel, old, new, zeros, users, note=''):
        log.append(f'{names:<12} {ty} 2^-{rel:<2} {value(old, ty)!r:>23} -> {value(new, ty)!r:<23} '
                   f'{zeros:2} zero bits{note}  [{users}]')

    out, log, original = {}, [], {}
    pool = []                     # (section, bytes, labels): the 4- and 8-byte entries, laid out anew
    counts = {'SF': [0, 0], 'DF': [0, 0], 'pinned': 0}
    for label, entry in entries.items():
        if label != entry.labels[0]: continue
        group, raw = entry.labels, entry.raw(lines)
        original[label] = raw
        sharing = [a for a, (t, _) in aliases.items() if t in group]
        kinds = {info[l][0] for l in group}
        why = next((info[l][1] for l in group if info[l][0] is None), '')
        if not why and len(kinds) != 1: why = 'its labels disagree'
        if not why and any(info[a][0] is None for a in sharing): why = 'shares bytes with ' + ','.join(sharing)
        ty = kinds.pop() if not why else None
        if ty and len(raw) != WIDTH[ty]: why = f'a {len(raw)}-byte entry used as {ty}'
        rel, users = tolerance(group, ty or 'SF')
        newraw = raw
        if why:
            counts['pinned'] += 1
            log.append(f'{",".join(group):<12} as is   {raw.hex():<23}    {why}  [{users}]')
        else:
            bits = int.from_bytes(raw, 'little')
            new, zeros = round_bits(bits, ty, rel)
            newraw = new.to_bytes(WIDTH[ty], 'little')
            counts[ty][0] += 1
            counts[ty][1] += newraw != raw
            report(",".join(group), ty, rel, bits, new, zeros, users)
        if policy.sort and entry.section in ('.rodata.cst4', '.rodata.cst8') and len(raw) == int(entry.section[-1]):
            pool.append((entry.section, newraw, group))
            for no in entry.lines + [no for no, _, _ in entry.data]: out[no] = ''
        elif newraw != raw:
            pos = 0
            for no, directive, size in entry.data:
                vals = []
                for _ in split_operands(strip_comment(lines[no]).split(None, 1)[1]):
                    vals.append(str(int.from_bytes(newraw[pos:pos + size], 'little', signed=directive != '.byte')))
                    pos += size
                out[no] = f'\t{directive}\t{", ".join(vals)}'
    # GCC's pool sharing (.set A, B+4): A gets its own copy when it or B changes
    extra = []
    for name, (target, offset) in sorted(aliases.items()):
        ty = info[name][0]
        if ty is None or target not in original: continue
        target_new = next((raw for _, raw, labels in pool if target in labels), None)
        changed = target_new is not None and target_new != original[target] or any(
            out.get(no) for no, _, _ in entries[target].data)
        mine = original[target][offset:offset + WIDTH[ty]]
        if len(mine) != WIDTH[ty]: continue
        bits = int.from_bytes(mine, 'little')
        rel, users = tolerance([name], ty)
        new, zeros = round_bits(bits, ty, rel)
        if not changed and new == bits: continue
        out[next(k for k, l in enumerate(lines) if re.fullmatch(rf'\s*\.(set|equ)\s+{re.escape(name)}\s*,.*', l))] = ''
        w = WIDTH[ty]
        section, newraw = f'.rodata.cst{w}', new.to_bytes(w, 'little')
        if policy.sort: pool.append((section, newraw, [name]))
        else: extra += [f'\t.section\t{section},"aM",@progbits,{w}', f'\t.align {w}', f'{name}:', *words(newraw)]
        report(name, ty, rel, bits, new, zeros, users, f', its own copy of {target}+{offset}')
    # the pool laid out by bit pattern: neighbours share their sign, exponent and top bits
    for section in ('.rodata.cst4', '.rodata.cst8'):
        items = sorted((p for p in pool if p[0] == section), key=lambda p: int.from_bytes(p[1], 'little'))
        if not items: continue
        w = int(section[-1])
        extra.append(f'\t.section\t{section},"aM",@progbits,{w}')
        for _, raw, labels in items:
            extra += [f'\t.align {w}', *[f'{l}:' for l in labels], *words(raw)]
    new_lines = [out.get(i, l) for i, l in enumerate(lines) if out.get(i, l) != '' or i not in out]
    if extra:
        while new_lines and not new_lines[-1].strip(): new_lines.pop()
        new_lines += extra + ['']
    head = (f'fpround: {counts["SF"][1]} of {counts["SF"][0]} float and {counts["DF"][1]} of {counts["DF"][0]} '
            f'double constants rounded, {counts["pinned"]} left as they are'
            + (f', {len(pool)} laid out by bit pattern' if policy.sort else '') + f' ({policy})')
    return '\n'.join(new_lines), [head] + log, counts

# ------------------------------------------------------------------ the interfaces

def policy_args(ap):
    ap.add_argument('--rel', type=int, default=16, metavar='N', help='|error| <= 2^-N |x| (default 16)')
    ap.add_argument('--double', type=int, metavar='N', help='at least 2^-N for double constants')
    ap.add_argument('--tight', action='append', default=[], metavar='ROOTS:N',
                    help='at least 2^-N for the constants of ROOTS (comma-separated globs) and what they call')
    ap.add_argument('--no-sort', dest='sort', action='store_false', help='keep the pool in GCC\'s order')
    ap.add_argument('--report', type=Path, help='write every pool constant and what it became')

def policy_of(a):
    tight = []
    for spec in a.tight:
        roots, _, rel = spec.rpartition(':')
        tight.append((roots.split(','), int(rel)))
    return Policy(a.rel, a.double, tight, a.sort)

def run_rewrite(src, dst, a):
    new, log, counts = rewrite(Path(src).read_text(), policy_of(a))
    Path(dst).write_text(new)
    if a.report: a.report.write_text('\n'.join(log) + '\n')
    return log[0]

def assembler(argv):
    """DIR/as: rewrite the .s inputs into temporary files, then run the real
    assembler on them."""
    split = argv.index('--')
    ap = argparse.ArgumentParser(prog='fpround.py --as')
    ap.add_argument('--as', dest='real', required=True)
    ap.add_argument('--off', action='store_true')
    policy_args(ap)
    a = ap.parse_args(argv[:split])
    args = argv[split + 1:]
    if a.off: os.execv(a.real, [a.real, *args])
    inputs = [n for n, arg in enumerate(args)
              if arg.endswith('.s') and os.path.isfile(arg) and (n == 0 or args[n - 1] != '-o')]
    temps = []
    try:
        if not inputs and '--version' not in args and not sys.stdin.isatty():   # gcc -pipe
            fd, temp = tempfile.mkstemp(suffix='.s')
            with os.fdopen(fd, 'w') as f: f.write(sys.stdin.read())
            temps.append(temp)
            args = [x for x in args if x != '-'] + [temp]
            inputs = [len(args) - 1]
        for n in inputs:
            fd, temp = tempfile.mkstemp(suffix='.s', dir=os.path.dirname(os.path.abspath(args[n])))
            os.close(fd); temps.append(temp)
            run_rewrite(args[n], temp, a)
            args[n] = temp
        return subprocess.call([a.real, *args])
    finally:
        for t in temps: os.unlink(t)

def install(a):
    """Write DIR/as, only when its text changes: it is the link's stamp too."""
    directory = a.install_as.resolve()
    path = os.pathsep.join(p for p in os.environ.get('PATH', '').split(os.pathsep)
                           if p and Path(p).resolve() != directory)
    real = shutil.which(a.assembler, path=path)
    if not real: sys.exit(f'fpround: no assembler {a.assembler!r}')
    opts = ['--as', real]
    if a.off:
        opts.append('--off')
        if a.report and a.report.exists(): a.report.unlink()    # nothing is rounded: no stale list
    else:
        opts += ['--rel', str(a.rel)] + (['--double', str(a.double)] if a.double is not None else [])
        for t in a.tight: opts += ['--tight', t]
        if not a.sort: opts.append('--no-sort')
        if a.report: opts += ['--report', str(a.report.resolve())]
    text = ('#!/bin/sh\n# gcc -B' + str(a.install_as) + '/ assembles through tools/fpround.py (Makefile: FPROUND)\n'
            'exec ' + ' '.join(shlex.quote(x) for x in [a.python, str(Path(__file__).resolve()), *opts, '--'])
            + ' "$@"\n')
    out = directory / 'as'
    if out.exists() and out.read_text() == text: return
    directory.mkdir(parents=True, exist_ok=True)
    fd, temp = tempfile.mkstemp(dir=directory)
    with os.fdopen(fd, 'w') as f: f.write(text)
    os.chmod(temp, 0o755)
    os.replace(temp, out)

def main(argv):
    if argv[:1] == ['--as']: return assembler(argv)
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input', nargs='?')
    ap.add_argument('-o', '--output')
    ap.add_argument('--install-as', type=Path, metavar='DIR')
    ap.add_argument('--assembler', default='as', help='with --install-as: the real assembler (default: as)')
    ap.add_argument('--python', default=sys.executable, help='with --install-as: the interpreter it runs')
    ap.add_argument('--off', action='store_true', help='with --install-as: assemble as written')
    ap.add_argument('--selftest', action='store_true')
    policy_args(ap)
    a = ap.parse_args(argv)
    if a.selftest: return selftest()
    if a.install_as: install(a); return 0
    if not a.input or not a.output: ap.error('IN.s and -o OUT.s are required')
    print(run_rewrite(a.input, a.output, a))
    return 0

SELFTEST_ASM = """\t.text
\t.section\t.text.f,"ax",@progbits
\t.type\tf, @function
f:
\tmovss\t.LC1(%rip), %xmm1
\tmulss\t%xmm1, %xmm0
\taddss\t.LC2(%rip), %xmm0
\tandps\t.LC3(%rip), %xmm0
\tmovss\t.LC4(%rip), %xmm2
\tandps\t%xmm2, %xmm0
\tmovl\t$.LC5, %esi
\tmovq\t.LC6(%rip), %rax
\tmovq\t%rax, 8(%rsp)
\tmovq\t.LC7(%rip), %rax
\tmovq\t%rax, 16(%rsp)
\txorl\t%eax, %eax
\tmulsd\t.LC7(%rip), %xmm3
\tmovss\t.LC8(%rip), %xmm4
\ttestl\t%edi, %edi
\tje\t.L2
\txorps\t%xmm4, %xmm4
\tjmp\t.L3
.L2:
\tmovd\t%xmm4, %eax
\taddl\t$1, %eax
.L3:
\tmovss\t.LC9(%rip), %xmm0
\tret
\t.size\tf, .-f
\t.section\t.text.g,"ax",@progbits
\t.type\tg, @function
g:
\tmovss\t.LC10(%rip), %xmm0
\tcall\th
\tmulss\t.LC11(%rip), %xmm0
\tcomiss\t.LC13(%rip), %xmm0
\tmovsd\t.LC14(%rip), %xmm1
\tmovaps\t%xmm1, %xmm2
\tmovsd\t%xmm2, 24(%rsp)
\taddss\t.LC14(%rip), %xmm0
\tret
\t.size\tg, .-g
\t.type\th, @function
h:
\tmulss\t.LC15(%rip), %xmm0
\tret
\t.size\th, .-h
\t.section\t.rodata.cst16,"aM",@progbits,16
\t.align 16
.LC3:
\t.long\t2147483647
\t.long\t0
\t.long\t0
\t.long\t0
\t.section\t.rodata.cst4,"aM",@progbits,4
\t.align 4
.LC1:
\t.long\t1036831949
\t.align 4
.LC2:
\t.long\t1078530011
\t.align 4
.LC4:
\t.long\t1050253722
\t.align 4
.LC5:
\t.long\t1067030938
\t.align 4
.LC8:
\t.long\t1061997773
\t.align 4
.LC9:
\t.long\t-1060139254
\t.align 4
.LC10:
\t.long\t1073741823
\t.align 4
.LC12:
\t.long\t1045220557
\t.align 4
.LC13:
\t.long\t2139095040
\t.align 4
.LC15:
\t.long\t1067030938
\t.section\t.rodata.cst8,"aM",@progbits,8
\t.align 8
.LC6:
\t.long\t-1717986918
\t.long\t1069128089
\t.align 8
.LC7:
\t.long\t1374389535
\t.long\t1074339512
\t.align 8
.LC14:
\t.long\t0
\t.long\t1072693248
\t.set\t.LC11,.LC7+4
\t.data
\t.quad\t.LC12
"""

def selftest():
    def bits(x, ty='SF'):
        return struct.unpack('<I', struct.pack('<f', x))[0] if ty == 'SF' else struct.unpack('<Q', struct.pack('<d', x))[0]
    # the rounding: within the tolerance, the coarsest, nothing non-finite, zero or denormal
    for x in (0.1, -0.3, 3.14159265, 1e-3, 238.06451612903228, 44100.0 / 7):
        for ty, rel in (('SF', 16), ('SF', 8), ('DF', 16), ('DF', 30)):
            b = bits(x, ty)
            r, zeros = round_bits(b, ty, rel)
            v, w = value(b, ty), value(r, ty)
            assert abs(w - v) <= abs(v) * 2.0 ** -rel, (x, ty, rel)
            assert zeros >= rel // 2 or ty == 'SF' and zeros >= 24 - rel, (x, ty, rel, zeros)
            assert round_bits(r, ty, rel)[0] == r
    assert round_bits(0x3fffffff, 'SF', 16) == (0x40000000, 23)          # the carry into the exponent
    for b in (0, 0x80000000, 0x7f800000, 0xff800000, 0x7fc00000, 0x7fffffff, 1, 0x007fffff, 0x3f800000):
        assert round_bits(b, 'SF', 8)[0] == b, hex(b)
    assert round_bits(bits(1 / 3, 'DF'), 'DF', 16)[1] >= 37
    # what is a float use
    lines = SELFTEST_ASM.split('\n')
    info = classify(*parse(lines))
    expect = {'.LC1': 'SF', '.LC2': 'SF', '.LC3': None, '.LC4': None, '.LC5': None, '.LC6': None, '.LC7': 'DF',
              '.LC8': None, '.LC9': 'SF', '.LC10': 'SF', '.LC11': 'SF', '.LC12': None, '.LC13': 'SF', '.LC14': None,
              '.LC15': 'SF'}
    got = {label: ty for label, (ty, _, _) in info.items()}
    assert got == expect, {k: (got.get(k), v) for k, v in expect.items() if got.get(k) != v}
    for label, why in (('.LC3', 'andps .LC3'), ('.LC4', 'a bitwise use'), ('.LC5', '$.LC5'), ('.LC6', 'no float use'),
                       ('.LC8', 'addl'), ('.LC12', 'from data'), ('.LC14', 'used as DF and SF')):
        assert why in info[label][1], (label, info[label][1])
    # the rewrite: pinned bytes stay, the rest within 2^-16 and ending in a zero byte, the pool
    # in bit order, the alias of a changed double its own float
    for policy in (Policy(16), Policy(16, sort=False), Policy(8, tight=[(['h'], 20)])):
        text, log, counts = rewrite(SELFTEST_ASM, policy)
        new = text.split('\n')
        funcs, entries, aliases, elsewhere = parse(new)
        old_entries = parse(lines)[1]
        assert '.LC11' not in aliases and '.LC11' in entries and len(funcs['g']) == len(parse(lines)[0]['g'])
        assert [l for l in new if l.startswith('\t') and not l.startswith(('\t.long', '\t.align', '\t.section'))] == \
               [l for l in lines if l.startswith('\t') and not l.startswith(('\t.long', '\t.align', '\t.section', '\t.set'))]
        for label, ty in expect.items():
            raw = entries[label].raw(new)
            if label == '.LC11': before = old_entries['.LC7'].raw(lines)[4:8]
            else: before = old_entries[label].raw(lines)
            if ty is None or label == '.LC13':
                assert raw == before, label
                continue
            o, n = int.from_bytes(before, 'little'), int.from_bytes(raw, 'little')
            rel = 20 if label == '.LC15' and policy.tight else policy.rel
            assert abs(value(n, ty) - value(o, ty)) <= abs(value(o, ty)) * 2.0 ** -rel, label
            assert n & 0xff == 0 or rel > 16, label
        if policy.sort:
            for section in ('.rodata.cst4', '.rodata.cst8'):
                order = [int.from_bytes(e.raw(new), 'little') for l, e in entries.items() if e.section == section]
                assert order == sorted(order), section
        assert log[0].startswith('fpround: ') and counts['pinned'] == 7
    assert rewrite('\t.intel_syntax noprefix\n' + SELFTEST_ASM, Policy(16))[0].endswith(SELFTEST_ASM)
    # the classes: what h calls is tight, and h only
    funcs = parse(lines)[0]
    assert reachable(funcs, ['g']) == {'g', 'h'} and reachable(funcs, ['h']) == {'h'}
    print('fpround: selftest passed (rounding bounds, float-use classification, pinning, aliases, pool order)')
    return 0

if __name__ == '__main__': sys.exit(main(sys.argv[1:]))
