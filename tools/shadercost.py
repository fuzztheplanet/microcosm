#!/usr/bin/env python3
"""shadercost.py - what each shader costs in the packed demo.

Replays the packer's model (tools/cm.h, in the configuration the last pack
chose, read from pack.json) over the release payload bit by bit. It adds up
the coded bits that land on each shader's text in mc_shader_blob and on each
entry of the shared dictionary (mc_shader_dict). A shader's cost depends on
what came before it, because the model has already seen every earlier byte, so
the number is what the shader adds to the 64k file where it stands.

    python3 tools/shadercost.py [BUILD_DIR] [--save FILE] [--against FILE]

    BUILD_DIR    the demo's build directory (default build/hello-world), after
                 `make` (any PACK): .release/payload.elf, pack.json, the headers
    --save FILE  also write "name<TAB>bytes" for every shader and entry
    --against F  compare with a file written by --save: the changes per shader,
                 largest first, and in all

`make shader-report` runs it on the current build. The sums are the model's
ideal code length, and the real stream adds a byte or two of coder overhead.
"""
import argparse
import importlib.util
import json
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

# The cost replayer: cmpack's loop, writing out each byte's coded bits instead
# of the stream.
REPLAY = r"""
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#define CM_ENCODER
#include "cm.h"
int main(int argc, char **argv) {
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    cm_u8 *buf = malloc((size_t)n + 1);
    float *cost = calloc((size_t)n + 1, sizeof *cost);
    if (!buf || !cost || fread(buf, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    cm_x86t(buf, (cm_u32)n, 1);
    cm_init(buf);
    for (long i = 0; i < n; ++i)
        for (int b = 7; b >= 0; --b) {
            int y = buf[i] >> b & 1, p = cm_predict();
            cost[i] += (float)-log2((y ? p : 4096 - p) / 4096.0);
            cm_update(y);
        }
    f = fopen(argv[2], "wb");
    if (!f || fwrite(cost, sizeof *cost, (size_t)n, f) != (size_t)n || fclose(f)) return 2;
    return 0;
}
"""


def c_strings(text, name):
    """The bytes of the C array `name[] =`, initialised with string literals."""
    m = re.search(r"\b%s\[\]\s*=\s*((?:\s*\"(?:[^\"\\]|\\.)*\")*)\s*;" % name, text)
    if not m:
        raise SystemExit("shadercost: no %s in the header" % name)
    out = bytearray()
    for lit in re.findall(r"\"((?:[^\"\\]|\\.)*)\"", m.group(1)):
        i = 0
        while i < len(lit):
            ch = lit[i]
            if ch != "\\":
                out += ch.encode("latin-1")
                i += 1
                continue
            nx = lit[i + 1]
            if nx in "01234567":
                j = i + 1
                while j < len(lit) and j < i + 4 and lit[j] in "01234567":
                    j += 1
                out.append(int(lit[i + 1:j], 8))
                i = j
            else:
                out += {"n": b"\n", "t": b"\t", "\\": b"\\", "\"": b"\"", "0": b"\0"}.get(nx, nx.encode())
                i += 2
    return bytes(out)


def costs(build):
    """Per-byte costs (bits) of the release payload under its packed configuration."""
    rel = os.path.join(build, ".release")
    manifest = json.load(open(os.path.join(rel, "pack.json")))
    spec = importlib.util.spec_from_file_location("pack", os.path.join(HERE, "pack.py"))
    pack = importlib.util.module_from_spec(spec)
    sys.dont_write_bytecode = True
    spec.loader.exec_module(pack)
    defs = pack.defines(manifest["config"], manifest["code_range"])
    with tempfile.TemporaryDirectory(prefix="shadercost-") as tmp:
        src, exe, out = (os.path.join(tmp, x) for x in ("replay.c", "replay", "cost.bin"))
        with open(src, "w") as f:
            f.write(REPLAY)
        cc = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run([*cc, "-std=gnu11", "-O2", "-I", HERE, *("-D%s=%s" % kv for kv in defs.items()),
                        src, "-lm", "-o", exe], check=True)
        subprocess.run([exe, os.path.join(rel, "payload.elf"), out], check=True)
        data = open(out, "rb").read()
    return struct.unpack("%df" % (len(data) // 4), data), manifest


def report(build):
    cost, manifest = costs(build)
    payload = open(os.path.join(build, ".release", "payload.elf"), "rb").read()
    header = open(os.path.join(build, "shaders_gen.h"), encoding="latin-1").read()
    dict_bytes, blob_bytes = c_strings(header, "mc_shader_dict"), c_strings(header, "mc_shader_blob")
    names = {int(i): n for n, i in re.findall(r'"([^"]+)"\)\) return mc_shader_at\((\d+)\)', header)}
    engine = os.path.join(build, "mc_engine_shaders.h")
    if os.path.exists(engine):
        for stem, kind, i in re.findall(r"#define (\w+)_(VS|FS|CS) mc_shader_at\((\d+)\)", open(engine).read()):
            names[int(i)] = "%s.%s" % (stem.lower(), {"VS": "vert", "FS": "frag", "CS": "comp"}[kind])
    rows = {}
    for label, blob, split in (("dict", dict_bytes, "entry"), ("blob", blob_bytes, "shader")):
        at = payload.find(blob)
        if at < 0:
            raise SystemExit("shadercost: the %s is not in the payload (stale build?)" % label)
        pos = at
        for k, part in enumerate(blob.split(b"\0")[:-1]):
            name = names.get(k, "#%d" % k) if split == "shader" else "dict %d" % k
            rows[name] = (len(part), sum(cost[pos:pos + len(part) + 1]) / 8)   # with its NUL
            pos += len(part) + 1
    return rows, sum(cost) / 8, manifest["payload_bytes"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("build", nargs="?", default="build/hello-world")
    ap.add_argument("--save")
    ap.add_argument("--against")
    a = ap.parse_args()
    rows, total, real = report(a.build)
    shaders = sum(c for _, c in rows.values())
    print("%-22s %8s %8s" % ("shader", "bytes", "packed"))
    for name, (raw, c) in sorted(rows.items(), key=lambda kv: -kv[1][1]):
        print("%-22s %8d %8.1f" % (name, raw, c))
    print("%-22s %8d %8.1f   of %.0f modelled (%d B real stream)"
          % ("all shader text", sum(r for r, _ in rows.values()), shaders, total, real))
    if a.save:
        with open(a.save, "w") as f:
            f.write("".join("%s\t%.1f\n" % (n, c) for n, (_, c) in sorted(rows.items())))
    if a.against:
        old = dict((ln.split("\t")[0], float(ln.split("\t")[1])) for ln in open(a.against) if "\t" in ln)
        delta = {n: rows.get(n, (0, 0.0))[1] - old.get(n, 0.0) for n in set(rows) | set(old)}
        print("\nchanges against %s:" % a.against)
        for n, d in sorted(delta.items(), key=lambda kv: -abs(kv[1])):
            if abs(d) >= 0.5:
                print("  %-22s %+8.1f" % (n, d))
        print("  %-22s %+8.1f" % ("all shader text", sum(delta.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
