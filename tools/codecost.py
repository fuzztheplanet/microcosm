#!/usr/bin/env python3
"""codecost.py - what each function costs in the packed demo.

This is the code-side counterpart of shadercost.py. It replays the packer's
model (tools/cm.h, in the configuration the last pack chose, read from
pack.json) over the release payload bit by bit in the same way. Here the coded
bits are added up per function and per named array instead of per shader.

The payload is built with -ffunction-sections and -fdata-sections, so in its
link map (payload.map, which the link rule keeps) every function and every
named array has an input section of its own. That gives the exact bytes of
payload.elf each one covers. Bytes that no name covers are summed in groups:
the float constant pool the compiler emits (.rodata.cst4/8/16), string
literals, unnamed .rodata, alignment padding, and the ELF headers, dynamic
tables and gaps. Each name is also traced to the source file that defines it,
for per-file totals. As with shaders, a function's cost depends on everything
before it in the file, so the number is what that function adds.

    python3 tools/codecost.py [BUILD_DIR] [--top N] [--save FILE] [--against FILE]

    BUILD_DIR    the demo's build directory (default build/hello-world), after
                 `make` (any PACK): .release/payload.elf, payload.map, pack.json
    --top N      list the N costliest functions and arrays (default 40, 0 for all)
    --save FILE  also write "name<TAB>bytes<TAB>file" for every row
    --against F  compare with a file written by --save: the changes per row,
                 largest first, per source file, and in all

`make code-report` runs it on the current build. The sums are the model's ideal
code length, and the real stream adds a byte or two of coder overhead.
"""
import argparse
import glob
import importlib.util
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SKIP = {"if", "for", "while", "switch", "return", "sizeof", "defined", "__attribute__", "__typeof__",
        "__asm__", "_Static_assert", "typedef"}


def module(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    m = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(m)
    return m


def definitions(paths):
    """Map each name defined in the given sources to its files, {name: [files]}.

    It finds functions defined at the start of a line (the project's style),
    arrays of unstated size, and anything initialised with braces or a string.
    If a .c file defines a name, matches for it in headers are dropped."""
    defs = {}
    for path in paths:
        for line in open(path, encoding="latin-1"):
            code = re.sub(r"/\*.*?(\*/|$)|//.*", "", line).rstrip()
            names = re.findall(r"\b([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*=\s*[{\"]", code)
            names += re.findall(r"\b([A-Za-z_]\w*)\s*\[\s*\](?:\s*\[[^\]]*\])*\s*=", code)
            if re.match(r"[A-Za-z_]", code) and not (code.endswith(";") and "{" not in code):   # no prototype
                calls = [n for n in re.findall(r"\b([A-Za-z_]\w*)\s*\(", code) if n not in SKIP]
                if calls and not calls[0].isupper():                                        # nor a macro
                    names.append(calls[0])
            for n in names:
                defs.setdefault(n, [])
                if path not in defs[n]:
                    defs[n].append(path)
    return {n: [f for f in fs if f.endswith(".c")] or fs for n, fs in defs.items()}


def dependencies(rel):
    """The sources and headers the payload's objects were compiled from, read
    from their -MMD files. System headers are left out."""
    out = set()
    for d in glob.glob(os.path.join(rel, "**", "*.d"), recursive=True):
        for f in open(d).read().replace("\\\n", " ").split()[1:]:
            path = os.path.normpath(os.path.join(ROOT, f))
            if path.startswith(ROOT + os.sep) and not f.endswith(":"):
                out.add(path)
    return sorted(out)


def loads(elf):
    """(vaddr, offset, filesz) of every PT_LOAD."""
    phoff, = struct.unpack_from("<Q", elf, 32)
    phentsize, phnum = struct.unpack_from("<HH", elf, 54)
    out = []
    for i in range(phnum):
        kind, _, offset, vaddr, _, filesz = struct.unpack_from("<IIQQQQ", elf, phoff + i * phentsize)
        if kind == 1:
            out.append((vaddr, offset, filesz))
    return out


def report(build):
    cost, manifest = module("shadercost").costs(build)
    rel = os.path.join(build, ".release")
    elf = open(os.path.join(rel, "payload.elf"), "rb").read()
    entries = module("importcheck").link_map(open(os.path.join(rel, "payload.map")).read())
    segs = loads(elf)

    def offset(addr, size):
        for vaddr, off, filesz in segs:
            if vaddr <= addr and addr + size <= vaddr + filesz:
                return addr - vaddr + off
        return None

    start, end = (int(x, 0) for x in open(os.path.join(rel, "payload.range")).read().split())
    covered = bytearray(len(elf))
    rows = {}    # name -> [bytes, bits, file]
    defs = definitions(dependencies(rel))

    def where(name, obj):
        if obj.endswith(".o") and ".ltrans" not in obj:      # not through LTO, so the object tells us
            src = os.path.relpath(os.path.join(ROOT, obj), os.path.abspath(rel))[:-2] + ".c"
            return src if os.path.exists(os.path.join(ROOT, src)) else obj
        base = re.sub(r"\.(isra|constprop|part|cold|lto_priv|localalias)(\.\d+)?", "", name)
        base = re.sub(r"\.\d+$", "", base)                    # a static variable inside a function
        files = defs.get(base, [])
        return os.path.relpath(files[0], ROOT) if len(files) == 1 else "?" if not files else "? (%d files)" % len(files)

    def add(name, off, size, src):
        row = rows.setdefault(name, [0, 0.0, src])
        row[0] += size
        row[1] += sum(cost[off:off + size])
        covered[off:off + size] = b"\1" * size

    for out, sec, addr, size, obj, syms in entries:
        if out not in (".text", ".rodata") or not size:
            continue
        off = offset(addr, size)
        if off is None:
            raise SystemExit("codecost: %s is not in the payload (stale build?)" % sec)
        if sec == "*fill*":
            add("(alignment padding)", off, size, "")
        elif sec.startswith(".text."):
            name = re.sub(r"^\.text\.((startup|unlikely|hot|exit)\.)?", "", sec)
            add(name, off, size, where(name, obj))
        elif sec == ".text":
            add(" ".join(syms) or "(.text of %s)" % obj, off, size, where("", obj))
        elif re.match(r"\.rodata\.cst\d+$", sec):
            add("(constant pool: .rodata.cst*)", off, size, "")
        elif ".str1." in sec:
            add("(string literals)", off, size, "")
        elif sec == ".rodata":
            add("(unnamed .rodata: switch tables etc.)", off, size, "")
        elif sec.startswith(".rodata."):
            name = sec[8:]
            add(name, off, size, "" if name.startswith("CSWTCH.") else where(name, obj))
    code = [(offset(e[2], e[3]), e[3]) for e in entries if e[0] == ".text" and e[3]]
    if (min(o for o, _ in code), max(o + n for o, n in code)) != (start, end):
        raise SystemExit("codecost: payload.map does not match payload.range (stale build?)")
    rest = [i for i, c in enumerate(covered) if not c]
    rows["(ELF headers, dynamic tables, gaps)"] = [len(rest), sum(cost[i] for i in rest), ""]
    for row in rows.values():
        row[1] /= 8
    return rows, sum(cost) / 8, manifest["payload_bytes"]


def per_file(rows):
    out = {}
    for _, c, src in rows.values():
        out[src or "(the groups)"] = out.get(src or "(the groups)", 0.0) + c
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", nargs="?", default="build/hello-world")
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--save")
    ap.add_argument("--against")
    a = ap.parse_args()
    rows, total, real = report(a.build)
    named = {n: r for n, r in rows.items() if not n.startswith("(")}
    groups = {n: r for n, r in rows.items() if n.startswith("(")}
    shown = sorted(named.items(), key=lambda kv: -kv[1][1])
    if a.top:
        shown = shown[:a.top]
    print("%-40s %7s %8s  %s" % ("function / array", "bytes", "packed", "defined in"))
    for name, (raw, c, src) in shown:
        print("%-40s %7d %8.1f  %s" % (name, raw, c, src))
    if len(shown) < len(named):
        rest = [r for n, r in named.items() if n not in dict(shown)]
        print("%-40s %7d %8.1f" % ("... %d more" % len(rest), sum(r[0] for r in rest), sum(r[1] for r in rest)))
    print()
    for name, (raw, c, _) in sorted(groups.items(), key=lambda kv: -kv[1][1]):
        print("%-40s %7d %8.1f" % (name, raw, c))
    print("\n%-48s %8s" % ("per source file", "packed"))
    for src, c in sorted(per_file(rows).items(), key=lambda kv: -kv[1]):
        print("%-48s %8.1f" % (src, c))
    print("\n%-40s %7d %8.1f   modelled (%d B real stream)"
          % ("all of payload.elf", sum(r[0] for r in rows.values()), total, real))
    if a.save:
        with open(a.save, "w") as f:
            f.write("".join("%s\t%.1f\t%s\n" % (n, c, s) for n, (_, c, s) in sorted(rows.items())))
    if a.against:
        old = {}
        for ln in open(a.against):
            f = ln.rstrip("\n").split("\t")
            if len(f) >= 2:
                old[f[0]] = [0, float(f[1]), f[2] if len(f) > 2 else ""]
        delta = {n: rows.get(n, [0, 0.0])[1] - old.get(n, [0, 0.0])[1] for n in set(rows) | set(old)}
        print("\nchanges against %s:" % a.against)
        for n, d in sorted(delta.items(), key=lambda kv: -abs(kv[1])):
            if abs(d) >= 0.5:
                print("  %-40s %+8.1f" % (n, d))
        new_f, old_f = per_file(rows), per_file(old)
        print("  per source file:")
        for src in sorted(set(new_f) | set(old_f), key=lambda s: -abs(new_f.get(s, 0) - old_f.get(s, 0))):
            d = new_f.get(src, 0.0) - old_f.get(src, 0.0)
            if abs(d) >= 0.5:
                print("    %-46s %+8.1f" % (src, d))
        print("  %-40s %+8.1f" % ("all", sum(delta.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
