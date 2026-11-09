#!/usr/bin/env python3
"""importcheck.py - check the payload resolves only what it calls, and finds it.

Before main, the payload fills two tables. mcgl gets every GL entry point in
mc_gl.h's MC_GL_FUNCS, looked up by name through eglGetProcAddress, and mc_imp
gets every library function in mc_imports.h's MC_IMPORTS, looked up by name
hash in src/mc_start.c. An entry that nothing calls still costs its name or
hash in the packed file, so this script walks the freshly linked payload's code
with the packer's own x86 decoder (cm_xfield, tools/cm.h). If a slot in either
table is never reached, it fails and names the unused entries. A use is a
RIP-relative operand, since that's how GCC reaches a member of a global. The
loaders' indexed stores into the tables don't count, and neither does a lea,
which takes an address rather than making a call.

It also rejects the import hash width (MC_IMPORT_HASH_BITS, mc_imports.h) if
an import's truncated hash matches any other symbol the resolver would take
from the libraries the payload maps on this machine. Those are the DT_NEEDED
libraries and their own dependencies, as the payload's dynamic loader lists
them (PT_INTERP --list).

    python3 tools/importcheck.py ELF MAP [--warn]

ELF is the second link of the payload, which still has its section headers,
and MAP is its -Map file. With --warn the script only lists unused entries,
which is what the Makefile's IMPORTCHECK=warn does for a demo that needs less
of the engine than hello-world. A hash collision always fails.
"""
import argparse
import importlib.util
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The walker steps through the code one instruction at a time, as the packer's
# x86 transform does, and prints the opcode and target address of every
# [rip + disp32] operand (ModRM mod 0, rm 5).
WALK = r"""
#include <stdio.h>
#include <stdlib.h>
#define CM_TB 1
#include "cm.h"
int main(int argc, char **argv) {
    FILE *f = argc == 5 ? fopen(argv[1], "rb") : NULL;
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    cm_u8 *b = calloc((size_t)n + 16, 1);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) return 2;
    unsigned long i = strtoul(argv[2], 0, 0), end = strtoul(argv[3], 0, 0), base = strtoul(argv[4], 0, 0) - i;
    while (i < end) {
        cm_u32 o, m;
        int r, l;
        cm_xfield(b + i, 15, &o, &m, &r, &l);
        if (r >= 0 && (m & 0xc7) == 5 && r + 4 <= l)
            printf("%x %lx\n", o, base + i + l + (long)(int)(b[i + r] | b[i + r + 1] << 8 | b[i + r + 2] << 16 | (cm_u32)b[i + r + 3] << 24));
        i += (unsigned long)l;
    }
    return 0;
}
"""


def module(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(m)
    return m


def sections(d):
    """{name: (address, file offset, size, type, link)} from an ELF64's section headers."""
    if d[:5] != b"\x7fELF\x02":
        raise SystemExit("importcheck: not an ELF64 file")
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3a)
    if not shoff or shstrndx >= shnum:
        raise SystemExit("importcheck: no section headers")
    hdr = [struct.unpack_from("<IIQQQQIIQQ", d, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdr[shstrndx][4]
    return {d[strtab + h[0]:d.index(b"\0", strtab + h[0])].decode(): (h[3], h[4], h[5], h[1], h[6])
            for h in hdr}, hdr


def link_map(text):
    """Parse every input section of a GNU ld -Map file into [output section,
    name, address, size, object, symbols]. A long name pushes its address onto
    the next line, and *fill* is the padding between sections. Global symbols
    the map lists under a section are kept with it. The discarded sections
    listed before the memory map are skipped."""
    out, sec, wrapped = [], None, None
    lines = text.splitlines()
    start = next((i for i, ln in enumerate(lines) if ln.startswith("Linker script and memory map")), 0)
    for line in lines[start:]:
        if wrapped:
            f = line.split()
            out.append([sec, wrapped, int(f[0], 16), int(f[1], 16), " ".join(f[2:]), []])
            wrapped = None
            continue
        m = re.match(r"(\.\S+)", line)
        if m:
            sec = m.group(1)
            continue
        m = re.match(r" (\.\S+|\*fill\*)(?:\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s*(.*))?", line)
        if m:
            if m.group(2) is None:
                wrapped = m.group(1)
            else:
                out.append([sec, m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip(), []])
            continue
        m = re.match(r"\s+0x([0-9a-f]+)\s+([A-Za-z_][\w.$]*)\s*$", line)
        if m and out and out[-1][2] <= int(m.group(1), 16) < out[-1][2] + max(out[-1][3], 1):
            out[-1][5].append(m.group(2))
    return out


def table(entries, name):
    """(address, size) of the global table `name` in the map."""
    for _, sec, addr, size, _, syms in entries:
        if size and (sec == ".bss." + name or name in syms):
            return addr, size
    raise SystemExit("importcheck: no %s in the link map" % name)


def gl_names(text):
    m = re.search(r"#define\s+MC_GL_FUNCS\(X\)((?:[^\n]*\\\n)*[^\n]*)", text)
    if not m:
        raise SystemExit("importcheck: no MC_GL_FUNCS(X) list in mc_gl.h")
    return re.findall(r"\bX\((\w+),", m.group(1))


def dead_slots(path, d, entries):
    """[(list, [unused names])] for mcgl and mc_imp."""
    text = sections(d)[0].get(".text")
    if not text:
        raise SystemExit("importcheck: no .text section")
    with tempfile.TemporaryDirectory(prefix="importcheck-") as tmp:
        src, exe = os.path.join(tmp, "walk.c"), os.path.join(tmp, "walk")
        with open(src, "w") as f:
            f.write(WALK)
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run([*cc, "-std=gnu11", "-O1", "-I", HERE, src, "-o", exe], check=True)
        out = subprocess.run([exe, path, hex(text[1]), hex(text[1] + text[2]), hex(text[0])],
                             check=True, capture_output=True, text=True).stdout
    used = {int(t, 16) for o, t in (ln.split() for ln in out.splitlines()) if o != "8d"}
    lists = (("GL entry points (mc_gl.h, MC_GL_FUNCS)", "mcgl",
              gl_names(open(os.path.join(ROOT, "include/microcosm/mc_gl.h")).read())),
             ("imports (mc_imports.h, MC_IMPORTS)", "mc_imp",
              module("genimports").import_names(open(os.path.join(ROOT, "include/microcosm/mc_imports.h")).read())))
    result = []
    for label, sym, names in lists:
        addr, size = table(entries, sym)
        if size != 8 * len(names):
            raise SystemExit("importcheck: %s holds %d slots but the list names %d" % (sym, size // 8, len(names)))
        slots = {(t - addr) // 8 for t in used if addr <= t < addr + size}
        result.append((label, names, [n for k, n in enumerate(names) if k not in slots]))
    return result


def exported(path):
    """Return the names the resolver would take from a library: defined
    functions, indirect functions and objects whose version is the default
    one (not hidden)."""
    d = open(path, "rb").read()
    secs, hdr = sections(d)
    versym = next((s[1] for s in secs.values() if s[3] == 0x6fffffff), None)   # SHT_GNU_versym
    out = set()
    for _, offset, size, kind, link in secs.values():
        if kind != 11:                                                         # SHT_DYNSYM
            continue
        strtab = hdr[link][4]
        for i in range(size // 24):
            st_name, st_info, _, st_shndx = struct.unpack_from("<IBBH", d, offset + 24 * i)
            if not st_shndx or st_info & 15 not in (1, 2, 10):                 # object, func, ifunc
                continue
            if versym is not None and struct.unpack_from("<H", d, versym + 2 * i)[0] & 0x8000:
                continue
            out.add(d[strtab + st_name:d.index(b"\0", strtab + st_name)].decode())
    return out


def collisions(path, d, names, bits):
    """Return the libraries the payload maps, how many names they export, and
    every (import, other name, library) whose hashes match in the low `bits`."""
    interp = sections(d)[0].get(".interp")
    if not interp:
        raise SystemExit("importcheck: no PT_INTERP to ask for the payload's libraries")
    loader = d[interp[1]:d.index(b"\0", interp[1])].decode()
    listing = subprocess.run([loader, "--list", os.path.abspath(path)], check=True,
                             capture_output=True, text=True).stdout
    libs = re.findall(r"(/\S+) \(0x[0-9a-f]+\)", listing)   # the vDSO has no path, and the resolver skips it
    genimports = module("genimports")
    mask = (1 << bits) - 1
    want = {genimports.gnu_hash(n) & mask: n for n in names}
    found, seen = [], set()
    for lib in libs:
        for s in exported(lib):
            seen.add(s)
            n = want.get(genimports.gnu_hash(s) & mask)
            if n and n != s:
                found.append((n, s, lib))
    return libs, len(seen), found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf")
    ap.add_argument("map")
    ap.add_argument("--warn", action="store_true", help="only list unused entries")
    a = ap.parse_args()
    d = open(a.elf, "rb").read()
    entries = link_map(open(a.map).read())
    dead = dead_slots(a.elf, d, entries)
    bits = module("genimports").hash_bits(open(os.path.join(ROOT, "include/microcosm/mc_imports.h")).read())
    libs, exports, clash = collisions(a.elf, d, dead[1][1], bits)
    status = 0
    if clash:
        sys.stderr.write("importcheck: at MC_IMPORT_HASH_BITS %d these imports share their hash with another"
                         " name the payload's libraries export:\n" % bits)
        for n, s, lib in sorted(clash):
            sys.stderr.write("  %s = %s (%s)\n" % (n, s, lib))
        sys.stderr.write("raise MC_IMPORT_HASH_BITS in include/microcosm/mc_imports.h (32 is the whole hash)\n")
        status = 1
    if any(unused for _, _, unused in dead):
        sys.stderr.write("importcheck: the payload resolves these at startup but never calls them:\n")
        for label, _, unused in dead:
            if unused:
                sys.stderr.write("  %s: %s\n" % (label, ", ".join(unused)))
        sys.stderr.write("move a GL entry point into MC_GL_TOOLS(X) and drop an import's X() and #define%s\n"
                         % (" (IMPORTCHECK=warn: not failing)" if a.warn else ", or build with IMPORTCHECK=warn"))
        return status or (0 if a.warn else 1)
    if not status:
        print("importcheck: {} GL entry points and {} imports, every one called; no {}-bit import hash"
              " shared with another of the {:,} names {} libraries export"
              .format(len(dead[0][1]), len(dead[1][1]), bits, exports, len(libs)))
    return status


if __name__ == "__main__":
    sys.exit(main())
