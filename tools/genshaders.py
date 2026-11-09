#!/usr/bin/env python3
"""genshaders.py - bake the demo's and the engine's shaders into C headers.

Dev builds read the demo's GLSL straight off disk so you can hot-reload it.
Release builds can't ship loose files, so this script minifies every shader
with glslmin.py and writes them all into one header as a single blob. main.c
#includes it under MC_RELEASE and takes each program's sources by index, with
mc_shader_at() and the mc_program_shaders table (in programs.def order), so it
never touches the filesystem. The tools look shaders up by file name instead:

    static const char *mc_demo_shader(const char *name);

returns the minified source for "object.vert", "object.frag" and so on. Text
that several shaders share (the jungle's common block, helpers) is stored once
and expanded again on first use (see share() below).

With --engine, the engine's own shaders (the post passes in src/shaders) are
minified in the same run and written to a second header, which render.c
#includes in every build. Each is named after its file: post.vert -> POST_VS,
bright.frag -> BRIGHT_FS. When the run has the demo's shaders as well, the
engine's go into the demo's blob and under MC_RELEASE those names expand to
mc_shader_at() calls. Otherwise they are plain arrays.

All shaders in one run share a single rename mapping (glslmin's --rename), so
an identifier gets the same short name in every file. That keeps varyings
matching across stages, and code repeated between files stays repeated for the
compressor. Uniform and block names are never renamed. --raw skips the
minifier, so the dev build's engine header keeps readable source.

    python3 tools/genshaders.py demos/hello-world/shaders -o build/shaders_gen.h \\
        --engine src/shaders --engine-output build/mc_engine_shaders.h

The output depends only on the shaders' contents and names and on the program
manifests.
"""
import argparse
import re
import importlib.util
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
GLSLMIN = os.path.join(HERE, "glslmin.py")
EX = (".vert", ".frag", ".comp", ".glsl")
SUFFIX = {".vert": "_VS", ".frag": "_FS", ".comp": "_CS", ".glsl": "_GLSL"}


def c_escape(s: str) -> str:
    out = []
    for ch in s:
        if ch == "\\":
            out.append("\\\\")
        elif ch == '"':
            out.append('\\"')
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            pass
        elif 32 <= ord(ch) < 127:
            out.append(ch)
        else:   # Octal, because a \x escape would swallow a following hex digit.
                # 0x80-0xff are share()'s one-byte references, and higher is UTF-8.
            out.extend("\\%03o" % b for b in (bytes([ord(ch)]) if ord(ch) < 256 else ch.encode("utf-8")))
    return "".join(out)


def minify(path: str, strict: bool = True) -> str:
    """Minify one file in a child process (no renaming)."""
    try:
        res = subprocess.run([sys.executable, GLSLMIN, path],
                             capture_output=True, text=True, check=True)
        return res.stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as e:
        if strict:
            detail = getattr(e, "stderr", None) or str(e)
            raise RuntimeError("minification failed for %s: %s" % (path, detail)) from e
        sys.stderr.write("genshaders: minify failed for %s (%s); using raw source\n"
                         % (path, e))
        with open(path, "r", encoding="utf-8") as f:
            return f.read()


FUNC = re.compile(r"\b(?:float|vec[234]|mat[234]|void|bool|int|ivec[234])\s+(\w+)\(([^()]*)\)\{")


def arity(text, o):
    """Count the arguments in the bracket opened at text[o] (minified text)."""
    d, n, i = 0, 0, o + 1
    while True:
        c = text[i]
        if c in "([{":
            d += 1
        elif c in ")]}":
            if d == 0:
                return n + (text[o + 1:i].strip() not in ("", "void"))
            d -= 1
        elif c == "," and d == 0:
            n += 1
        i += 1


def drop_dead_functions(text):
    """Remove every function the program never calls, directly or through
    another live function. The shared blocks bring in helpers a given stage
    doesn't use. Unused #defines go first, so a function that only such a
    macro calls goes too (a fragment-only helper in a block both stages share,
    for example). glslmin overloads by argument count, so functions may share
    a name, and a call only counts for the definition with the same number of
    arguments. This works on minified text, where definitions have a regular
    shape."""
    text = "".join(ln for ln in text.splitlines(True) if not ln.startswith("#define ") or
                   len(re.findall(r"\b%s\b" % re.escape(re.match(r"#define (\w+)", ln).group(1)), text)) > 1)
    while True:
        for m in FUNC.finditer(text):
            name, n = m.group(1), arity(text, m.start(2) - 1)
            if name == "main" or any(arity(text, c.end() - 1) == n for c in
                                     re.finditer(r"\b%s\(" % re.escape(name), text)
                                     if c.start() != m.start(1)):
                continue
            depth, end = 0, m.end() - 1
            while True:
                depth += {"{": 1, "}": -1}.get(text[end], 0)
                if depth == 0:
                    break
                end += 1
            text = text[:m.start()] + text[end + 1:]
            break
        else:
            return text


def minify_all(paths, rename=True, strict=True):
    """Minify {key: path} together, with one shared mapping.

    Returns ({key: text}, mapping). Any failure is fatal unless strict is off,
    in which case every file falls back to its raw source.
    """
    sources = {}
    for key, path in paths.items():
        with open(path, "r", encoding="utf-8") as f:
            sources[key] = f.read()
    try:
        sys.dont_write_bytecode = True        # no __pycache__ in the source tree
        spec = importlib.util.spec_from_file_location("glslmin", GLSLMIN)
        glslmin = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(glslmin)
        texts, mapping = glslmin.minify_set(sources, rename=rename)
        return {k: drop_dead_functions(t) for k, t in texts.items()}, mapping
    except Exception as e:  # noqa: BLE001 - fail closed on anything
        if strict:
            raise RuntimeError("minification failed: %s" % e) from e
        sys.stderr.write("genshaders: minify failed (%s); using raw sources\n" % e)
        return sources, {}


def write_if_changed(path, text):
    """Write atomically, but leave the file and its mtime alone if unchanged."""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    previous = None
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            previous = f.read()
    if previous == text:
        return
    fd, candidate = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(path)))
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(candidate, path)
    finally:
        if os.path.exists(candidate):
            os.unlink(candidate)


def shader_files(directory):
    if not directory or not os.path.isdir(directory):
        return []
    return sorted(f for f in os.listdir(directory) if f.endswith(EX))


def program_pairs(shader_dir):
    """Return the (vs, fs) file pairs from programs.def and the manifests it
    includes, in the order the C X-macros see them. MC_PROGRAM and MC_BAKE
    entries are treated the same."""
    import re
    out = []
    def walk(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                inc = re.match(r'\s*#include\s+"([^"]+)"', line)
                if inc:
                    walk(os.path.join(os.path.dirname(path), inc.group(1)))
                    continue
                m = re.search(r'"([^"]+)"\s*,\s*"([^"]+)"', line)
                if m and re.match(r'\s*MC_(PROGRAM|BAKE)\(', line):
                    out.append((m.group(1), m.group(2)))
    manifest = os.path.join(os.path.dirname(os.path.abspath(shader_dir)), "programs.def")
    if os.path.exists(manifest):
        walk(manifest)
    return out


def blob_order(files, texts):
    """Put the fragment shaders after the rest (vertex and compute), largest
    first within each group. Of the orders we measured (by stage, by package,
    in pairs, by size), this one packed smallest, by about 60 bytes."""
    return sorted(files, key=lambda n: (n.endswith(".frag"), -len(texts[n]), n))


def top_level(text):
    """Cut a minified shader into its top-level pieces: preprocessor lines,
    declarations (up to their ';') and function definitions (up to their
    '}')."""
    out, depth, start, i = [], 0, 0, 0
    while i < len(text):
        c = text[i]
        if c == "#" and depth == 0 and (i == 0 or text[i - 1] == "\n"):
            if start < i:
                out.append(text[start:i])
            start = text.index("\n", i) + 1
            out.append(text[i:start])
            i = start
            continue
        depth += {"{": 1, "}": -1}.get(c, 0)
        if depth == 0 and (c in ";\n" or c == "}" and text[i + 1:i + 2] != ";"):
            out.append(text[start:i + 1])
            start = i + 1
        i += 1
    if start < len(text):
        out.append(text[start:])
    assert "".join(out) == text
    return out


def share(texts, gain=0.04, cost=1.0):
    """Store text that the shaders share only once. A run of top-level pieces
    (a block of declarations, a helper) that repeats in several shaders is
    replaced by one byte, 0x80 + its index in the returned list, which the
    header's mc_shader_at expands again. The packer's match model already
    makes a repeat cheap, about `gain` bytes of coded stream per byte, while a
    reference costs `cost` bytes. Each round takes the run that saves the most
    by that measure, and a run may contain earlier references. It stops when
    no run would save anything or 128 entries are taken.
    Returns (entries, texts with the references)."""
    assert all(t.isascii() for t in texts), "share() needs ASCII shaders"
    seqs, entries = [top_level(t) for t in texts], []
    while len(entries) < 128:
        runs = {}
        for q, seq in enumerate(seqs):
            for i in range(len(seq)):
                for m in range(1, min(60, len(seq) - i) + 1):
                    runs.setdefault(tuple(seq[i:i + m]), []).append((q, i))
        best, run = 0.0, None
        for key, at in runs.items():
            n, end = 0, {}
            for q, i in at:                 # non-overlapping occurrences
                if i >= end.get(q, -1):
                    n, end[q] = n + 1, i + len(key)
            score = (n - 1) * sum(map(len, key)) * gain - n * cost - 2
            if n > 1 and score > best:
                best, run = score, key
        if run is None:
            break
        ref = chr(0x80 + len(entries))
        entries.append("".join(run))
        for q, seq in enumerate(seqs):
            out, i = [], 0
            while i < len(seq):
                if tuple(seq[i:i + len(run)]) == run:
                    out.append(ref)
                    i += len(run)
                else:
                    out.append(seq[i])
                    i += 1
            seqs[q] = out
    return entries, ["".join(seq) for seq in seqs]


def demo_header(files, texts, shader_dir, engine=None):
    """Build the demo's header. With *engine* ({file: text}), the engine's
    shaders join the blob and its dictionary under the keys "engine/<file>".
    The post passes share their uniform block and helpers with each other.
    mc_shader_at then gets external linkage, so render.c can reach them
    through engine_header_shared's macros."""
    texts = dict(texts)
    texts.update({"engine/" + fn: t for fn, t in (engine or {}).items()})
    order = (blob_order(list(files), texts)                # engine after demo packs 60 B smaller
             + blob_order(["engine/" + fn for fn in (engine or {})], texts))   # than interleaved or first
    index = {fn: i for i, fn in enumerate(order)}
    entries, shared = share([texts[fn] for fn in order])
    lines = [
        "/* Generated by genshaders.py - do not edit. */",
        "#ifndef MC_SHADERS_GEN_H",
        "#define MC_SHADERS_GEN_H",
        "#include <string.h>",
        "",
        "/* What the shaders share, kept once, NUL-separated: a byte from 0x80 up in",
        " * the blob (or in here) stands for entry (byte - 0x80). */",
        "static const char mc_shader_dict[] =",
    ]
    for e in entries:
        lines.append('    "%s\\0"' % c_escape(e))
    lines.append('    "";')
    lines.append("/* Every shader, NUL-separated, in one blob. */")
    lines.append("static const char mc_shader_blob[] =")
    for t in shared:
        lines.append('    "%s\\0"' % c_escape(t))
    lines.append("    ;")
    lines.append("/* Copy s to o, the shared text spelled out; returns the end. */")
    lines.append("static char *mc_shader_spell(char *o, const char *s) {")
    lines.append("    for (; *s; ++s)")
    lines.append("        if (*s & 128) {")
    lines.append("            const char *d = mc_shader_dict;")
    lines.append("            for (int i = *s & 127; i--; ) while (*d++) {}")
    lines.append("            o = mc_shader_spell(o, d);")
    lines.append("        } else *o++ = *s;")
    lines.append("    return o;")
    lines.append("}")
    lines.append("/* Shader i, from the blob spelled out in full on first use. */")
    lines.append("%sconst char *mc_shader_at(int i) {" % ("" if engine else "static "))
    lines.append("    static char text[%d];" % (sum(len(texts[fn]) + 1 for fn in order) + 1))
    lines.append("    if (!*text) {")
    lines.append("        char *o = text;")
    lines.append("        for (const char *s = mc_shader_blob; *s; ) { o = mc_shader_spell(o, s) + 1; while (*s++) {} }")
    lines.append("    }")
    lines.append("    const char *s = text;")
    lines.append("    while (i--) while (*s++) {}")
    lines.append("    return s;")
    lines.append("}")
    lines.append("/* Each program's (vertex, fragment) blob indices, in programs.def order. */")
    pairs = program_pairs(shader_dir)
    lines.append("static const unsigned char mc_program_shaders[][2] = {%s};"
                 % ",".join("{%d,%d}" % (index[v], index[f]) for v, f in pairs))
    lines.append("/* By file name (the tools; the demo itself goes by index, and the payload")
    lines.append(" * imports no strcmp). */")
    lines.append("#ifndef MC_IMPORT_HASH")
    lines.append("static inline const char *mc_demo_shader(const char *name) {")
    for fn in order:
        if not fn.startswith("engine/"):
            lines.append('    if (!strcmp(name, "%s")) return mc_shader_at(%d);' % (fn, index[fn]))
    lines.append('    return "";')
    lines.append("}")
    lines.append("#endif")
    lines.append("")
    lines.append("#endif /* MC_SHADERS_GEN_H */")
    return "\n".join(lines) + "\n", index


def engine_header_shared(files, texts, index):
    """Build the engine's header for when its shaders live in the demo's blob.
    In a release build each name is a call that expands the blob
    (shaders_gen.h defines mc_shader_at). Anything else built against these
    minified headers without the demo's, such as the dev-mode checks, still
    gets plain strings."""
    lines = [
        "/* Generated by genshaders.py - do not edit. */",
        "#ifndef MC_ENGINE_SHADERS_H",
        "#define MC_ENGINE_SHADERS_H",
        "",
        "#ifdef MC_RELEASE   /* in the demo's shader blob, sharing its dictionary (shaders_gen.h) */",
        "const char *mc_shader_at(int i);",
    ]
    for fn in files:
        stem, ext = os.path.splitext(fn)
        lines.append("#define %s mc_shader_at(%d)" % (stem.upper().replace("-", "_") + SUFFIX[ext],
                                                     index["engine/" + fn]))
    lines.append("#else")
    lines += engine_header(files, texts).split("\n")[4:-3]
    lines.append("#endif")
    lines.append("")
    lines.append("#endif /* MC_ENGINE_SHADERS_H */")
    return "\n".join(lines) + "\n"


def engine_header(files, texts):
    lines = [
        "/* Generated by genshaders.py - do not edit. */",
        "#ifndef MC_ENGINE_SHADERS_H",
        "#define MC_ENGINE_SHADERS_H",
        "",
    ]
    for fn in files:
        stem, ext = os.path.splitext(fn)
        sym = stem.upper().replace("-", "_") + SUFFIX[ext]
        lines.append('static const char %s[] = "%s";' % (sym, c_escape(texts[fn])))
    lines.append("")
    lines.append("#endif /* MC_ENGINE_SHADERS_H */")
    return "\n".join(lines) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir", nargs="?", help="directory of the demo's shader files")
    ap.add_argument("-o", "--output", help="demo header path")
    ap.add_argument("--engine", help="directory of the engine's shader files")
    ap.add_argument("--engine-output", help="engine header path")
    ap.add_argument("--raw", action="store_true", help="embed sources verbatim (dev engine header)")
    ap.add_argument("--no-rename", action="store_true", help="minify without the shared renaming")
    ap.add_argument("--emit-dir", help="also write every minified shader to this directory")
    ap.add_argument("--map", help="also write the rename mapping (original short) to this file")
    ap.add_argument("--allow-raw", action="store_true", help="explicit development fallback on minifier failure")
    ap.add_argument("--allow-empty", action="store_true", help="emit an empty lookup for a demo without loose shaders")
    args = ap.parse_args()
    if bool(args.dir) != bool(args.output) or bool(args.engine) != bool(args.engine_output):
        ap.error("give DIR with -o and/or --engine with --engine-output")
    if not args.output and not args.engine_output:
        ap.error("nothing to generate")

    demo = shader_files(args.dir) if args.output else []
    engine = shader_files(args.engine) if args.engine_output else []
    if args.output and not demo and not args.allow_empty:
        sys.stderr.write("genshaders: no shaders found in %s\n" % args.dir)
        return 1
    if args.engine_output and not engine:
        sys.stderr.write("genshaders: no shaders found in %s\n" % args.engine)
        return 1
    if args.emit_dir and set(demo) & set(engine):
        sys.stderr.write("genshaders: --emit-dir needs distinct demo and engine file names\n")
        return 1

    paths = {"demo/" + fn: os.path.join(args.dir, fn) for fn in demo}
    paths.update({"engine/" + fn: os.path.join(args.engine, fn) for fn in engine})
    if args.raw:
        texts, mapping = {}, {}
        for key, path in paths.items():
            with open(path, "r", encoding="utf-8") as f:
                texts[key] = f.read()
    else:
        texts, mapping = minify_all(paths, rename=not args.no_rename,
                                    strict=not args.allow_raw)

    # A release build that also has the demo's shaders keeps the engine's in
    # the same blob, under one dictionary. Otherwise each header stands alone.
    joint = bool(demo and engine and args.output and args.engine_output and not args.raw)
    index = None
    for files, prefix, output, make in ((demo, "demo/", args.output, demo_header),
                                        (engine, "engine/", args.engine_output, engine_header)):
        if not output:
            continue
        own = {fn: texts[prefix + fn] for fn in files}
        if make is demo_header:
            text, index = demo_header(files, own, args.dir,
                                      {fn: texts["engine/" + fn] for fn in engine} if joint else None)
        else:
            text = engine_header_shared(files, own, index) if joint else engine_header(files, own)
        write_if_changed(output, text)
        total_raw = sum(os.path.getsize(paths[prefix + fn]) for fn in files)
        sys.stderr.write("genshaders: %d shaders, %d -> %d bytes %s -> %s\n"
                         % (len(files), total_raw, sum(len(t) for t in own.values()),
                            "raw" if args.raw else "minified", output))
        if args.emit_dir:
            for fn, text in own.items():
                write_if_changed(os.path.join(args.emit_dir, fn), text)
    if args.map:
        write_if_changed(args.map, "".join("%s %s\n" % kv for kv in sorted(mapping.items())))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except RuntimeError as e:
        sys.stderr.write("genshaders: %s\n" % e)
        raise SystemExit(1)
