#!/usr/bin/env python3
"""textrange.py ELF: print the file range "start end" of the ELF64's .text
section, for pack.py --code-range. The Makefile runs it on the freshly linked
payload, before elfshrink drops the section headers, which leaves the file
offsets as they were."""
import struct
import sys

def main():
    d = open(sys.argv[1], 'rb').read()
    if d[:5] != b'\x7fELF\x02': sys.exit('textrange: not an ELF64 file')
    shoff, = struct.unpack_from('<Q', d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x3a)
    if not shoff or shstrndx >= shnum: sys.exit('textrange: no section headers')
    sec = lambda i: struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize)
    names = sec(shstrndx)[4]
    for i in range(shnum):
        name, _, _, _, offset, size = sec(i)[:6]
        if d[names + name:d.index(b'\0', names + name)] == b'.text':
            print(f'0x{offset:x} 0x{offset + size:x}')
            return
    sys.exit('textrange: no .text section')

if __name__ == '__main__': main()
