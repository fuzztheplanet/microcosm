#!/usr/bin/env python3
"""funcorder.py ELF MAP: print a linker --section-ordering-file that lays the
payload's functions out by similarity, so the packer's context and match models
see related code side by side. On this payload that saves about 320 bytes of
stream over the linker's own order, and the code itself doesn't change. It
also moves the shader text (mc_shader_blob, then mc_shader_dict) to the end
of .rodata, so the binary tables follow the code that reads them and the text
runs in one piece to the end. That saves about 60 more bytes. Putting the
dictionary before the blob measured worse.

ELF and MAP are a first link of the payload and its -Map file. Each function's
bytes give a set of 4-byte grams, and the order is a greedy chain that starts
at the largest function. Each step takes the unplaced function that shares the
most grams with the last one placed, divided by the square root of their union
so big functions don't win every time. Ties go to the first name in sorted
order, so the output depends on the inputs alone."""
import re
import struct
import sys

def functions(elf, map_lines):
    """(name, file offset, size) of every non-empty .text.* input section."""
    phoff, = struct.unpack_from('<Q', elf, 32)
    phentsize, phnum = struct.unpack_from('<HH', elf, 54)
    loads = []
    for i in range(phnum):
        kind, _, offset, vaddr, _, filesz = struct.unpack_from('<IIQQQQ', elf, phoff + i * phentsize)
        if kind == 1: loads.append((vaddr, offset, filesz))
    out, i = [], 0
    while i < len(map_lines):
        m = re.match(r'^ (\.text\.\S+)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+))?', map_lines[i])
        if m:
            if m.group(2) is None:            # a long name: the address is on the next line
                i += 1
                fields = map_lines[i].split()
                addr, size = int(fields[0], 16), int(fields[1], 16)
            else:
                addr, size = int(m.group(2), 16), int(m.group(3), 16)
            for vaddr, offset, filesz in loads:
                if size and vaddr <= addr and addr + size <= vaddr + filesz:
                    out.append((m.group(1), addr - vaddr + offset, size))
                    break
        i += 1
    return out

SHADER_TEXT = ('.rodata.mc_shader_blob', '.rodata.mc_shader_dict')

def rodata(map_lines):
    """The names of .rodata's input sections, in the first link's order."""
    out, inside = [], False
    for line in map_lines:
        if line.startswith('.rodata '): inside = True
        elif inside and line[:1] not in ('', ' '): break
        elif inside:
            m = re.match(r'^ (\.rodata\S*)', line)
            if m and m.group(1) not in out: out.append(m.group(1))
    return out

def main():
    elf = open(sys.argv[1], 'rb').read()
    map_lines = open(sys.argv[2]).read().splitlines()
    funcs = functions(elf, map_lines)
    grams = {n: {elf[o + j:o + j + 4] for j in range(s - 3)} for n, o, s in funcs}
    size = {n: s for n, o, s in funcs}
    left = sorted(grams)
    cur = max(left, key=lambda n: (size[n], n))
    order = []
    while left:
        order.append(cur)
        left.remove(cur)
        if left:
            g = grams[cur]
            cur = max(left, key=lambda n: len(g & grams[n]) / (1 + len(g | grams[n]) ** 0.5))
    print('.text : {\n' + ''.join(f'  *({n})\n' for n in order) + '}')
    ro = rodata(map_lines)
    ro = [n for n in ro if n not in SHADER_TEXT] + [n for n in SHADER_TEXT if n in ro]
    print('.rodata : {\n' + ''.join(f'  *({n})\n' for n in ro) + '}')

if __name__ == '__main__': main()
