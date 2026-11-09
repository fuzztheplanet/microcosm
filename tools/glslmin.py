#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""glslmin.py - a GLSL minifier that will not change what a shader does.

The job is to make the demo's GLSL smaller so it compresses better inside the
64k executable. The hard rule is that behaviour never changes, and nothing the
C side looks up by string gets renamed. That means uniforms, block names, and
attributes and outputs it binds by name. A wrong rename breaks the demo
silently at runtime, so every transform below is either provably safe or
conservative enough to give up some compression.

    python3 glslmin.py [options] INPUT.glsl [-o OUTPUT.glsl]
    python3 glslmin.py --rename [options] A.vert B.frag ... --outdir DIR

    INPUT           input file(s); "-" reads one shader from stdin
    -o, --output    output file for a single input; stdout if omitted
    --outdir DIR    write each input to DIR/<its file name> (several inputs)
    --stats         print original vs minified byte counts to stderr
    --rename        rename identifiers with one mapping shared by all inputs,
                    and fold repeated declarations (see below)
    --protect A,B   extra identifiers --rename must leave alone
    --map FILE      write the --rename mapping (original new) to FILE
    --int-arith     with --rename, also turn integral literals inside * / + -
                    into ints (see int_literals: drivers may round differently)
    --selftest      run the built-in tests and exit (nonzero on failure)

The default pass is always on and always safe:
  - Strips // and /* */ comments. GLSL has no string literals, so the comment
    scanner has nothing to be careful about. A block comment becomes a single
    space, so it can never join two tokens. A line comment is removed but its
    newline stays.
  - Keeps every preprocessor line (`#...`) on its own line, never merged into
    code, with the whitespace inside squeezed to single spaces.
    Backslash-newline continuations are spliced first, so a multi-line
    #define ends up on one line.
  - Collapses all other whitespace to the minimum. There's no hand-written
    table of which tokens need a space between them: the pass re-tokenizes the
    joined text and only inserts a space where leaving it out would lex
    differently. Guards stop '/' '/' becoming a comment and a trailing '.'
    sticking to a leading one.
  - Shortens float literals in the simple, safe way: 1.0 -> 1., 0.5 -> .5,
    10.00 -> 10., 1.0e3 -> 1.e3. Exponents and integer literals are untouched.
  - Ends the output with a single newline.

--rename processes every input together. genshaders.py feeds it a demo's and
the engine's shaders in one go, and a file's extension (.vert or .frag) gives
its stage. Renaming is plain alpha-renaming, an injective substitution whose
new names collide with nothing, so it can't change behaviour as long as
nothing outside the shader knows the old names. These stay as they are:
  - Names the C side looks up by name: uniforms without layout(location) or
    layout(binding), every interface-block name, and vertex inputs and
    fragment outputs without layout(location). Also anything passed to
    --protect. A uniform whose location or unit is fixed in the shader is set
    by number, so it gets renamed like any other global.
  - Built-ins, keywords, reserved words, gl_* and names containing "__".
  - Anything a shader uses without declaring it, such as a built-in this tool
    doesn't know or a macro the C side would prepend. To be safe, such a name
    is kept in every file.
  - Swizzles and every other name after '.', except declared struct and block
    members. Those are renamed both in their declarations and after '.'.
  - #version/#extension/#pragma/#line lines, and any #define that uses # or ##.

Everything else is renamed: functions, parameters, locals, globals and consts,
varyings (the same way in both stages), struct types, members of structs and
uniform blocks, block instances and macros. Renaming block members is safe
because std140 blocks are filled by offset, never by member name. File-scope
names share one mapping across all files. A function's parameters and locals
are numbered in order of first use within that function, and local number k
gets the same name in every function. That way a helper copied between files
stays byte-identical for the compressor. Functions whose names aren't used for
anything else can share a name if their argument counts never meet in one
shader, because a call can't confuse such overloads. Each kind of name has its
own family: a first letter, then a running character. Locals are numbered Vx,
Vy, Vz, Vw, Va, ..., functions are Ux, Uy, ..., struct and block members are
Tx, Ty, ..., and the rest of the file-scope names are Sx, Sy, .... Within each
family the most used name comes first.

Swizzles are all written in one alphabet, xyzw (`.rgb` becomes `.xyz`), unless
a member has the same spelling as the swizzle. A local that is read exactly
once is folded into that read (inline_locals: `float b=E;...b...` ->
`...E...`), but only where it can't make a difference. The type and block must
be the same, there must be no write in between, and the target spot must not
let the compiler's expression rewriting see more of E than before. Brackets
the parse doesn't need are removed (drop_brackets). Functions a shader never
calls are dropped before anything is counted.

--rename also merges consecutive declarations with the same qualifiers and
type into one list (`uniform float a;uniform float b;` -> `uniform float
a,b;`), which GLSL evaluates the same as the separate statements. It also
writes integral float literals as ints where GLSL's implicit int->float
conversion gives the same value and the same arithmetic. Those places are
constructor arguments, float initializers and assignments, comparisons with a
float operand, one branch of a float ?:, and parameters of built-in and user
functions that only take floats (`vec3(0.,1.,0.)` -> `vec3(0,1,0)`). It works
out operand types from the declarations, per function, and leaves alone any
literal it can't type.

It fails closed. Source it can't follow (unbalanced brackets, anonymous or
function-local structs, an unrecognised `NAME {`) raises RenameError instead
of guessing. Renamed shaders should still be compiled and linked after every
change. tools/verify.py links every minified program pair.

Requires Python 3.8+, standard library only.
"""

import argparse
import itertools
import os
import re
import string
import sys
from collections import namedtuple

# GLSL's reserved vocabulary as plain identifier sets, so the --rename pass
# knows what it must never touch. To the lexer, keywords are identifiers like
# any other, which is why they sit here and not in the tokenizer.

GLSL_KEYWORDS = {
    # storage / qualifiers
    "attribute", "const", "uniform", "varying", "buffer", "shared",
    "coherent", "volatile", "restrict", "readonly", "writeonly",
    "layout", "centroid", "flat", "smooth", "noperspective",
    "patch", "sample", "in", "out", "inout", "invariant", "precise",
    "precision", "highp", "mediump", "lowp",
    # control flow
    "break", "continue", "do", "for", "while", "switch", "case",
    "default", "if", "else", "return", "discard",
    # misc
    "true", "false", "struct", "subroutine",
}

GLSL_TYPES = {
    "void", "bool", "int", "uint", "float", "double",
    "vec2", "vec3", "vec4",
    "bvec2", "bvec3", "bvec4",
    "ivec2", "ivec3", "ivec4",
    "uvec2", "uvec3", "uvec4",
    "dvec2", "dvec3", "dvec4",
    "mat2", "mat3", "mat4",
    "mat2x2", "mat2x3", "mat2x4",
    "mat3x2", "mat3x3", "mat3x4",
    "mat4x2", "mat4x3", "mat4x4",
    "dmat2", "dmat3", "dmat4",
    "dmat2x2", "dmat2x3", "dmat2x4",
    "dmat3x2", "dmat3x3", "dmat3x4",
    "dmat4x2", "dmat4x3", "dmat4x4",
    # samplers / textures
    "sampler1D", "sampler2D", "sampler3D", "samplerCube",
    "sampler1DArray", "sampler2DArray", "samplerCubeArray",
    "sampler1DShadow", "sampler2DShadow", "samplerCubeShadow",
    "sampler1DArrayShadow", "sampler2DArrayShadow", "samplerCubeArrayShadow",
    "sampler2DRect", "sampler2DRectShadow", "samplerBuffer",
    "sampler2DMS", "sampler2DMSArray",
    "isampler1D", "isampler2D", "isampler3D", "isamplerCube",
    "isampler1DArray", "isampler2DArray", "isamplerCubeArray",
    "isampler2DRect", "isamplerBuffer", "isampler2DMS", "isampler2DMSArray",
    "usampler1D", "usampler2D", "usampler3D", "usamplerCube",
    "usampler1DArray", "usampler2DArray", "usamplerCubeArray",
    "usampler2DRect", "usamplerBuffer", "usampler2DMS", "usampler2DMSArray",
    # images
    "image1D", "image2D", "image3D", "imageCube", "imageBuffer",
    "image1DArray", "image2DArray", "imageCubeArray", "image2DRect",
    "image2DMS", "image2DMSArray",
    "iimage1D", "iimage2D", "iimage3D", "iimageCube", "iimageBuffer",
    "uimage1D", "uimage2D", "uimage3D", "uimageCube", "uimageBuffer",
    "atomic_uint",
}

GLSL_BUILTIN_FUNCS = {
    # angle & trig
    "radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan",
    "sinh", "cosh", "tanh", "asinh", "acosh", "atanh",
    # exponential
    "pow", "exp", "log", "exp2", "log2", "sqrt", "inversesqrt",
    # common
    "abs", "sign", "floor", "trunc", "round", "roundEven", "ceil", "fract",
    "mod", "modf", "min", "max", "clamp", "mix", "step", "smoothstep",
    "isnan", "isinf", "floatBitsToInt", "floatBitsToUint",
    "intBitsToFloat", "uintBitsToFloat", "fma", "frexp", "ldexp",
    # packing
    "packUnorm2x16", "packSnorm2x16", "packUnorm4x8", "packSnorm4x8",
    "unpackUnorm2x16", "unpackSnorm2x16", "unpackUnorm4x8", "unpackSnorm4x8",
    "packHalf2x16", "unpackHalf2x16", "packDouble2x32", "unpackDouble2x32",
    # geometric
    "length", "distance", "dot", "cross", "normalize", "faceforward",
    "reflect", "refract",
    # matrix
    "matrixCompMult", "outerProduct", "transpose", "determinant", "inverse",
    # vector relational
    "lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual",
    "equal", "notEqual", "any", "all", "not",
    # integer
    "uaddCarry", "usubBorrow", "umulExtended", "imulExtended",
    "bitfieldExtract", "bitfieldInsert", "bitfieldReverse",
    "bitCount", "findLSB", "findMSB",
    # texture
    "textureSize", "textureQueryLod", "textureQueryLevels", "textureSamples",
    "texture", "textureProj", "textureLod", "textureOffset", "texelFetch",
    "texelFetchOffset", "textureProjOffset", "textureLodOffset",
    "textureProjLod", "textureProjLodOffset", "textureGrad",
    "textureGradOffset", "textureProjGrad", "textureProjGradOffset",
    "textureGather", "textureGatherOffset", "textureGatherOffsets",
    # legacy texture
    "texture1D", "texture2D", "texture3D", "textureCube",
    "texture2DLod", "textureCubeLod", "texture2DProj", "shadow2D",
    # derivatives / fragment
    "dFdx", "dFdy", "dFdxCoarse", "dFdxFine", "dFdyCoarse", "dFdyFine",
    "fwidth", "fwidthCoarse", "fwidthFine",
    "interpolateAtCentroid", "interpolateAtSample", "interpolateAtOffset",
    # noise (deprecated but still reserved)
    "noise1", "noise2", "noise3", "noise4",
    # geometry / tessellation / compute
    "EmitVertex", "EndPrimitive", "EmitStreamVertex", "EndStreamPrimitive",
    "barrier", "memoryBarrier", "memoryBarrierAtomicCounter",
    "memoryBarrierBuffer", "memoryBarrierShared", "memoryBarrierImage",
    "groupMemoryBarrier",
    # atomics / images
    "atomicAdd", "atomicMin", "atomicMax", "atomicAnd", "atomicOr",
    "atomicXor", "atomicExchange", "atomicCompSwap",
    "atomicCounter", "atomicCounterIncrement", "atomicCounterDecrement",
    "imageLoad", "imageStore", "imageSize", "imageAtomicAdd",
    "imageAtomicMin", "imageAtomicMax", "imageAtomicAnd", "imageAtomicOr",
    "imageAtomicXor", "imageAtomicExchange", "imageAtomicCompSwap",
}

GLSL_BUILTIN_VARS = {
    "gl_Position", "gl_PointSize", "gl_ClipDistance", "gl_CullDistance",
    "gl_FragCoord", "gl_FrontFacing", "gl_FragDepth", "gl_PointCoord",
    "gl_FragColor", "gl_FragData", "gl_SampleID", "gl_SampleMask",
    "gl_SampleMaskIn", "gl_SamplePosition",
    "gl_VertexID", "gl_InstanceID", "gl_DrawID", "gl_BaseVertex",
    "gl_BaseInstance",
    "gl_PrimitiveID", "gl_PrimitiveIDIn", "gl_InvocationID", "gl_Layer",
    "gl_ViewportIndex",
    "gl_in", "gl_out", "gl_TessCoord", "gl_TessLevelOuter",
    "gl_TessLevelInner", "gl_PatchVerticesIn", "gl_MaxPatchVertices",
    "gl_NumWorkGroups", "gl_WorkGroupSize", "gl_WorkGroupID",
    "gl_LocalInvocationID", "gl_GlobalInvocationID", "gl_LocalInvocationIndex",
    # legacy
    "gl_ModelViewMatrix", "gl_ProjectionMatrix", "gl_ModelViewProjectionMatrix",
    "gl_NormalMatrix", "gl_Vertex", "gl_Normal", "gl_Color",
    "gl_MultiTexCoord0", "gl_TexCoord",
}

# Qualifiers that may precede a type in a local declaration.
_LOCAL_QUALIFIERS = {"const", "highp", "mediump", "lowp", "precise"}

# token model and lexer

T_PP = "pp"        # preprocessor directive line (text already normalized)
T_ID = "id"        # identifier (incl. keywords/types lexically)
T_FLOAT = "float"  # float literal (text already shortened)
T_INT = "int"      # integer literal
T_OP = "op"        # operator / punctuation / stray char

Token = namedtuple("Token", "type text")

# One master regex for code tokens (no comments, no preprocessor lines).
# Order matters: multi-char operators go before single-char ones, and float
# before int so "1.0" lexes as one float and not int "1" + "." + "0".
_CODE_TOKEN_RE = re.compile(
    r"""
      (?P<float>
            (?:\d+\.\d*|\.\d+)(?:[eE][+-]?\d+)?(?:lf|LF|f|F)?   # 1.  1.5  .5  1.e3
          | \d+[eE][+-]?\d+(?:lf|LF|f|F)?                        # 1e3  (exponent, no dot)
      )
    | (?P<int> 0[xX][0-9a-fA-F]+[uU]? | \d+[uU]? )
    | (?P<id> [A-Za-z_][A-Za-z0-9_]* )
    | (?P<op>
            <<=|>>=|<<|>>|<=|>=|==|!=|&&|\|\||\+\+|--|\+=|-=|\*=|/=|%=|&=|\|=|\^=
          | [-+*/%<>=!&|^~?:.,;(){}\[\]]
      )
    | (?P<ws> \s+ )
    | (?P<other> . )
    """,
    re.VERBOSE,
)


def normalize_newlines(src):
    """Normalize CRLF / CR to LF for deterministic line handling."""
    return src.replace("\r\n", "\n").replace("\r", "\n")


def splice_lines(src):
    """Remove backslash-newline continuations (C translation phase 2)."""
    return src.replace("\\\n", "")


def strip_comments(src):
    """Remove // line comments and /* */ block comments.

    GLSL has no string literals, so there's no quoting to respect.  A block
    comment is replaced by a single space, as in C, so it can never join two
    tokens.  A line comment is removed but its newline is kept.
    """
    out = []
    i = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            i += 2
            while i < n and src[i] != "\n":
                i += 1
            # leave the newline in place for the next loop iteration
        elif c == "/" and i + 1 < n and src[i + 1] == "*":
            i += 2
            while i + 1 < n and not (src[i] == "*" and src[i + 1] == "/"):
                i += 1
            if i + 1 < n:
                i += 2          # consume the closing */
            else:
                i = n           # unterminated comment: swallow the rest
            out.append(" ")
        else:
            out.append(c)
            i += 1
    return "".join(out)


def shorten_float(text):
    """Shorten a float literal without changing its value.

    1.0->1.  0.5->.5  10.00->10.  1.0e3->1.e3 .  The exponent and any suffix
    are kept verbatim.  Redundant zeros are stripped from the integer and
    fraction parts, but the mantissa is never left empty.
    """
    suffix = ""
    m = re.search(r"(lf|LF|f|F)$", text)
    if m:
        suffix = m.group(1)
        text = text[: m.start()]

    exp = ""
    m = re.search(r"([eE][+-]?\d+)$", text)
    if m:
        exp = m.group(1)
        mant = text[: m.start()]
    else:
        mant = text

    if "." in mant:
        ipart, fpart = mant.split(".", 1)
        ipart = ipart.lstrip("0")   # redundant leading zeros
        fpart = fpart.rstrip("0")   # redundant trailing zeros
        if ipart == "" and fpart == "":
            new_mant = "0."         # value is zero
        elif fpart == "":
            new_mant = ipart + "."
        elif ipart == "":
            new_mant = "." + fpart
        else:
            new_mant = ipart + "." + fpart
    else:
        # Dot-less exponent form (e.g. "1e3"): leave the mantissa alone.
        new_mant = mant
    return new_mant + exp + suffix


def tokenize(src):
    """Lex GLSL source into a list of Token.

    Steps: normalize newlines -> splice continuations -> strip comments ->
    split into lines.  Each preprocessor line becomes a single normalized T_PP
    token.  Every other line goes through the code tokenizer.  Float literals
    are shortened at this stage.
    """
    src = strip_comments(splice_lines(normalize_newlines(src)))
    tokens = []
    for line in src.split("\n"):
        stripped = line.strip()
        if stripped.startswith("#"):
            tokens.append(Token(T_PP, re.sub(r"\s+", " ", stripped)))
            continue
        for m in _CODE_TOKEN_RE.finditer(line):
            kind = m.lastgroup
            text = m.group()
            if kind == "ws":
                continue
            if kind == "float":
                tokens.append(Token(T_FLOAT, shorten_float(text)))
            elif kind == "int":
                tokens.append(Token(T_INT, text))
            elif kind == "id":
                tokens.append(Token(T_ID, text))
            else:  # op or stray char
                tokens.append(Token(T_OP, text))
    return tokens


def _code_token_texts(s):
    """Return the list of token texts the code lexer produces for *s*."""
    texts = []
    for m in _CODE_TOKEN_RE.finditer(s):
        if m.lastgroup == "ws":
            continue
        texts.append(m.group())
    return texts


def needs_space(a, b):
    """Is a space required between the already-emitted texts a and b?

    No space is needed as long as a+b lexes back to [a, b].  There are
    two extra guards.  The code lexer knows nothing about comments, and '/'
    followed by '/' or '*' would start one.  And the float rule doesn't let a
    trailing '.' touch a leading one.
    """
    if not a or not b:
        return False
    if a.endswith("/") and (b.startswith("/") or b.startswith("*")):
        return True
    if a.endswith(".") and b.startswith("."):
        return True
    return _code_token_texts(a + b) != [a, b]


def emit(tokens):
    """Render a token list to minified text.

    Code tokens are packed onto a single logical line with minimal spacing.
    Each preprocessor directive gets its own line and is never merged with
    code.  Output ends with a single trailing newline (empty input -> "").
    """
    lines = []
    cur = []
    prev = None  # previous emitted code-token text (None at line start)

    def flush():
        if cur:
            lines.append("".join(cur))
            cur.clear()

    for tok in tokens:
        if tok.type == T_PP:
            flush()
            prev = None
            lines.append(tok.text)
        else:
            if prev is not None and needs_space(prev, tok.text):
                cur.append(" ")
            cur.append(tok.text)
            prev = tok.text
    flush()

    if not lines:
        return ""
    return "\n".join(lines) + "\n"


# the --rename pass: one consistent renaming over a set of shaders

# Reserved for future use, or meaningful to the preprocessor / linker. Never
# renamed, never handed out as a new name.
GLSL_RESERVED = {
    "common", "partition", "active", "asm", "class", "union", "enum",
    "typedef", "template", "this", "resource", "goto", "inline", "noinline",
    "public", "static", "extern", "external", "interface", "long", "short",
    "half", "fixed", "unsigned", "superp", "input", "output", "hvec2", "hvec3",
    "hvec4", "fvec2", "fvec3", "fvec4", "sampler3DRect", "filter", "sizeof",
    "cast", "namespace", "using", "main", "defined",
}
_NEVER = GLSL_KEYWORDS | GLSL_TYPES | GLSL_BUILTIN_FUNCS | GLSL_BUILTIN_VARS | GLSL_RESERVED

# Qualifiers that may sit between a statement start and the declared type.
_QUALIFIERS = {
    "const", "uniform", "in", "out", "inout", "attribute", "varying", "buffer",
    "shared", "centroid", "flat", "smooth", "noperspective", "invariant",
    "precise", "highp", "mediump", "lowp", "patch", "sample", "coherent",
    "volatile", "restrict", "readonly", "writeonly",
}
_SWIZZLE_RE = re.compile(r"(?:[xyzw]{1,4}|[rgba]{1,4}|[stpq]{1,4})\Z")
_PP_DIRECTIVE_RE = re.compile(r"#\s*([A-Za-z_]*)")
# Directives whose identifiers take part in renaming. Anything else (#version,
# #extension, #pragma, #line, #error) is left verbatim and its words kept.
_PP_RENAMED = {"define", "undef", "if", "ifdef", "ifndef", "elif"}
# Name characters in the order they are handed out, most frequent name first.
# The swizzle letters come first, so the most used names repeat contexts that
# every `.x` and `.xy` already writes. That packed 33 bytes smaller than a..z,
# A..Z. Each kind of name takes its first letter from the end of _NAME_FIRST,
# and the second character runs through _NAME_REST.
_NAME_FIRST = "xyzw" + string.ascii_lowercase[:22] + "XYZW" + string.ascii_uppercase[:22]
_NAME_REST = _NAME_FIRST + string.digits + "_"


class RenameError(ValueError):
    """The renamer met source it cannot analyse. Minify without --rename."""


def _skip_balanced(tokens, i, end):
    """Given tokens[i] is one of ( [ {, return the index just past its match."""
    depth = 0
    while i < end:
        t = tokens[i]
        if t.type == T_OP:
            if t.text in "([{":
                depth += 1
            elif t.text in ")]}":
                depth -= 1
                if depth == 0:
                    return i + 1
        i += 1
    raise RenameError("unbalanced brackets")


def _parse_declarator_list(tokens, j, end):
    """Parse `name [array][= init] (, name ...)* ;` starting at the first name.

    Returns (set_of_declared_names, index_just_past_the_terminating_;).
    Balanced () [] {} inside initializers are skipped so their contents are not
    mistaken for further declarators.
    """
    names = set()
    i = j
    expect_name = True
    while i < end:
        t = tokens[i]
        txt = t.text
        if expect_name:
            if t.type == T_ID:
                names.add(txt)
                expect_name = False
                i += 1
            else:
                break  # malformed; stop collecting
        else:
            if t.type == T_OP and txt == ";":
                i += 1
                break
            if t.type == T_OP and txt == ",":
                expect_name = True
                i += 1
            elif t.type == T_OP and txt in "([{":
                i = _skip_balanced(tokens, i, end)
            elif t.type == T_OP and txt in ")]}":
                break  # e.g. a for-init closed by ')'
            else:
                i += 1
    return names, i


def _never(name):
    return name in _NEVER or name.startswith("gl_") or "__" in name


def _stage(name):
    """'vert' / 'frag' from a file name, None when the stage is unknown."""
    return {".vert": "vert", ".frag": "frag"}.get(os.path.splitext(name)[1])


def _pp_split(text):
    """(directive word, [(start, end, kind, text)] for the tokens after it)."""
    m = _PP_DIRECTIVE_RE.match(text)
    rest = text[m.end():]
    toks = [(m.end() + t.start(), m.end() + t.end(), t.lastgroup, t.group())
            for t in _CODE_TOKEN_RE.finditer(rest) if t.lastgroup != "ws"]
    return m.group(1), toks


def _match_back(code, k):
    """Given code[k] is ')', return the index of its '(', or -1 if unbalanced."""
    depth = 0
    while k >= 0:
        if code[k].type == T_OP and code[k].text in ")]}":
            depth += 1
        elif code[k].type == T_OP and code[k].text in "([{":
            depth -= 1
            if depth == 0:
                return k
        k -= 1
    return -1


def _analyze(tokens, stage):
    """Collect what one shader declares, uses and must keep, for plan_renames().

    Returns a dict:
      code      the shader's code tokens (no preprocessor lines). Every index
                below refers to this list.
      scope     names declared at file scope: functions, globals, varyings,
                struct types, block instances, macros and macro parameters
      fields    struct fields and interface-block members
      funcs     [(start, end, function name, {local names})]. A function's span
                runs from its parameter list's '(' to its body's '}'. The set
                holds its parameters and the declarations in its body.
      keep      names that must stay verbatim: uniforms, block names, vertex
                inputs and fragment outputs without layout(location), and
                words on directives we don't rewrite
      pp        identifiers on preprocessor lines
      dotted    identifiers met right after '.' (swizzles, members)
    """
    code = [t for t in tokens if t.type != T_PP]
    n = len(code)
    scope, fields, keep, pp, dotted = set(), set(), set(), set(), set()

    def txt(i):
        return code[i].text if 0 <= i < n else None

    def is_id(i):
        return 0 <= i < n and code[i].type == T_ID

    def skip(i):
        """Index past the balanced group opened at code[i]."""
        return _skip_balanced(code, i, n)

    depth, d = [], 0
    for t in code:
        if t.type == T_OP and t.text == "}":
            d -= 1
        depth.append(d)
        if t.type == T_OP and t.text == "{":
            d += 1
        if d < 0:
            raise RenameError("unbalanced braces")
    if d:
        raise RenameError("unbalanced braces")

    types = set(GLSL_TYPES)
    for i in range(n):
        if txt(i) == "struct":
            if depth[i] or not is_id(i + 1):
                raise RenameError("only named file-scope structs are supported")
            types.add(txt(i + 1))
            scope.add(txt(i + 1))
        elif is_id(i) and txt(i - 1) == ".":
            dotted.add(txt(i))

    # Function bodies: a file-scope '{' right after the ')' closing NAME(...).
    funcs = []
    for i in range(n):
        if txt(i) == "{" and depth[i] == 0 and txt(i - 1) == ")":
            o = _match_back(code, i - 1)
            if o < 1 or not is_id(o - 1):
                raise RenameError("unrecognised function header")
            funcs.append((o, skip(i) - 1, txt(o - 1), set()))

    def owner(i):
        for f in funcs:
            if f[0] <= i <= f[1]:
                return f[3]
        return scope

    def qualifiers(k):
        """Walk back over qualifiers and layout(...) from code[k]. Return
        (index of the token before them, qualifier words, whether a location
        or binding is given). Nothing outside needs the name of something
        that has either."""
        quals, located = set(), False
        while k >= 0:
            if is_id(k) and txt(k) in _QUALIFIERS:
                quals.add(txt(k))
                k -= 1
            elif txt(k) == ")":
                o = _match_back(code, k)
                if o < 1 or txt(o - 1) != "layout":
                    break
                located |= any(code[x].text in ("location", "binding") for x in range(o, k))
                k = o - 2
            else:
                break
        return k, quals, located

    def statement_start(k):
        return k < 0 or txt(k) in (";", "{", "}") or (txt(k) == "(" and txt(k - 1) == "for")

    # Struct and interface-block bodies: their declarations are members.
    in_body = [False] * n
    for i in range(n):
        if txt(i) != "{" or depth[i] != 0 or not is_id(i - 1):
            continue
        k, quals, _ = qualifiers(i - 2)
        if txt(i - 2) == "struct":
            pass
        elif quals & {"uniform", "in", "out", "buffer"} and statement_start(k):
            keep.add(txt(i - 1))                      # linked by name
        else:
            raise RenameError("unrecognised block before '{'")
        close = skip(i) - 1
        for x in range(i + 1, close):
            in_body[x] = True
        x = i + 1
        while x < close:
            if txt(x) in types:
                y = x + 1
                if txt(y) == "[":
                    y = skip(y)
                if not is_id(y):
                    raise RenameError("unsupported member declaration")
                names, x = _parse_declarator_list(code, y, close)
                fields.update(names)
            else:
                x += 1
        if is_id(close + 1):                          # instance name(s)
            names, _ = _parse_declarator_list(code, close + 1, n)
            scope.update(names)

    # Everything else: TYPE [array] NAME.
    for i in range(n):
        if in_body[i] or not is_id(i) or txt(i) not in types or txt(i - 1) == ".":
            continue
        j = i + 1
        if txt(j) == "[":
            j = skip(j)
        if not is_id(j) or txt(j) in _NEVER:
            continue
        k, quals, located = qualifiers(i - 1)
        stmt = statement_start(k)
        if stmt and txt(j + 1) != "(":
            names, _ = _parse_declarator_list(code, j, n)
        else:
            names = {txt(j)}
        if stmt and depth[i] == 0 and (
                quals & {"buffer", "attribute"}
                or ("uniform" in quals and not located)
                or ("in" in quals and stage != "frag" and not located)
                or ("out" in quals and stage != "vert" and not located)):
            keep.update(names)
        owner(i).update(names)

    # Preprocessor lines.
    for t in tokens:
        if t.type != T_PP:
            continue
        word, toks = _pp_split(t.text)
        ids = [tk[3] for tk in toks if tk[2] == "id"]
        pp.update(ids)
        if word not in _PP_RENAMED or any(tk[3] == "#" for tk in toks):
            keep.update(ids)                          # also anything that uses # or ##
            continue
        if word == "define" and toks and toks[0][2] == "id":
            scope.add(toks[0][3])
            if len(toks) > 1 and toks[1][3] == "(" and toks[1][0] == toks[0][1]:
                for _, _, kind, text in toks[2:]:      # function-like: its parameters
                    if text == ")":
                        break
                    if kind == "id":
                        scope.add(text)
    keep |= {nm for nm in fields if _SWIZZLE_RE.match(nm)}
    return {"code": code, "scope": scope, "fields": fields, "funcs": funcs,
            "keep": keep, "pp": pp, "dotted": dotted}


def _family(first, avoid):
    """Yield the names that start with *first*: two characters long (in
    _NAME_REST's order), then three, and so on. None of them is in *avoid*
    or reserved."""
    for length in itertools.count(1):
        for rest in itertools.product(_NAME_REST, repeat=length):
            name = first + "".join(rest)
            if name not in avoid and not _never(name):
                yield name


def plan_renames(shaders, protect=()):
    """Plan one consistent renaming for a whole set of shaders.

    *shaders* maps a file name (its extension gives the stage) to its token
    list. There are two kinds of names:

      file scope  Functions, globals, varyings, macros, struct types and
                  members. One mapping covers every shader, so the same name
                  gets the same short name everywhere. Varyings still match
                  across stages, and code repeated between files stays
                  repeated for the compressor.
      local       A function's parameters and locals, if the shader doesn't
                  also use the name at file scope, on a directive or as a
                  member. The k-th local to be used in each function becomes
                  slot k, and each slot has one short name. So identical
                  functions in different files still come out identical.

    File-scope names are ranked by use count, most used first, and take the
    names of their kind's family in that order: functions get Ux, Uy, ...,
    members Tx, Ty, ..., and the rest Sx, Sy, .... Slot k takes the k-th name
    of the locals' family, Vx, Vy, .... A file-scope name is renamed only if
    no shader uses it without declaring it, as happens with built-ins this
    tool doesn't know and macros supplied from outside. Nothing may mark it as
    external either: uniforms without a location, block names, inputs and
    outputs without a location, and --protect all keep their names.

    New names avoid every name that stays verbatim anywhere and every reserved
    word, so nothing can be captured. A swizzle such as '.z' lives in another
    namespace, so a variable may become 'z'. But a renamed member never takes
    a name that appears after a '.'.

    Returns {file: (mapping, members, [(start, end, {local: new})])} for
    apply_renames(), plus the file-scope mapping.
    """
    protect = set(protect)
    facts = {name: _analyze(toks, _stage(name)) for name, toks in shaders.items()}
    keep = set(protect)
    dotted, members = set(), set()
    for f in facts.values():
        keep |= f["keep"]
        dotted |= f["dotted"]
        members |= f["fields"]

    # Work out who owns each identifier occurrence: a function's locals or
    # file scope.
    gcount, nslots = {}, 0            # file-scope use counts; most locals in one function
    sites = {}                        # file -> [(start, end, [locals by rank], decl)]
    for name in sorted(facts):
        f = facts[name]
        code, shared = f["code"], f["scope"] | f["fields"] | f["pp"] | protect
        spans = []
        everything_local = set()
        for start, end, _, decl in f["funcs"]:
            local = {nm for nm in decl if nm not in shared and not _never(nm)}
            everything_local |= decl
            first = {}
            for i in range(start, end + 1):
                t = code[i]
                if t.type == T_ID and t.text in local and code[i - 1].text != ".":
                    first.setdefault(t.text, i)
            ranked = sorted(local, key=lambda nm: (first.get(nm, len(code)), nm))   # by first use
            nslots = max(nslots, len(ranked))
            spans.append((start, end, ranked, decl))
        sites[name] = spans
        known = f["scope"] | f["fields"]
        fi = 0
        for i, t in enumerate(code):
            if t.type != T_ID or _never(t.text):
                continue
            while fi < len(spans) and spans[fi][1] < i:
                fi += 1
            inside = fi < len(spans) and spans[fi][0] <= i
            if i and code[i - 1].text == ".":
                if t.text not in f["fields"] and not _SWIZZLE_RE.match(t.text):
                    keep.add(t.text)                  # unknown member: leave it alone
                if t.text in members:
                    gcount[t.text] = gcount.get(t.text, 0) + 1
                continue
            if inside and t.text in spans[fi][2]:
                continue                              # a local, named by its slot
            if t.text not in known and not (inside and t.text in spans[fi][3]):
                keep.add(t.text)                      # used, never declared here
            gcount[t.text] = gcount.get(t.text, 0) + 1
        for nm in f["pp"]:
            if nm not in known and nm not in everything_local and not _never(nm):
                keep.add(nm)
        for t in shaders[name]:
            if t.type == T_PP:
                for _, _, kind, text in _pp_split(t.text)[1]:
                    if kind == "id":
                        gcount[text] = gcount.get(text, 0) + 1

    renamed = {nm for nm in gcount if nm not in keep and not _never(nm)}
    swizzly = {nm for nm in members if _SWIZZLE_RE.match(nm)}   # members spelled like swizzles
    members &= renamed
    verbatim = (set(gcount) - renamed) | keep
    # Functions whose argument counts never meet in one shader share a name
    # (GLSL overloading). A group is ranked as one name with all its uses.
    groups = _overloads(facts, shaders, renamed, gcount)
    lead = {nm: g[0] for g in groups for nm in g}
    gsum = {}
    for nm in renamed:
        gsum[lead.get(nm, nm)] = gsum.get(lead.get(nm, nm), 0) + gcount[nm]
    ranking = [(-c, nm) for nm, c in gsum.items()]

    avoid = verbatim | _NEVER
    # Every new name belongs to a family. The first letter gives its kind and
    # comes from the end of _NAME_FIRST, and the rest runs through _NAME_REST
    # in order, two characters and then three. Slots take V in slot order
    # (Vx, Vy, Vz, Vw, Va, ...) and functions take U. Members take T (never a
    # name seen after a '.'), and the other file-scope names (globals,
    # varyings, uniforms, struct types, macros) take S. The most used name in
    # each family comes first. A name then says what it is (a local, a call, a
    # member, a global) before it's finished, and the k-th local of every
    # function has the same name. The coder's word and statement contexts
    # learn both. The minified text comes out 16 KB longer than with one
    # shared pool of a..Z, aa.. by use count, but it packs about 1.3 KB
    # smaller. Numbering locals by first use, putting the slots after every
    # file-scope name, and giving functions, members and the rest their own
    # families each helped. The family letters are ones the text rarely uses.
    # Common letters like x, w, k or q packed worse.
    funcs = {nm for f in facts.values() for _, _, nm, _ in f["funcs"]} - members
    family = _family(_NAME_FIRST[-1], avoid)
    slot_names = {k: next(family) for k in range(nslots)}
    avoid |= set(slot_names.values())
    mapping = {}
    for first, kind, skip in ((_NAME_FIRST[-2], funcs, set()), (_NAME_FIRST[-3], members, dotted),
                              (_NAME_FIRST[-4], None, set())):
        family = _family(first, avoid | skip)
        for _, key in sorted(ranking):
            if key not in mapping and (kind is None or key in kind):
                mapping[key] = next(family)
        avoid |= set(mapping.values())
    for nm, first in lead.items():
        mapping[nm] = mapping[first]

    plan = {}
    for name, spans in sites.items():
        plan[name] = (mapping, members,
                      [(s, e, {nm: slot_names[k] for k, nm in enumerate(ranked)})
                       for s, e, ranked, _ in spans], swizzly)
    return plan, mapping


def _arity(code, o):
    """Count the arguments (or parameters) in the bracket opened at code[o]."""
    if code[o + 1].text == ")" or code[o + 1].text == "void" and code[o + 2].text == ")":
        return 0
    d, n = 0, 1
    for t in code[o + 1:]:
        if t.type == T_OP and t.text in "([{":
            d += 1
        elif t.type == T_OP and t.text in ")]}":
            if d == 0:
                return n
            d -= 1
        elif t.type == T_OP and t.text == "," and d == 0:
            n += 1
    raise RenameError("unbalanced brackets")


def _overloads(facts, shaders, renamed, gcount):
    """Find groups of functions that can share one name. In every shader,
    each argument count belongs to at most one function of the group, so a
    call can only resolve to the function it called before. Only functions
    whose name isn't used for anything else take part: no variable, member,
    macro or prototype of that name, never on a directive, and not main. The
    most used go first, and each joins the first group it fits. (Sharing
    between equal counts with different argument types was no smaller, and
    it keeps dead helpers alive in genshaders.)"""
    arity, other = {}, set()
    for name, f in facts.items():
        code = f["code"]
        fn = set()
        for start, _, nm, _ in f["funcs"]:
            arity.setdefault(nm, {}).setdefault(name, set()).add(_arity(code, start))
            fn.add(nm)
        other |= (f["scope"] - fn) | f["fields"] | f["pp"]
        for i, t in enumerate(code):                  # prototypes; uses that aren't calls
            if t.type == T_ID and t.text in fn and i + 1 < len(code) and code[i + 1].text != "(":
                other.add(t.text)
        depth = 0
        for i, t in enumerate(code):
            if t.type == T_OP and t.text in "{}":
                depth += 1 if t.text == "{" else -1
            elif depth == 0 and t.text == ")" and i + 1 < len(code) and code[i + 1].text == ";":
                o = _match_back(code, i)
                if o > 0 and code[o - 1].type == T_ID:
                    other.add(code[o - 1].text)
    cands = sorted((nm for nm in arity if nm in renamed and nm not in other and nm != "main"),
                   key=lambda nm: (-gcount[nm], nm))
    groups, used = [], []
    for nm in cands:
        for g, u in zip(groups, used):
            if all(not (a & u.get(sh, set())) for sh, a in arity[nm].items()):
                g.append(nm)
                for sh, a in arity[nm].items():
                    u.setdefault(sh, set()).update(a)
                break
        else:
            groups.append([nm])
            used.append({sh: set(a) for sh, a in arity[nm].items()})
    return [g for g in groups if len(g) > 1]


def _rename_pp(text, mapping, fields):
    word, toks = _pp_split(text)
    if word not in _PP_RENAMED or any(tk[3] == "#" for tk in toks):
        return text
    out, last, prev = [], 0, None
    for start, end, kind, tok in toks:
        if kind == "id" and tok in mapping and (prev != "." or tok in fields):
            out.append(text[last:start])
            out.append(mapping[tok])
            last = end
        prev = tok
    out.append(text[last:])
    return "".join(out)


# A swizzle's colour (rgba) and texture (stpq) letters, as position letters.
_XYZW = str.maketrans("rgbastpq", "xyzwxyzw")


def apply_renames(tokens, mapping, fields=(), locals_=(), swizzly=None):
    """Return *tokens* renamed. File-scope names go through *mapping*, and
    members (*fields*) are renamed after '.' too. *locals_* holds
    (start, end, {name: new}) spans over the code tokens, as plan_renames()
    gives them. Inside each span, that function's locals are looked up first.

    *swizzly* is the set of declared members spelled like swizzles, which
    stay verbatim. When it's given, every other swizzle is written with xyzw:
    `.rgb` becomes `.xyz` and `.a` becomes `.w`. The language defines them as
    the same components, and one alphabet repeats better for the compressor
    than three. A name after '.' that some struct or block declares as a
    member is left alone."""
    out, prev, ci, fi = [], None, -1, 0
    for t in tokens:
        if t.type == T_PP:
            out.append(Token(T_PP, _rename_pp(t.text, mapping, fields)))
            continue
        ci += 1
        while fi < len(locals_) and locals_[fi][1] < ci:
            fi += 1
        if t.type == T_ID and prev != ".":
            local = locals_[fi][2] if fi < len(locals_) and locals_[fi][0] <= ci else {}
            new = local.get(t.text) or mapping.get(t.text)
        elif t.type == T_ID and t.text in fields:
            new = mapping[t.text]
        elif (t.type == T_ID and swizzly is not None and t.text not in swizzly
              and _SWIZZLE_RE.match(t.text) and t.text.translate(_XYZW) != t.text):
            new = t.text.translate(_XYZW)
        else:
            new = None
        out.append(Token(T_ID, new) if new else t)
        prev = t.text
    return out


def merge_declarations(tokens):
    """Fold consecutive declarations with the same qualifiers and type into
    one declarator list: `uniform float a;uniform float b;` becomes
    `uniform float a,b;`, `vec3 p=x;vec3 q=p;` becomes `vec3 p=x,q=p;`.

    GLSL processes a declarator list left to right, the same as separate
    statements (a name is in scope from its own initializer on), so this
    doesn't change behaviour. It only applies to plain statements at file
    scope or in a function body block. It never applies inside a struct or
    block body, across a directive, to a layout() or array-typed prefix, or
    to a prototype. Both statements must also start at a statement boundary.
    """
    toks = list(tokens)
    out = []
    i = 0
    n = len(toks)
    kinds = []          # per open '{': True when statements inside may merge

    def prefix_at(j):
        """Return the length of `qualifiers TYPE` at toks[j] if a NAME (not
        '(') follows, or 0."""
        k = j
        while k < n and toks[k].type == T_ID and toks[k].text in _QUALIFIERS:
            k += 1
        if k >= n or toks[k].type != T_ID or toks[k].text not in GLSL_TYPES:
            return 0
        if k + 2 >= n or toks[k + 1].type != T_ID or toks[k + 1].text in _NEVER:
            return 0
        if toks[k + 2].type != T_OP or toks[k + 2].text not in (";", ",", "="):
            return 0
        return k + 1 - j

    def statement_end(j):
        """Index of the ';' ending the declaration starting at toks[j]."""
        depth = 0
        while j < n:
            t = toks[j]
            if t.type == T_PP:
                return -1
            if t.type == T_OP:
                if t.text in "([{":
                    depth += 1
                elif t.text in ")]}":
                    depth -= 1
                    if depth < 0:
                        return -1
                elif t.text == ";" and depth == 0:
                    return j
            j += 1
        return -1

    prev = None
    while i < n:
        t = toks[i]
        at_start = (t.type != T_PP and (prev is None or prev in (";", "{", "}"))
                    and all(kinds))
        plen = prefix_at(i) if at_start else 0
        if plen:
            end = statement_end(i)
            if end > 0:
                stmt = toks[i:end]
                pre = [x.text for x in toks[i:i + plen]]
                while True:
                    j = end + 1
                    if (j + plen <= n and [x.text for x in toks[j:j + plen]] == pre
                            and all(x.type == T_ID for x in toks[j:j + plen])
                            and prefix_at(j) == plen):
                        e2 = statement_end(j)
                        if e2 > 0:
                            stmt = stmt + [Token(T_OP, ",")] + toks[j + plen:e2]
                            end = e2
                            continue
                    break
                out.extend(stmt)
                out.append(toks[end])
                prev = ";"
                i = end + 1
                continue
        if t.type == T_OP and t.text == "{":
            kinds.append(prev not in (None, "=") and not (out and out[-1].type == T_ID
                                                          and out[-1].text not in ("else", "do")))
        elif t.type == T_OP and t.text == "}" and kinds:
            kinds.pop()
        out.append(t)
        if t.type != T_PP:
            prev = t.text
        i += 1
    return out


# integral float literals: 2. -> 2 where GLSL turns the int back into the float

_FLOAT_TYPES = {"float", "vec2", "vec3", "vec4", "mat2", "mat3", "mat4", "mat2x2", "mat2x3",
                "mat2x4", "mat3x2", "mat3x3", "mat3x4", "mat4x2", "mat4x3", "mat4x4"}
_BUILTIN_VAR_TYPES = {"gl_FragCoord": "vec4", "gl_Position": "vec4", "gl_PointCoord": "vec2",
                      "gl_FragDepth": "float", "gl_PointSize": "float", "gl_VertexID": "int",
                      "gl_InstanceID": "int", "gl_FrontFacing": "bool"}
# Built-ins whose result is float-family whatever float-family arguments they get.
_FLOAT_RESULT = {"length", "distance", "dot", "determinant", "sin", "cos", "tan", "asin", "acos",
                 "atan", "pow", "exp", "log", "exp2", "log2", "sqrt", "inversesqrt", "fract",
                 "floor", "ceil", "round", "trunc", "mod", "mix", "step", "smoothstep",
                 "normalize", "reflect", "refract", "faceforward", "cross", "dFdx", "dFdy",
                 "fwidth", "radians", "degrees", "inverse", "transpose", "outerProduct",
                 "matrixCompMult"}
_SCALAR_RESULT = {"length", "distance", "dot", "determinant"}
# Argument positions with a plain `float` overload, so an int literal there can
# only resolve to that signature: smoothstep(float,float,genType),
# step(float,genType), mix(genType,genType,float), mod(genType,float),
# textureLod(sampler, P, float lod).
_FLOAT_ARGS = {"smoothstep": (0, 1), "step": (0,), "mix": (2,), "mod": (1,), "textureLod": (2,)}
# Positions that are safe once the first argument is known to be float-family
# (rules out the int overloads): clamp(genType,float,float), min/max(genType,float).
_FLOAT_ARGS_AFTER_FLOAT = {"clamp": (1, 2), "min": (1,), "max": (1,)}
_PREC = {"*": 3, "/": 3, "+": 2, "-": 2, "<": 1, ">": 1, "<=": 1, ">=": 1, "==": 0, "!=": 0}
_ASSIGN = {"=", "+=", "-=", "*=", "/="}
_INT_OPS = {"(", ")", "+", "-", "*", "/", "%", "&", "|", "^", "~", "<<", ">>"}
_INTEGRAL_FLOAT_RE = re.compile(r"\d+\.\Z")


def _vec_size(t):
    return int(t[-1]) if t and t[-1].isdigit() and "vec" in t else 1


def int_literals(tokens, arith=False, casts=True):
    """Write integral float literals as ints (`2.` -> `2`) wherever GLSL's
    implicit int->float conversion gives back exactly the same float:
      - a whole argument of a float/vec/mat constructor
      - the whole right-hand side of an assignment or initializer of a
        float-family variable
      - an operand of < > <= >= == != whose partner operand (the whole
        sub-expression, by precedence) is known to be float-family. With
        *arith*, the same goes for * / + -
      - one branch of a ?: whose other branch is float-family
      - a float-only argument slot of a built-in, or a float parameter of a
        user function that is defined only once.
    Types come from the declarations, per function scope. Integral values below
    2^24 convert exactly. It never converts both operands of one operator.
    Anything it can't type is skipped: return values (Mesa doesn't convert
    them), overloaded calls, built-ins with int overloads and no known float
    argument, and names declared with two types in one scope. Int arithmetic,
    array sizes, loop counters and preprocessor lines are never touched.

    Every conversion keeps the value, but with *arith* the driver may not
    keep the arithmetic. Mesa fuses a*2.+b and a*2+b into different
    multiply-adds, so a few pixels change by a rounding step. The minifier
    promises identical pictures, so *arith* is off by default.

    With *casts*, an explicit float(X) of an int expression X is handled the
    same way (`a*float(i)` -> `a*i`, `vec2(float(i+1),0)` -> `vec2(i+1,0)`).
    This also applies in arithmetic, because there the compiler converts X at
    the same point either way. (What changes the fusing is that a literal
    gets folded first.) X only counts as int if it's built from int literals,
    int names and ivec components, int(...) and the integer operators. Its
    brackets are dropped when it's a single operand or a whole argument or
    initializer."""
    toks = list(tokens)
    code = [k for k, t in enumerate(toks) if t.type != T_PP]
    c = [toks[k] for k in code]
    n = len(c)

    def txt(i):
        return c[i].text if 0 <= i < n else None

    def is_id(i):
        return 0 <= i < n and c[i].type == T_ID

    def match(i, step):
        """Index of the bracket matching c[i] (step +1 forward, -1 back)."""
        opens, closes = ("([", ")]") if step > 0 else (")]", "([")
        d = 0
        while 0 <= i < n:
            if c[i].type == T_OP and c[i].text in opens:
                d += 1
            elif c[i].type == T_OP and c[i].text in closes:
                d -= 1
                if d == 0:
                    return i
            i += step
        return -1

    # Record every declaration's type by scope and name. A scope is either a
    # function, from its parameter list to its closing brace, or the file. A
    # name declared with two types in one scope counts as unknown.
    structs = {txt(i + 1) for i in range(n) if txt(i) == "struct" and is_id(i + 1)}
    types = GLSL_TYPES | structs
    fn_of, depth, params = [-1] * n, 0, {}
    for i in range(n):
        if txt(i) == "{" and depth == 0 and txt(i - 1) == ")":
            o = match(i - 1, -1)
            d, e = 0, i
            while e < n:
                d += {"{": 1, "}": -1}.get(txt(e), 0) if c[e].type == T_OP else 0
                if d == 0:
                    break
                e += 1
            if o < 1 or e >= n:
                return tokens
            for x in range(o, e + 1):
                fn_of[x] = o
            ps, k = [], o + 1                           # its parameter types
            while k < i - 1:
                while is_id(k) and txt(k) in _QUALIFIERS:
                    k += 1
                ps.append(txt(k) if txt(k) in types else None)
                while k < i - 1 and txt(k) != ",":
                    k += 1
                k += 1
            params.setdefault(txt(o - 1), []).append(ps if txt(o + 1) != ")" else [])
        if c[i].type == T_OP and txt(i) in ("{", "}"):
            depth += 1 if txt(i) == "{" else -1
    decl, arrays, funcs = {}, set(), {}
    for i in range(n):
        if not is_id(i) or txt(i) not in types or txt(i - 1) in (".", "struct"):
            continue
        j = i + 1
        if txt(j) == "[":
            j = match(j, 1) + 1
            if j <= 0:
                return tokens
        while is_id(j) and txt(j) not in _NEVER:
            if txt(j + 1) == "(":
                funcs.setdefault(txt(j), set()).add(txt(i))
                break
            decl.setdefault((fn_of[j], txt(j)), set()).add(txt(i))
            k = j + 1
            if txt(k) == "[":
                arrays.add(txt(j))
                k = match(k, 1) + 1
                if k <= 0:
                    return tokens
            d = 0
            while k < n and not (d == 0 and txt(k) in (",", ";", ")")):
                d += {"(": 1, "[": 1, "{": 1, ")": -1, "]": -1, "}": -1}.get(txt(k), 0) if c[k].type == T_OP else 0
                k += 1
            if txt(k) != "," or not is_id(k + 1) or txt(k + 1) in types:
                break
            j = k + 1
    samplers = [t for v in decl.values() for t in v if t.startswith(("isampler", "usampler"))]
    members = {}
    for (f, nm), t in decl.items():
        members.setdefault(nm, set()).update(t)

    def name_type(nm, at=None):
        """Type of nm as seen at c[at] (at=None: a struct member, any scope)."""
        if nm in _BUILTIN_VAR_TYPES:
            return _BUILTIN_VAR_TYPES[nm]
        if nm in funcs:
            return None
        t = members.get(nm) if at is None else decl.get((fn_of[at], nm)) or decl.get((-1, nm))
        return next(iter(t)) if t and len(t) == 1 else None

    def postfix(t, i):
        """Apply .member / .swizzle / [index] from c[i] and return (type, end)."""
        while t:
            if txt(i) == "." and is_id(i + 1):
                sw = txt(i + 1)
                if t in _FLOAT_TYPES and t.startswith("vec") and _SWIZZLE_RE.match(sw):
                    t = "float" if len(sw) == 1 else "vec%d" % len(sw)
                elif t in structs:
                    t = name_type(sw)
                elif t == "float" and _SWIZZLE_RE.match(sw):
                    t = "float" if len(sw) == 1 else "vec%d" % len(sw)
                else:
                    return None, i
                i += 2
            elif txt(i) == "[":
                e = match(i, 1)
                if e < 0:
                    return None, i
                t = ("float" if t.startswith("vec") else "vec" + t[3] if t.startswith("mat") and len(t) == 4
                     else None) if t in _FLOAT_TYPES else None
                i = e + 1
            else:
                break
        return t, i

    def call_type(fn, o):
        """Result type of the call fn( at c[o] ('F' = some float-family type)."""
        if fn in _FLOAT_TYPES or fn in ("int", "bool", "ivec2", "ivec3", "ivec4"):
            return fn
        if fn in funcs:
            r = funcs[fn]
            return next(iter(r)) if len(r) == 1 else None
        if fn in _SCALAR_RESULT:
            return "float"
        if fn in ("min", "max", "clamp", "abs", "sign"):  # the first argument's type
            return operand(o + 1, branch_end(o + 1))
        if fn in _FLOAT_RESULT:
            return "F"
        if fn in ("texture", "textureLod", "texelFetch") and not samplers:
            return "vec4"
        return None

    def primary_right(s):
        """Type and end (exclusive) of the operand starting at c[s]."""
        while txt(s) in ("-", "+") and c[s].type == T_OP:
            s += 1
        t = c[s].type if s < n else None
        if t == T_FLOAT:                                # an undecided integral literal: unknown
            return (None if _INTEGRAL_FLOAT_RE.match(txt(s)) and s >= cur[0] else "float"), s + 1
        if t == T_INT:
            return "int", s + 1
        if t != T_ID:
            return None, s
        if txt(s + 1) == "(":
            e = match(s + 1, 1)
            if e < 0:
                return None, s
            return postfix(call_type(txt(s), s + 1), e + 1)
        base = name_type(txt(s), s)
        if txt(s) in arrays and txt(s + 1) == "[":
            e = match(s + 1, 1)
            return postfix(base, e + 1) if e > 0 else (None, s)
        return postfix(base, s + 1)

    def _chain_start(e):
        s = e
        while True:
            if txt(s) in (")", "]"):
                o = match(s, -1)
                if o < 0:
                    return -1
                s = o - 1 if txt(s) == "]" or is_id(o - 1) else o
                if txt(s) == "(":
                    return s
                continue
            if is_id(s) and txt(s - 1) == ".":
                s -= 2
                continue
            return s

    def primary_left(e):
        """Type of the operand ending at c[e] (inclusive)."""
        # walk back to the start of the postfix chain, then type it forwards
        s = e
        while True:
            if txt(s) in (")", "]"):
                o = match(s, -1)
                if o < 0:
                    return None
                s = o - 1 if txt(s) == "]" or is_id(o - 1) else o
                if txt(s) == "(":                      # a bare (...) group
                    return None
                continue
            if is_id(s) and txt(s - 1) == ".":
                s -= 2
                continue
            break
        if c[s].type == T_FLOAT:
            return "float" if s == e and (not _INTEGRAL_FLOAT_RE.match(txt(s)) or s in kept) else None
        t, end = primary_right(s)
        return t if end == e + 1 else None

    def binary(i):
        """Is the operator at c[i] binary (preceded by an operand)?"""
        p = c[i - 1] if i > 0 else None
        return p is not None and (p.type in (T_ID, T_FLOAT, T_INT) and p.text not in _NEVER - _BUILTIN_VAR_TYPES.keys()
                                  or p.text in (")", "]"))

    def decl_type(k):
        """If c[k] is a name being declared (TYPE a=.., b=..), its TYPE."""
        if not is_id(k):
            return None
        d = 0
        while k > 0:                                    # back to the statement start
            k -= 1
            if txt(k) in (")", "]"):
                d += 1
            elif txt(k) in ("(", "["):
                if d == 0:
                    break
                d -= 1
            elif d == 0 and txt(k) in (";", "{", "}"):
                break
        k += 1
        while is_id(k) and txt(k) in _QUALIFIERS:
            k += 1
        return txt(k) if is_id(k) and txt(k) in types and is_id(k + 1) else None

    floaty = lambda t: t == "F" or t in _FLOAT_TYPES

    def combine(ts):
        """Type of an arithmetic chain of operands of types ts."""
        if None in ts:
            return "F" if any(floaty(t) for t in ts if t) else None
        if all(t == "int" for t in ts):
            return "int"
        if any(t == "F" or t.startswith("mat") for t in ts):
            return "F"
        vecs = {t for t in ts if t.startswith("vec")}
        if len(vecs) > 1 or not all(floaty(t) or t == "int" for t in ts):
            return None
        return vecs.pop() if vecs else "float"

    def unary(k):
        """Is c[k] a prefix - + ! (nothing operand-like before it)?"""
        return c[k].type == T_OP and txt(k) in ("-", "+", "!") and not binary(k)

    def operand(s, e):
        """Type of the arithmetic expression c[s:e] (None: unknown or not numeric)."""
        ts, k = [], s
        while k < e:
            while k < e and unary(k):
                k += 1
            if txt(k) == "(":
                m = match(k, 1)
                if m < 0 or m >= e:
                    return None
                t, k = postfix(operand(k + 1, m), m + 1)
            else:
                t, k2 = primary_right(k)
                if k2 <= k:
                    return None
                k = k2
            ts.append(t)
            if k >= e:
                break
            if c[k].type != T_OP or txt(k) not in ("*", "/", "+", "-"):
                return None
            k += 1
        return combine(ts) if ts else None

    def right_operand(r):
        """Type of the right operand of the binary operator at c[r]."""
        prec, k = _PREC[txt(r)], r + 1
        while k < n:
            while k < n and unary(k):
                k += 1
            if txt(k) == "(":
                m = match(k, 1)
                if m < 0:
                    return None
                _, k = postfix("float", m + 1)
            else:
                _, k2 = primary_right(k)
                if k2 <= k:
                    return None
                k = k2
            if not (txt(k) in _PREC and c[k].type == T_OP and _PREC[txt(k)] > prec):
                break
            k += 1
        return operand(r + 1, k) if _PREC[txt(r)] >= 2 or prec >= 2 else operand(r + 1, k)

    def left_operand(l):
        """Type of the left operand of the binary operator at c[l]."""
        prec, j = _PREC[txt(l)], l - 1
        while True:
            ps = _chain_start(j)
            if ps < 0:
                return None
            while ps > 0 and unary(ps - 1):
                ps -= 1
            if ps > 0 and txt(ps - 1) in _PREC and binary(ps - 1) and _PREC[txt(ps - 1)] >= prec:
                j = ps - 2
                continue
            return operand(ps, l)

    def branch_end(k):
        """First depth-0 token ending a ternary branch starting at c[k]."""
        d = 0
        while k < n:
            if txt(k) in ("(", "["):
                d += 1
            elif txt(k) in (")", "]"):
                if d == 0:
                    return k
                d -= 1
            elif d == 0 and txt(k) in (",", ";", ":", "?"):
                return k
            k += 1
        return k

    def int_expr(s, e):
        """Is c[s:e] plainly an int expression (see the docstring)?"""
        k = s
        while k < e:
            t = txt(k)
            if c[k].type == T_INT or c[k].type == T_OP and t in _INT_OPS:
                k += 1
            elif t == "int" and txt(k + 1) == "(":     # int(...): an int whatever it holds
                m = match(k + 1, 1)
                if m < 0 or m >= e:
                    return False
                k = m + 1
            elif not is_id(k) or txt(k + 1) == "(" or txt(k + 1) == "[":
                return False
            elif name_type(t, k) == "int" and txt(k + 1) != ".":
                k += 1
            elif ((name_type(t, k) or "").startswith("ivec") and txt(k + 1) == "." and is_id(k + 2)
                  and len(txt(k + 2)) == 1 and _SWIZZLE_RE.match(txt(k + 2)) and txt(k + 3) not in (".", "[")):
                k += 3
            else:
                return False
        return s < e

    def primary(s, e):
        """Is c[s:e] one operand that needs no brackets (a name, a number, a call, a.b, a[i])?"""
        if not (is_id(s) or c[s].type == T_INT):
            return False
        k = s + 1
        while k < e:
            if txt(k) in ("(", "["):
                k = match(k, 1) + 1
                if k <= 0:
                    return False
            elif txt(k) == "." and is_id(k + 1):
                k += 2
            else:
                return False
        return k == e

    cands, cast_end = [], {}
    for i in range(n):
        if c[i].type == T_FLOAT and _INTEGRAL_FLOAT_RE.match(c[i].text):
            cands.append(i)
        elif casts and txt(i) == "float" and txt(i + 1) == "(" and txt(i - 1) not in (".", "struct"):
            e = match(i + 1, 1)
            if e > i + 2 and txt(e + 1) not in (".", "[") and not any(
                    txt(k) == "float" or c[k].type == T_FLOAT for k in range(i + 2, e)):
                cands.append(i)
                cast_end[i] = e
    kept, convert, cur, unwrap = set(), set(), [0], {}
    for i in cands:
        cast = i in cast_end
        hi = cast_end[i] if cast else i                 # the candidate's last token
        cur[0] = i
        if cast and not int_expr(i + 2, hi) or not cast and len(c[i].text) > 8:   # literals: well inside 2^24
            kept.add(i)
            continue
        ok = False
        lo = i - 1 if txt(i - 1) == "-" and not binary(i - 1) else i
        prev, nxt = txt(lo - 1), txt(hi + 1)
        whole = False                                   # a whole argument / initializer
        if prev in ("(", ",") and nxt in (",", ")"):
            o, d, arg = i - 1, 0, 0
            while o >= 0:                               # the enclosing '(' and our argument index
                if txt(o) in (")", "]"):
                    d += 1
                elif txt(o) in ("(", "["):
                    if d == 0:
                        break
                    d -= 1
                elif txt(o) == "," and d == 0:
                    arg += 1
                o -= 1
            fn = txt(o - 1) if o > 0 and txt(o) == "(" and is_id(o - 1) else None

            def arg_type(a):
                k, x = o + 1, 0
                while x < a:
                    k = branch_end(k) + 1
                    x += 1
                e = branch_end(k)
                return operand(k, e) if txt(e) in (",", ")") else None
            whole = lo == i
            if fn in _FLOAT_TYPES:
                ok = True
            elif lo != i:                               # -lit: constructors only
                ok = False
            elif fn in _FLOAT_ARGS:
                ok = arg in _FLOAT_ARGS[fn]
            elif fn in _FLOAT_ARGS_AFTER_FLOAT and arg in _FLOAT_ARGS_AFTER_FLOAT[fn]:
                ok = floaty(arg_type(0) or "")
            elif fn == "mix" and arg in (0, 1):
                ok = arg_type(1 - arg) == "float"
            elif fn == "pow" and arg == 1:
                ok = arg_type(0) == "float"
            elif fn in params and fn not in GLSL_BUILTIN_FUNCS:
                nargs = _arity(c, o)                    # one definition of this arity, float param
                sig = [ps for ps in params[fn] if len(ps) == nargs]
                ok = len(sig) == 1 and arg < len(sig[0]) and sig[0][arg] in _FLOAT_TYPES
        elif prev == "?" and nxt == ":":                # c?lit:b with b float-family
            ok = floaty(operand(i + 2, branch_end(i + 2)) or "")
        elif prev == ":" and nxt in (";", ")", ","):    # c?a:lit with a float-family
            q, d = lo - 2, 0
            while q >= 0 and not (d == 0 and txt(q) == "?"):
                d += {")": 1, "]": 1, "(": -1, "[": -1}.get(txt(q), 0)
                if d < 0:
                    break
                q -= 1
            ok = q >= 0 and txt(q) == "?" and floaty(operand(q + 1, lo - 1) or "")
        elif prev in _ASSIGN and nxt in (";", ","):
            t = decl_type(lo - 2) if prev == "=" else None
            ok = floaty(t if t else primary_left(lo - 2) or "")
            whole = lo == i and prev == "="
        else:
            lop = prev if prev in _PREC and binary(lo - 1) else None
            rop = nxt if nxt in _PREC and c[hi + 1].type == T_OP else None
            if lop and (not rop or _PREC[lop] >= _PREC[rop]):
                ok = (arith or cast or _PREC[lop] < 2) and floaty(left_operand(lo - 1) or "")
            elif rop:
                ok = (arith or cast or _PREC[rop] < 2) and floaty(right_operand(hi + 1) or "")
        if ok and cast:
            convert.add(i)
            unwrap[i] = whole or primary(i + 2, hi)
            c[i] = Token(T_ID, "int")                   # later candidates see int(X): an int
        elif ok:
            convert.add(i)
            c[i] = Token(T_INT, c[i].text[:-1])         # later literals see an int here
        else:
            kept.add(i)
    drop = set()
    for i in convert:
        if i in cast_end:                               # float(X) -> (X), or X
            drop.add(code[i])
            if unwrap[i]:
                drop.update((code[i + 1], code[cast_end[i]]))
        else:
            toks[code[i]] = c[i]
    return [t for k, t in enumerate(toks) if k not in drop]


# single-use locals: `float b=E;...b...` -> `...(E)...` where it's safe

def _gtype(name):
    """Describe a GLSL type name as (kind, n, m). For numbers, kind is
    f/i/u/b/d, n is the vector size (1 for a scalar) or a matrix's column
    count, and m is its row count (0 if it isn't a matrix). Samplers, structs
    and everything else give ("o", name)."""
    m = re.match(r"([dbiu]?)vec([234])\Z", name)
    if m:
        return (m.group(1) or "f", int(m.group(2)), 0)
    m = re.match(r"(d?)mat([234])(?:x([234]))?\Z", name)
    if m:
        return ("d" if m.group(1) else "f", int(m.group(2)), int(m.group(3) or m.group(2)))
    return {"float": ("f", 1, 0), "int": ("i", 1, 0), "uint": ("u", 1, 0), "bool": ("b", 1, 0),
            "double": ("d", 1, 0)}.get(name, ("o", name))


_RANK = {"i": 0, "u": 1, "f": 2, "d": 3}
_BIND = {"*": 12, "/": 12, "%": 12, "+": 11, "-": 11, "<<": 10, ">>": 10, "<": 9, ">": 9,
         "<=": 9, ">=": 9, "==": 8, "!=": 8, "&": 7, "^": 6, "|": 5, "&&": 4, "^^": 3, "||": 2}
_ASSIGN_OPS = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}
# Built-ins whose result is their first argument's type (genType / genIType).
_SAME_AS_ARG0 = {"radians", "degrees", "sin", "cos", "tan", "asin", "acos", "atan", "sinh", "cosh",
                 "tanh", "asinh", "acosh", "atanh", "pow", "exp", "log", "exp2", "log2", "sqrt",
                 "inversesqrt", "abs", "sign", "floor", "trunc", "round", "roundEven", "ceil",
                 "fract", "mod", "min", "max", "clamp", "mix", "normalize", "faceforward",
                 "reflect", "refract", "dFdx", "dFdy", "fwidth", "dFdxCoarse", "dFdxFine",
                 "dFdyCoarse", "dFdyFine", "fwidthCoarse", "fwidthFine", "fma", "inverse",
                 "matrixCompMult", "bitfieldReverse", "bitfieldExtract", "bitfieldInsert"}
_SAME_AS_LAST = {"step", "smoothstep"}
_SCALAR_F = {"length", "distance", "dot", "determinant"}
# Calls that write through an argument or have effects beyond their value.
_EFFECTS = {"modf", "frexp", "uaddCarry", "usubBorrow", "umulExtended", "imulExtended",
            "imageStore", "barrier", "memoryBarrier", "memoryBarrierShared",
            "memoryBarrierBuffer", "memoryBarrierImage", "groupMemoryBarrier", "EmitVertex",
            "EndPrimitive", "EmitStreamVertex", "EndStreamPrimitive", "atomicCounterIncrement",
            "atomicCounterDecrement", "interpolateAtCentroid", "interpolateAtSample",
            "interpolateAtOffset"} | {f for f in GLSL_BUILTIN_FUNCS if f.startswith(("atomic", "imageAtomic"))}
# Results that need neighbouring invocations (implicit derivatives).
_DERIV = {"texture", "textureOffset", "textureProj", "textureProjOffset", "dFdx", "dFdy",
          "fwidth", "dFdxCoarse", "dFdxFine", "dFdyCoarse", "dFdyFine", "fwidthCoarse",
          "fwidthFine", "textureQueryLod"}


class _Typer:
    """Work out the types of GLSL expressions over the code tokens c (no
    preprocessor lines), as far as the declarations tell. It gives None
    wherever it isn't sure."""

    def __init__(self, c):
        self.c, n = c, len(c)
        self.n = n
        self.structs, self.globals, self.funcs = {}, {}, {}
        self.fn_of = [-1] * n          # code index -> index of its function's '('
        self.locals = {}               # '(' index -> {name: type or None}
        depth = 0
        i = 0
        while i < n:
            t = c[i].text
            if t == "struct" and depth == 0 and self.is_id(i + 1) and self.txt(i + 2) == "{":
                close = self.match(i + 2, 1)
                self.structs[self.txt(i + 1)] = self._members(i + 3, close)
                i = close + 1
                continue
            if (t == "{" and depth == 0 and self.is_id(i - 1)
                    and self.txt(i - 1) not in GLSL_TYPES and i >= 2):
                close = self.match(i, 1)                   # interface block: NAME {
                mem = self._members(i + 1, close)
                if self.is_id(close + 1):
                    self.structs[self.txt(i - 1)] = mem
                    self.globals[self.txt(close + 1)] = ("o", self.txt(i - 1))
                else:
                    self.globals.update(mem)
                i = close + 1
                continue
            if t == "{" and depth == 0 and self.txt(i - 1) == ")":
                o = self.match(i - 1, -1)
                close = self.match(i, 1)
                for x in range(o, close + 1):
                    self.fn_of[x] = o
                rt = self.txt(o - 2)
                ps = self._params(o + 1, i - 1)
                self.funcs.setdefault(self.txt(o - 1), []).append((rt, ps, o))
                loc = {}
                for nm, ty, _ in ps:
                    loc[nm] = ty
                self._decls(o, close, loc)
                self.locals[o] = loc
                i = close + 1
                continue
            if t in ("{", "("):
                depth += 1
            elif t in ("}", ")"):
                depth -= 1
            elif depth == 0 and self.is_id(i) and (t in GLSL_TYPES or t in self.structs):
                self._declarators(i, self.globals)
            i += 1

    # small helpers
    def txt(self, i):
        return self.c[i].text if 0 <= i < self.n else None

    def is_id(self, i):
        return 0 <= i < self.n and self.c[i].type == T_ID

    def match(self, i, step):
        opens, closes = ("([{", ")]}") if step > 0 else (")]}", "([{")
        d = 0
        while 0 <= i < self.n:
            t = self.c[i]
            if t.type == T_OP and t.text in opens:
                d += 1
            elif t.type == T_OP and t.text in closes:
                d -= 1
                if d == 0:
                    return i
            i += step
        raise RenameError("unbalanced brackets")

    def tname(self, i):
        """The type named at c[i] (with an optional [N]) and the index past it."""
        t = self.txt(i)
        ty = _gtype(t) if t in GLSL_TYPES else ("o", t) if t in self.structs else None
        i += 1
        if ty and self.txt(i) == "[":
            ty, i = ("a", ty), self.match(i, 1) + 1
        return ty, i

    def _members(self, s, e):
        out = {}
        i = s
        while i < e:
            if self.is_id(i) and (self.txt(i) in GLSL_TYPES or self.txt(i) in self.structs):
                self._declarators(i, out)
                while i < e and self.txt(i) != ";":
                    i += 1
            i += 1
        return out

    def _declarators(self, i, into):
        """Record TYPE a[,b..] at c[i] in into, and return the index past it."""
        ty, j = self.tname(i)
        while self.is_id(j) and self.txt(j) not in _NEVER:
            if self.txt(j + 1) == "(":
                return j
            t2, k = ty, j + 1
            if self.txt(k) == "[":
                t2, k = ("a", ty), self.match(k, 1) + 1
            nm = self.txt(j)
            into[nm] = t2 if nm not in into or into[nm] == t2 else None
            d = 0
            while k < self.n and not (d == 0 and self.txt(k) in (",", ";", ")")):
                if self.c[k].type == T_OP:
                    d += {"(": 1, "[": 1, "{": 1, ")": -1, "]": -1, "}": -1}.get(self.txt(k), 0)
                k += 1
            if self.txt(k) != ",":
                return k
            j = k + 1
        return j

    def _params(self, s, e):
        out, i = [], s
        while i < e:
            quals = set()
            while self.is_id(i) and self.txt(i) in _QUALIFIERS:
                quals.add(self.txt(i))
                i += 1
            ty, j = self.tname(i)
            nm = self.txt(j) if self.is_id(j) else None
            if nm and self.txt(j + 1) == "[":
                ty = ("a", ty)
            if nm:
                out.append((nm, ty, quals))
            while i < e and self.txt(i) != ",":
                i += 1
            i += 1
        return out

    def _decls(self, s, e, into):
        for i in range(s, e):
            if (self.is_id(i) and (self.txt(i) in GLSL_TYPES or self.txt(i) in self.structs)
                    and self.txt(i - 1) in (";", "{", "}", "(", "const") and self.is_id(i + 1)
                    or self.is_id(i) and self.txt(i) in self.structs and self.is_id(i + 1)
                    and self.txt(i - 1) in (";", "{", "}", "(", "const")):
                if self.txt(i - 1) == "(" and self.txt(i - 2) not in ("for",):
                    continue
                self._declarators(i, into)

    def name_type(self, nm, at):
        if nm in _BUILTIN_VAR_TYPES:
            return _gtype(_BUILTIN_VAR_TYPES[nm])
        loc = self.locals.get(self.fn_of[at], {})
        if nm in loc:
            return loc[nm]
        return self.globals.get(nm)

    # expressions
    def expr(self, s, e, at=None):
        """Type of the whole expression c[s:e], or None."""
        try:
            ty, k = self._ternary(s, e)
        except (IndexError, KeyError, TypeError, RenameError):
            return None
        return ty if k == e else None

    def _ternary(self, s, e):
        ty, k = self._binary(s, e, 0)
        if k < e and self.txt(k) == "?":
            a, k = self._ternary(k + 1, e)
            if self.txt(k) != ":":
                return None, e + 1
            b, k = self._ternary(k + 1, e)
            return _join(a, b), k
        return ty, k

    def _binary(self, s, e, minbp):
        lhs, k = self._unary(s, e)
        while k < e:
            op = self.txt(k)
            bp = _BIND.get(op)
            if bp is None or self.c[k].type != T_OP or bp <= minbp:
                break
            rhs, k = self._binary(k + 1, e, bp)
            lhs = _arith(op, lhs, rhs)
        return lhs, k

    def _unary(self, s, e):
        t = self.txt(s)
        if self.c[s].type == T_OP and t in ("-", "+", "~", "!"):
            ty, k = self._unary(s + 1, e)
            return (("b", 1, 0) if t == "!" else ty), k
        if self.c[s].type == T_OP and t in ("++", "--"):
            return None, e + 1
        return self._postfix(s, e)

    def _postfix(self, s, e):
        ty, k = self._primary(s, e)
        while k < e:
            t = self.txt(k)
            if t == "." and self.is_id(k + 1):
                f = self.txt(k + 1)
                if self.txt(k + 2) == "(":              # .length()
                    ty, k = ("i", 1, 0), self.match(k + 2, 1) + 1
                    continue
                if ty and ty[0] in _RANK or ty and ty[0] == "b":
                    if not _SWIZZLE_RE.match(f) or ty[2]:
                        return None, e + 1
                    ty = (ty[0], len(f), 0)
                elif ty and ty[0] == "o" and ty[1] in self.structs:
                    ty = self.structs[ty[1]].get(f)
                else:
                    ty = None
                k += 2
            elif t == "[":
                k = self.match(k, 1) + 1
                if ty and ty[0] == "a":
                    ty = ty[1]
                elif ty and ty[0] in _RANK or ty and ty[0] == "b":
                    ty = (ty[0], ty[2], 0) if ty[2] else (ty[0], 1, 0) if ty[1] > 1 else None
                else:
                    ty = None
            elif t in ("++", "--"):
                return None, e + 1
            else:
                break
        return ty, k

    def _primary(self, s, e):
        tok = self.c[s]
        if tok.type == T_FLOAT:
            return (("d" if tok.text.lower().endswith("lf") else "f"), 1, 0), s + 1
        if tok.type == T_INT:
            return (("u" if tok.text[-1] in "uU" else "i"), 1, 0), s + 1
        if tok.text == "(":
            close = self.match(s, 1)
            ty, k = self._ternary(s + 1, close)
            return (ty if k == close else None), close + 1
        if tok.type != T_ID:
            return None, e + 1
        if tok.text in ("true", "false"):
            return ("b", 1, 0), s + 1
        if self.txt(s + 1) == "(":
            close = self.match(s + 1, 1)
            return self._call(tok.text, s + 1, close), close + 1
        if self.txt(s + 1) == "[" and (tok.text in GLSL_TYPES or tok.text in self.structs):
            return None, e + 1                           # array constructor
        return self.name_type(tok.text, s), s + 1

    def args(self, o, close):
        """[(start, end)] of the arguments of the call whose '(' is c[o]."""
        out, k, d, st = [], o + 1, 0, o + 1
        while k < close:
            t = self.txt(k)
            if self.c[k].type == T_OP and t in "([{":
                d += 1
            elif self.c[k].type == T_OP and t in ")]}":
                d -= 1
            elif t == "," and d == 0:
                out.append((st, k))
                st = k + 1
            k += 1
        if st < close:
            out.append((st, close))
        return out

    def _call(self, fn, o, close):
        if fn in GLSL_TYPES:
            return _gtype(fn)
        if fn in self.structs:
            return ("o", fn)
        a = self.args(o, close)
        if fn in self.funcs:
            defs = [d for d in self.funcs[fn] if len(d[1]) == len(a)]
            rts = {d[0] for d in defs}
            return _gtype(rts.pop()) if len(rts) == 1 else None
        types = [self.expr(s, e) for s, e in a]
        if fn in _SCALAR_F:
            return ("d" if types and types[0] and types[0][0] == "d" else "f", 1, 0)
        if fn in _SAME_AS_ARG0 and types:
            return types[0]
        if fn in _SAME_AS_LAST and types:
            return types[-1]
        if fn == "cross":
            return types[0] if types else None
        if fn == "transpose" and types and types[0] and types[0][0] in _RANK:
            return (types[0][0], types[0][2], types[0][1])
        if fn in ("lessThan", "lessThanEqual", "greaterThan", "greaterThanEqual", "equal",
                  "notEqual", "not"):
            return ("b", types[0][1], 0) if types and types[0] and types[0][0] != "o" else None
        if fn in ("any", "all", "isnan", "isinf") and fn in ("any", "all"):
            return ("b", 1, 0)
        if fn in ("floatBitsToInt", "floatBitsToUint", "intBitsToFloat", "uintBitsToFloat"):
            k = {"floatBitsToInt": "i", "floatBitsToUint": "u"}.get(fn, "f")
            return (k, types[0][1], 0) if types and types[0] and types[0][0] != "o" else None
        if fn.startswith("texture") and fn not in ("textureSize", "textureQueryLod",
                                                   "textureQueryLevels", "textureSamples"):
            smp = types[0] if types else None
            if not smp or smp[0] != "o":
                return None
            if "Shadow" in smp[1] and not fn.startswith("textureGather"):
                return ("f", 1, 0)
            return ({"i": "i", "u": "u"}.get(smp[1][0], "f"), 4, 0)
        if fn == "texelFetch":
            smp = types[0] if types else None
            return ({"i": "i", "u": "u"}.get(smp[1][0], "f"), 4, 0) if smp and smp[0] == "o" else None
        return None


def _join(a, b):
    if a == b:
        return a
    if a and b and a[0] in _RANK and b[0] in _RANK and a[1:] == b[1:]:
        return (max(a[0], b[0], key=_RANK.get),) + a[1:]
    return None


def _arith(op, a, b):
    if a is None or b is None or a[0] == "o" or b[0] == "o" or a[0] == "a" or b[0] == "a":
        return None
    if op in ("<", ">", "<=", ">=", "==", "!=", "&&", "||", "^^"):
        return ("b", 1, 0)
    if a[0] == "b" or b[0] == "b":
        return None
    if op in ("<<", ">>"):
        return a
    k = max(a[0], b[0], key=_RANK.get)
    if op in ("%", "&", "|", "^") and k not in ("i", "u"):
        return None
    if a[1:] == (1, 0):
        return (k,) + b[1:]
    if b[1:] == (1, 0):
        return (k,) + a[1:]
    if not a[2] and not b[2]:
        return (k,) + a[1:] if a[1] == b[1] else None
    if op != "*":
        return (k,) + a[1:] if a[1:] == b[1:] else None
    if a[2] and not b[2]:                                # mat * vec
        return (k, a[2], 0) if a[1] == b[1] else None
    if b[2] and not a[2]:                                # vec * mat
        return (k, b[1], 0) if a[1] == b[2] else None
    return (k, b[1], a[2]) if a[1] == b[2] else None     # mat * mat


def _stmts(ty, s, e):
    """Split the block body c[s:e] into statements: [(kind, start, end, parts)],
    with end exclusive. The kinds are:
      decl    parts is [(name, init_start, init_end)]
      expr
      return  parts is the (start, end) of its value
      if      parts is the condition's (start, end)
      other   loops, blocks, switch and jumps, which are never inlined into"""
    c, out, i = ty.c, [], s
    while i < e:
        t = ty.txt(i)
        if t == "{":
            k = ty.match(i, 1) + 1
            out.append(("other", i, k, None))
        elif t == "if":
            close = ty.match(i + 1, 1)
            k = _stmt_end(ty, close + 1, e)
            while ty.txt(k) == "else":
                k = _stmt_end(ty, k + 1, e)
            out.append(("if", i, k, (i + 2, close)))
        elif t in ("for", "while", "switch"):
            close = ty.match(i + 1, 1)
            k = _stmt_end(ty, close + 1, e)
            out.append(("other", i, k, None))
        elif t == "do":
            k = _stmt_end(ty, i + 1, e)                 # the body, then while(...);
            k = ty.match(k + 1, 1) + 2
            out.append(("other", i, k, None))
        else:
            k = i
            d = 0
            while k < e and not (d == 0 and ty.txt(k) == ";"):
                if c[k].type == T_OP and ty.txt(k) in "([{":
                    d += 1
                elif c[k].type == T_OP and ty.txt(k) in ")]}":
                    d -= 1
                k += 1
            k += 1
            j = i
            while ty.is_id(j) and ty.txt(j) in _QUALIFIERS:
                j += 1
            if t in ("return",):
                out.append(("return", i, k, (i + 1, k - 1)))
            elif (ty.is_id(j) and (ty.txt(j) in GLSL_TYPES or ty.txt(j) in ty.structs)
                  and ty.is_id(j + 1) and ty.txt(j + 2) in ("=", ",", ";")):
                quals = {ty.txt(x) for x in range(i, j)}
                parts, x = [], j + 1
                while True:
                    nm = x
                    if ty.txt(x + 1) == "=":
                        y, d = x + 2, 0
                        while not (d == 0 and ty.txt(y) in (",", ";")):
                            if c[y].type == T_OP and ty.txt(y) in "([{":
                                d += 1
                            elif c[y].type == T_OP and ty.txt(y) in ")]}":
                                d -= 1
                            y += 1
                        parts.append((nm, x + 2, y))
                    else:
                        y = x + 1
                        parts.append((nm, None, None))
                    if ty.txt(y) == "," and ty.is_id(y + 1):
                        x = y + 1
                        continue
                    break
                out.append(("decl" if quals <= {"const"} else "other", i, k, (j, parts)))
            elif t in ("break", "continue", "discard", ";"):
                out.append(("other", i, k, None))
            else:
                out.append(("expr", i, k, None))
        i = k
    return out


def _stmt_end(ty, i, e):
    """Index past the statement starting at c[i]."""
    t = ty.txt(i)
    if t == "{":
        return ty.match(i, 1) + 1
    if t == "if":
        k = _stmt_end(ty, ty.match(i + 1, 1) + 1, e)
        while ty.txt(k) == "else":
            k = _stmt_end(ty, k + 1, e)
        return k
    if t in ("for", "while", "switch"):
        return _stmt_end(ty, ty.match(i + 1, 1) + 1, e)
    if t == "do":
        k = _stmt_end(ty, i + 1, e)
        return ty.match(k + 1, 1) + 2
    d = 0
    while i < e and not (d == 0 and ty.txt(i) == ";"):
        if ty.c[i].type == T_OP and ty.txt(i) in "([{":
            d += 1
        elif ty.c[i].type == T_OP and ty.txt(i) in ")]}":
            d -= 1
        i += 1
    return i + 1


def _blocks(ty, s, e):
    """Every block body inside c[s:e] (a function body included): (start, end)."""
    out = [(s, e)]
    for i in range(s, e):
        if ty.txt(i) == "{" and ty.txt(i - 1) in (")", "else", "do", ";", "{", "}"):
            out.append((i + 1, ty.match(i, 1)))
    return out


def _operand_end(c, k):
    """Is c[k] the end of an operand (so an operator after it is binary)?"""
    return k >= 0 and (c[k].type in (T_INT, T_FLOAT) or c[k].type == T_ID
                       and c[k].text not in GLSL_KEYWORDS or c[k].text in (")", "]"))


def _top_bind(ty, s, e):
    """Return the loosest binding at depth 0 of c[s:e]. That's 1 for ?:, the
    _BIND value for its operators, 13 for a prefix operator, and 14 for a
    primary or postfix chain."""
    c, best, d = ty.c, 14, 0
    if c[s].type == T_OP and c[s].text in ("-", "+", "!", "~"):
        best = 13
    for k in range(s, e):
        t = c[k]
        if t.type != T_OP:
            continue
        if t.text in "([{":
            d += 1
        elif t.text in ")]}":
            d -= 1
        elif d == 0 and t.text == "?":
            best = min(best, 1)
        elif d == 0 and t.text in _BIND and _operand_end(c, k - 1):
            best = min(best, _BIND[t.text])
    return best


def inline_locals(tokens):
    """Fold a local that is read exactly once into that read. `float b=E;` is
    removed and the read of b becomes E. E gets brackets where the operators
    around it would otherwise bind into it. This only happens when it can't
    make a difference:
      - E's type, by the declarations, matches the variable's. Otherwise
        the conversion would move (`float b=i/2;` must stay a float).
      - The read comes later in the same block, in a plain statement, a
        declaration, a return or an if's condition, never in a loop, a
        nested block or an if's branches. So E is evaluated as many times as
        before, under the same control flow, with the same neighbours for
        derivatives. A ?: branch or the right side of && || only takes an E
        without calls.
      - Nothing between the two writes a name that E reads (an assignment,
        ++, an out argument, a call that writes globals). When E needs
        derivatives, nothing in between returns, discards, breaks or
        continues either.
      - E has no effects and only calls functions without effects.
      - The name doesn't appear on any preprocessor line.
    GLSL compilers keep values in SSA form, so they end up with the same
    expression as before. verify.py's frames confirm it."""
    toks = list(tokens)
    for _ in range(400):
        r = _inline_pass(toks)
        if r is None:
            return toks
        toks = r
    return toks


def _inline_pass(toks):
    code = [k for k, t in enumerate(toks) if t.type != T_PP]
    c = [toks[k] for k in code]
    try:
        ty = _Typer(c)
    except (RenameError, IndexError):
        return None
    n = len(c)
    ppnames = set()
    for t in toks:
        if t.type == T_PP:
            ppnames.update(re.findall(r"[A-Za-z_]\w*", t.text))
    # For each function, work out whether it's pure (no global writes, no out
    # parameters, no effects) and whether it needs derivatives, including
    # through the functions it calls.
    info = {}
    for fn, defs in ty.funcs.items():
        for rt, ps, o in defs:
            close = ty.match(ty.match(o, 1) + 1, 1)
            loc = ty.locals[o]
            pure = not any(q & {"out", "inout"} for _, _, q in ps)
            deriv, calls = False, set()
            for k in range(o, close):
                t = c[k]
                if t.type != T_ID:
                    continue
                if ty.txt(k + 1) == "(":
                    if t.text in _EFFECTS:
                        pure = False
                    if t.text in _DERIV:
                        deriv = True
                    if t.text in ty.funcs:
                        calls.add(t.text)
                elif ty.txt(k - 1) != "." and t.text not in loc and _is_write(ty, k):
                    pure = False
            info.setdefault(fn, [True, False, set()])
            info[fn][0] &= pure
            info[fn][1] |= deriv
            info[fn][2] |= calls
    changed = True
    while changed:
        changed = False
        for fn, (pure, deriv, calls) in info.items():
            p2 = pure and all(info[g][0] for g in calls if g in info)
            d2 = deriv or any(info[g][1] for g in calls if g in info)
            if (p2, d2) != (pure, deriv):
                info[fn][0], info[fn][1] = p2, d2
                changed = True
    edits, used = [], []
    for fn, defs in ty.funcs.items():
        for rt, ps, o in defs:
            bo = ty.match(o, 1) + 1
            bc = ty.match(bo, 1)
            if any(toks[x].type == T_PP for x in range(code[bo], code[bc])):
                continue
            for bs, be in _blocks(ty, bo + 1, bc):
                try:
                    sts = _stmts(ty, bs, be)
                except (RenameError, IndexError):
                    continue
                for p, (kind, ss, se, parts) in enumerate(sts):
                    if kind != "decl":
                        continue
                    tj, decls = parts
                    tdecl = _gtype(ty.txt(tj)) if ty.txt(tj) in GLSL_TYPES else ("o", ty.txt(tj))
                    for di, (nm, es, ee) in enumerate(decls):
                        cand = _inline_candidate(ty, info, ppnames, sts, p, di, nm, es, ee,
                                                 tdecl, bc)
                        if cand is None:
                            continue
                        lo, hi, u = ss, se, cand
                        if any(not (hi <= a or b <= lo) or a <= u < b for a, b in used) or \
                           any(lo <= x < hi for x in (uu for _, _, uu in edits)):
                            continue
                        rm = ((ss, se) if len(decls) == 1 else (nm, ee + 1) if di == 0
                              else (nm - 1, ee))
                        edits.append((rm, (es, ee), u))
                        used.append((lo, hi))
                        used.append((u, u + 1))
    if not edits:
        return None
    # Apply the edits: the read becomes E (in brackets where needed), and the
    # declarator is removed.
    out = list(toks)
    ops = []
    for (rs, re_), (es, ee), u in edits:
        e_toks = [c[k] for k in range(es, ee)]
        if _needs_brackets(ty, u, _top_bind(ty, es, ee)):
            e_toks = [Token(T_OP, "(")] + e_toks + [Token(T_OP, ")")]
        ops.append((code[u], code[u] + 1, e_toks))
        ops.append((code[rs], code[re_ - 1] + 1, []))
    for a, b, rep in sorted(ops, key=lambda x: -x[0]):
        out[a:b] = rep
    return out


def _needs_brackets(ty, u, pe):
    c = ty.c
    left, right = c[u - 1], c[u + 1] if u + 1 < len(c) else Token(T_OP, ";")
    if left.type == T_OP and left.text in _BIND and _operand_end(c, u - 2):
        if pe <= _BIND[left.text]:
            return True
    elif left.type == T_OP and left.text in ("-", "+", "!", "~"):
        if pe < 13:
            return True
    elif left.type == T_OP and left.text in ("?", ":") and pe <= 1:
        return True
    if right.type == T_OP:
        if right.text in _BIND and pe < _BIND[right.text]:
            return True
        if right.text in (".", "[", "++", "--") and pe < 14:
            return True
        if right.text == "?" and pe <= 1:
            return True
    return False


def _is_write(ty, k):
    """Does the name at c[k] get written (assigned, ++/--, an out argument)?"""
    c, n = ty.c, ty.n
    j = k + 1
    while j < n:
        if ty.txt(j) == "." and ty.is_id(j + 1):
            j += 2
        elif ty.txt(j) == "[":
            j = ty.match(j, 1) + 1
        else:
            break
    if ty.txt(j) in _ASSIGN_OPS or ty.txt(j) in ("++", "--"):
        return True
    if ty.txt(k - 1) in ("++", "--"):
        return True
    if ty.txt(k - 1) in ("(", ",") and ty.txt(j) in (",", ")"):
        o, d, arg = k - 1, 0, 0
        while o >= 0:
            t = ty.txt(o)
            if t in (")", "]"):
                d += 1
            elif t in ("(", "["):
                if d == 0:
                    break
                d -= 1
            elif t == "," and d == 0:
                arg += 1
            o -= 1
        if o > 0 and ty.txt(o) == "(" and ty.is_id(o - 1):
            fn = ty.txt(o - 1)
            if fn in _EFFECTS:
                return True
            for _, ps, _ in ty.funcs.get(fn, ()):
                if arg < len(ps) and ps[arg][2] & {"out", "inout"}:
                    return True
    return False


def _inline_candidate(ty, info, ppnames, sts, p, di, nm, es, ee, tdecl, bc):
    """The read of the local declared at c[nm] = c[es:ee], if it can take E."""
    c = ty.c
    name = ty.txt(nm)
    if es is None or name in ppnames or ty.txt(nm + 1) == "[":
        return None
    reads = [k for k in range(ee, bc) if c[k].type == T_ID and c[k].text == name
             and ty.txt(k - 1) != "."]
    if len(reads) != 1:
        return None
    u = reads[0]
    if _is_write(ty, u):
        return None
    # Check E: it must have no effects and only call functions without effects.
    # Also note which names it reads and whether it needs derivatives.
    names, calls, deriv = set(), False, False
    for k in range(es, ee):
        t = c[k]
        if t.type == T_OP and (t.text in _ASSIGN_OPS or t.text in ("++", "--", ",")):
            if t.text == "," and _depth_at(ty, es, k):
                continue
            return None
        if t.type != T_ID or ty.txt(k - 1) == ".":
            continue
        if ty.txt(k + 1) == "(":
            if t.text in _EFFECTS:
                return None
            if t.text in info:
                if not info[t.text][0]:
                    return None
                calls = True
                deriv |= info[t.text][1]
            elif t.text in _DERIV or t.text.startswith("texture"):
                deriv = True
                calls = True
            elif t.text in GLSL_BUILTIN_FUNCS:
                calls = True
        elif t.text not in GLSL_TYPES and t.text not in GLSL_KEYWORDS:
            names.add(t.text)
    # The read must be in this declaration's later initializers, or in a later
    # plain statement, declaration, return or if condition of the same block.
    q = next((x for x in range(p, len(sts)) if sts[x][1] <= u < sts[x][2]), None)
    if q is None:
        return None
    kind, ss, se, parts = sts[q]
    if q == p:
        pass
    elif kind == "if":
        if not parts[0] <= u < parts[1]:
            return None
    elif kind not in ("decl", "expr", "return"):
        return None
    cond_ctx = any(ty.txt(k) in ("?", "&&", "||") and c[k].type == T_OP for k in range(ss, se))
    if cond_ctx and calls:
        return None
    # Check what lies between E and its read.
    globals_read = any(nm2 not in ty.locals.get(ty.fn_of[es], {}) for nm2 in names)
    for k in range(ee, u):
        t = c[k]
        if t.type != T_ID:
            continue
        if deriv and t.text in ("return", "discard", "break", "continue"):
            return None
        if ty.txt(k + 1) == "(" and t.text in info and not info[t.text][0] and (globals_read or calls):
            return None
        if ty.txt(k + 1) == "(" and t.text in _EFFECTS:
            return None
        if t.text in names and ty.txt(k - 1) != "." and _is_write(ty, k):
            return None
    # Check where E lands. GLSL compilers still rewrite expression trees
    # before SSA. They regroup a quotient's reciprocal or a sum's constant
    # across a product, turn ?: and && into branches, and let a swizzle narrow
    # a vector call. So E only goes where no such rewrite can see more of it
    # than before. E itself must not contain ?:, && or ||. Unless it's a
    # single token, it can't go under a swizzle, an index, a unary operator or
    # a divisor. It only goes into a product if it's a product itself (or a
    # call or a name) without / or %. We checked this on Mesa: each excluded
    # shape changed some program's SSA graph, and everything allowed left all
    # of them alone.
    if any(c[k].type == T_OP and c[k].text in ("?", "&&", "||") for k in range(es, ee)):
        return None
    single = ee - es == 1
    left = ty.txt(u - 1) if c[u - 1].type == T_OP else None
    right = ty.txt(u + 1) if u + 1 < ty.n and c[u + 1].type == T_OP else None
    if not single and (right in (".", "[", "++", "--")
                       or left in ("-", "+", "!", "~") and not _operand_end(c, u - 2)
                       or left == "/"):
        return None
    if left in ("*", "/", "%") or right in ("*", "/", "%"):
        if not single and (_top_bind(ty, es, ee) < 12 or any(
                c[k].type == T_OP and c[k].text in ("/", "%") for k in range(es, ee))):
            return None
    if ty.expr(es, ee) != tdecl:
        return None
    return u


def _depth_at(ty, s, k):
    d = 0
    for x in range(s, k):
        if ty.c[x].type == T_OP and ty.txt(x) in "([{":
            d += 1
        elif ty.c[x].type == T_OP and ty.txt(x) in ")]}":
            d -= 1
    return d


# brackets that change nothing: `mix(1.,(1.-a),b)` -> `mix(1.,1.-a,b)`

def drop_brackets(tokens):
    """Remove every bracket pair that the parse doesn't need, so the compiler
    builds the same expression tree as before. A pair can go if it doesn't
    belong to a call, a constructor or a statement (if, for, while, layout)
    and has no comma or assignment at its top level. Its contents must also
    bind at least as tightly as the operators beside it require (see
    _needs_brackets)."""
    toks = list(tokens)
    while True:
        code = [k for k, t in enumerate(toks) if t.type != T_PP]
        c = [toks[k] for k in code]
        try:
            ty = _Typer(c)
        except (RenameError, IndexError):
            return toks
        drop = set()
        for i, t in enumerate(c):
            if t.type != T_OP or t.text != "(" or i in drop:
                continue
            if i > 0 and (c[i - 1].type == T_ID and c[i - 1].text != "return"
                          or c[i - 1].text in (")", "]")):
                continue
            j = ty.match(i, 1)
            if j == i + 1 or any(x in drop for x in (j,)):
                continue
            d, plain = 0, True
            for k in range(i + 1, j):
                if c[k].type == T_OP and c[k].text in "([{":
                    d += 1
                elif c[k].type == T_OP and c[k].text in ")]}":
                    d -= 1
                elif d == 0 and c[k].type == T_OP and (c[k].text == "," or c[k].text in _ASSIGN_OPS):
                    plain = False
                    break
            if not plain or _group_needs(ty, i, j, _top_bind(ty, i + 1, j)):
                continue
            drop.update((i, j))
        if not drop:
            return toks
        toks = [t for k, t in enumerate(toks) if k not in {code[x] for x in drop}]


def _group_needs(ty, i, j, pe):
    """Do the operators around c[i..j] (a bracketed group) need its brackets?"""
    c = ty.c
    left = c[i - 1] if i > 0 else Token(T_OP, ";")
    right = c[j + 1] if j + 1 < len(c) else Token(T_OP, ";")
    if left.type == T_OP and left.text in _BIND and _operand_end(c, i - 2):
        if pe <= _BIND[left.text]:
            return True
    elif left.type == T_OP and left.text in ("-", "+", "!", "~"):
        if pe < 13:
            return True
    elif left.type == T_OP and left.text in ("?", ":") and pe <= 1:
        return True
    if right.type == T_OP:
        if right.text in _BIND and pe < _BIND[right.text]:
            return True
        if right.text in (".", "[", "++", "--") and pe < 14:
            return True
        if right.text == "?" and pe <= 1:
            return True
    return False


# functions nothing calls are dropped before anything is counted

def drop_uncalled(tokens):
    """Remove the functions a shader never calls, starting from main and
    following the functions it calls. The shared blocks bring in helpers a
    stage doesn't use, and their uses would skew the renaming's counts. A
    function named on a preprocessor line or in a file-scope initializer
    counts as called. A name defined more than once is kept or dropped as a
    whole."""
    toks = list(tokens)
    code = [k for k, t in enumerate(toks) if t.type != T_PP]
    c = [toks[k] for k in code]
    n = len(c)
    defs, depth, start = {}, 0, 0
    i = 0
    while i < n:
        t = c[i]
        if t.type == T_OP and t.text == "{" and depth == 0 and i and c[i - 1].text == ")":
            o = _match_back(c, i - 1)
            if o < 1 or c[o - 1].type != T_ID:
                return toks
            close = _skip_balanced(c, i, n) - 1
            defs.setdefault(c[o - 1].text, []).append((start, close))
            i = close + 1
            start = i
            continue
        if t.type == T_OP and t.text in "({[":
            depth += 1
        elif t.type == T_OP and t.text in ")}]":
            depth -= 1
        elif t.type == T_OP and t.text == ";" and depth == 0:
            start = i + 1
        i += 1
    if "main" not in defs:
        return toks
    called = {"main"}
    for t in toks:
        if t.type == T_PP:
            called.update(re.findall(r"[A-Za-z_]\w*", t.text))
    inside = set()
    for spans in defs.values():
        for s0, e0 in spans:
            inside.update(range(s0, e0 + 1))
    for k in range(n - 1):                           # file-scope initializers
        if k not in inside and c[k].type == T_ID and c[k + 1].text == "(":
            called.add(c[k].text)
    todo = list(called & set(defs))
    while todo:
        for s0, e0 in defs[todo.pop()]:
            for k in range(s0, e0):
                nm = c[k].text
                if c[k].type == T_ID and c[k + 1].text == "(" and nm in defs and nm not in called:
                    called.add(nm)
                    todo.append(nm)
    drop = set()
    for nm, spans in defs.items():
        if nm not in called:
            for s0, e0 in spans:
                drop.update(code[k] for k in range(s0, e0 + 1))
    return [t for k, t in enumerate(toks) if k not in drop]


# public API

def minify_set(sources, rename=False, protect=(), int_arith=False, inline=True):
    """Minify {name: source} together and return ({name: text}, mapping).

    With *rename*, every shader shares one mapping (see plan_renames). Locals
    read once are folded into their read (inline_locals, unless *inline* is
    off). Declarations are merged, and integral float literals are written
    as ints (int_literals, and with *int_arith* in arithmetic too).
    """
    tokens = {name: tokenize(src) for name, src in sources.items()}
    if not rename:
        return {name: emit(toks) for name, toks in tokens.items()}, {}
    tokens = {name: drop_uncalled(toks) for name, toks in tokens.items()}
    plan, mapping = plan_renames(tokens, protect)
    fold = inline_locals if inline else (lambda toks: toks)
    return ({name: emit(drop_brackets(int_literals(merge_declarations(
                 fold(apply_renames(toks, *plan[name]))), int_arith)))
             for name, toks in tokens.items()}, mapping)


def minify(src, do_rename=False, protect=(), name="", inline=True):
    """Minify GLSL source text and return the minified text.

    *name* only matters with *do_rename*: its extension gives the stage.
    """
    return minify_set({name: src}, do_rename, protect, inline=inline)[0][name]


# self test: a handful of realistic shaders, run end to end

VERT_SRC = """\
#version 330 core
// model-view-projection transform
uniform mat4 uModelViewProj;   /* MVP */
in  vec3 aPosition;
in  vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    vTexCoord   = aTexCoord;
    gl_Position = uModelViewProj * vec4(aPosition, 1.0);
}
"""

FRAG_SRC = """\
#version 330 core
uniform float iTime;
uniform vec2  iResolution;
out vec4 fragColor;

float sdSphere(vec3 p, float r) {
    return length(p) - r;   // signed distance
}

void main() {
    vec2 uv = (gl_FragCoord.xy - 0.5 * iResolution.xy) / iResolution.y;
    vec3 ro = vec3(0.0, 0.0, -3.0);
    vec3 rd = normalize(vec3(uv, 1.0));
    float t = 0.0;
    for (int i = 0; i < 64; ++i) {
        vec3 pos = ro + rd * t;
        float d  = sdSphere(pos, 1.0);
        if (d < 0.001) break;
        t += d;
    }
    vec3 col = vec3(1.0 - t * 0.1);
    fragColor = vec4(col.xyz, 1.0);
}
"""

DEFINE_SRC = """\
#version 300 es
precision highp float;
#define PI 3.14159265
#define TAU (2.0 * PI)
#ifdef HIGH_QUALITY
  #define STEPS 128
#else
  #define STEPS 64
#endif
uniform float u_time;
out vec4 o_color;
void main() {
    float a = PI * u_time;
    o_color = vec4(sin(a), cos(a), 0.0, 1.0);
}
"""

TRICKY_SRC = """\
void main() {
    float a = 1.0;
    float b = 2.0;
    float c = a - -b;         // must not become a--b
    float d = 1.0e-5;
    float e = 2.0E+3;
    vec3  v = vec3(1.0);
    float s = v.x + v.y;
    for (int i = 0; i < 10; ++i) { c += float(i); }
}
"""


# Two stages and a second program sharing a helper, for the --rename tests.
SET_VERT = """\
#version 330 core
layout(location = 0) in vec3 aPos;
in vec3 aLoose;                       // no location: the C side may bind it
layout(std140) uniform Frame { mat4 uViewProj; float uTime; };
uniform float uScale;
out vec3 vWorld;
void main() {
    vec3 world = aPos * uScale + aLoose;
    vWorld = world;
    gl_Position = uViewProj * vec4(world, 1.0);
}
"""

SET_FRAG = """\
#version 330 core
#define STEPS 4
#ifdef EXTERNAL_FLAG
#define GAIN 2.0
#endif
struct Light { vec3 dir; float power; vec3 x; };
layout(std140) uniform Frame { mat4 uViewProj; float uTime; };
uniform sampler2D uTex;
in vec3 vWorld;
out vec4 fragColor;
float shade = 0.5;
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5); }
float lightIt(Light l, vec3 n) { return max(dot(n, l.dir), 0.0) * l.power + l.x.y; }
void main() {
    Light sun = Light(vec3(0.0, 1.0, 0.0), 1.0, vec3(0.0));
    vec3 n = normalize(vWorld);
    float acc = 0.0;
    for (int i = 0; i < STEPS; ++i) acc += hash(n.xy + float(i));
    float shade = acc * lightIt(sun, n) + shade;      // shadows the global
    vec4 c = texture(uTex, n.xy * uTime);
    fragColor = vec4(c.rgb * shade, unknownBuiltin(c.a));
}
"""

SET_OTHER = """\
#version 330 core
in vec3 vWorld;
out vec4 o;
float hash(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5); }
void main() { o = vec4(hash(vWorld.xz)); }
"""

MERGE_SRC = """\
#version 330 core
uniform float uA;
uniform float uB;
layout(location = 0) out vec4 o;
struct S { float a; float b; };
void main() {
    float x = 1.0;
    float y = x * 2.0;
    if (y > 1.0) float z = 3.0;
    float w = y;
    const vec2 k = vec2(1.0);
    const vec2 m = k;
    o = vec4(x + y + w + uA + uB, k + m, 1.0);
}
"""


def _ids(text):
    """Identifiers in minified text that are not right after a '.'."""
    return set(re.findall(r"(?<![\w.])[A-Za-z_]\w*", text))


def _selftest():
    failures = [0]

    def check(cond, msg):
        if not cond:
            print("    FAIL: " + msg)
            failures[0] += 1
        return cond

    def no_comments(out):
        return "//" not in out and "/*" not in out and "*/" not in out

    def test_vertex():
        out = minify(VERT_SRC)
        for name in ("uModelViewProj", "aPosition", "aTexCoord", "vTexCoord"):
            check(name in out, "vertex: identifier %r preserved" % name)
        check("gl_Position" in out, "vertex: gl_Position preserved")
        check(no_comments(out), "vertex: no comments remain")
        check("1." in out, "vertex: 1.0 shortened to 1.")
        check("vec4(aPosition,1.)" in out, "vertex: packed call present")
        check(minify(out) == out, "vertex: idempotent")

    def test_fragment():
        out = minify(FRAG_SRC)
        for name in ("iTime", "iResolution", "fragColor", "sdSphere"):
            check(name in out, "fragment: identifier %r preserved" % name)
        check(".xy" in out, "fragment: .xy swizzle preserved")
        check(".xyz" in out, "fragment: .xyz swizzle preserved")
        check(".5" in out, "fragment: 0.5 shortened to .5")
        check("for(int i=0;i<64;++i)" in out, "fragment: for-loop packed")
        check(no_comments(out), "fragment: no comments remain")
        check(minify(out) == out, "fragment: idempotent")

    def test_define():
        out = minify(DEFINE_SRC)
        for needle in ("#version 300 es", "#define PI 3.14159265",
                       "#define TAU (2.0 * PI)", "#ifdef HIGH_QUALITY",
                       "#define STEPS 128", "#else", "#define STEPS 64",
                       "#endif", "precision highp float"):
            check(needle in out, "define: directive %r preserved" % needle)
        for name in ("u_time", "o_color"):
            check(name in out, "define: identifier %r preserved" % name)
        check(no_comments(out), "define: no comments remain")
        # every directive must stand on its own line
        for line in out.splitlines():
            if line.startswith("#"):
                check("#" not in line[1:] or line.count("#") == 1,
                      "define: directive not merged (%r)" % line)
        check(minify(out) == out, "define: idempotent")

    def test_tricky():
        out = minify(TRICKY_SRC)
        check("a--b" not in out, "tricky: a - -b did not fuse into a--b")
        check("- -" in out, "tricky: unary minus kept apart")
        check("1.e-5" in out, "tricky: 1.0e-5 -> 1.e-5 (exponent intact)")
        check("2.E+3" in out, "tricky: 2.0E+3 -> 2.E+3 (exponent intact)")
        check("i<10" in out, "tricky: comparison packed with no spaces")
        check("++i" in out, "tricky: increment preserved")
        check(".x" in out and ".y" in out, "tricky: swizzles preserved")
        check(no_comments(out), "tricky: no comments remain")
        check(minify(out) == out, "tricky: idempotent")

    def test_rename():
        base = minify(FRAG_SRC)
        out = minify(FRAG_SRC, do_rename=True, name="ray.frag")
        # Public interface must survive renaming verbatim.
        for name in ("iTime", "iResolution", "fragColor", "main"):
            check(name in out, "rename: %r preserved" % name)
        check("gl_FragCoord" in out, "rename: gl_FragCoord preserved")
        check(".xy" in out and ".xyz" in out, "rename: swizzles preserved")
        check(no_comments(out), "rename: no comments remain")
        check(len(out) < len(base), "rename: smaller than plain minify")
        for name in ("sdSphere", "pos", "col", "uv"):
            check(name not in _ids(out), "rename: %r was renamed" % name)
        check(minify(FRAG_SRC, do_rename=True, name="ray.frag") == out,
              "rename: deterministic")

    def test_rename_set():
        srcs = {"a.vert": SET_VERT, "a.frag": SET_FRAG, "b.frag": SET_OTHER}
        out, mapping = minify_set(srcs, rename=True)
        vert, frag, other = out["a.vert"], out["a.frag"], out["b.frag"]
        for name in ("uScale", "uTex", "Frame", "aLoose", "fragColor", "o",
                     "EXTERNAL_FLAG", "unknownBuiltin", "main", "gl_Position"):
            check(any(name in _ids(t) for t in out.values()),
                  "set: %r kept verbatim" % name)
        for name in ("vWorld", "aPos", "uViewProj", "uTime", "hash", "Light",
                     "dir", "power", "STEPS", "GAIN", "shade", "world", "acc"):
            check(all(name not in _ids(t) and "." + name not in t
                      for t in out.values()), "set: %r renamed" % name)
        v = mapping.get("vWorld")
        check(v and re.search(r"out vec3 %s;" % v, vert)
              and re.search(r"in vec3 %s;" % v, frag)
              and re.search(r"in vec3 %s;" % v, other),
              "set: varying renamed identically in every stage")
        block = re.search(r"uniform Frame\{[^}]*\}", vert)
        check(block and block.group() in frag,
              "set: uniform block members renamed identically")
        h = mapping.get("hash")
        fv = re.search(r"float %s\(vec2 \w+\)\{[^}]*\}" % h, frag)
        check(fv and fv.group() in other,
              "set: a helper shared by two files stays byte-identical")
        check(".x.y" in frag and ".xy" in frag and ".xyz" in frag and ".rgb" not in frag
              and ".a" not in frag and ".w)" in frag,
              "set: swizzles in xyzw, a swizzle-named member left alone")
        t2, _ = minify_set({"s.frag": "#version 330 core\nstruct S{vec4 r;};uniform S u;out vec4 o;"
                                      "void main(){vec4 v=u.r;o=vec4(v.r,v.gba);}"}, rename=True)
        check(".r)" not in t2["s.frag"] and ".r;" in t2["s.frag"] and ".yzw" in t2["s.frag"],
              "set: a member spelled r keeps every .r verbatim")
        check("x" not in mapping, "set: swizzle-named member 'x' kept")
        check("#ifdef EXTERNAL_FLAG" in frag, "set: external macro kept")
        check(re.search(r"#define %s 4\n" % mapping["STEPS"], frag)
              and re.search(r"<%s;" % mapping["STEPS"], frag),
              "set: macro renamed in its #define and its use")
        check(re.search(r"\.%s\)" % mapping["dir"], frag)
              and re.search(r"vec3 %s;" % mapping["dir"], frag),
              "set: struct member renamed in declaration and after '.'")
        g = mapping["shade"]
        check(re.search(r"float %s=\.5;" % g, frag)
              and re.search(r"float %s=\w+\*\w+\([^;]*\)\+%s;" % (g, g), frag),
              "set: a local shadowing a global keeps the shadowing")
        check(minify_set(srcs, rename=True) == (out, mapping),
              "set: deterministic")

    def test_rename_fail_closed():
        for bad in ("void main(){ { }", "struct { float a; } s; void main(){}",
                    "void main(){ struct S { float a; } s; }"):
            try:
                minify_set({"x.frag": bad}, rename=True)
            except RenameError:
                continue
            check(False, "fail closed: %r raises RenameError" % bad)
        out = minify("#version 330 core\n#define CAT(a,b) a##b\nfloat f;"
                     "void main(){float CAT(q,r)=f;}", do_rename=True, name="x.frag")
        check("#define CAT(a,b) a##b" in out, "fail closed: ## macro left alone")
        out = minify("in vec3 p; void main(){ gl_Position = vec4(p, 1.0); }",
                     do_rename=True, name="x.vert")
        check("vec3 p;" in out, "fail closed: unlocated vertex input kept")
        out = minify("uniform vec4 uA;layout(location=1)uniform vec4 uLong;"
                     "layout(binding=2)uniform sampler2D uTex;out vec4 c;"
                     "void main(){c=uA+uLong+texture(uTex,uA.xy);}", do_rename=True, name="x.frag")
        check("vec4 uA;" in out and "uLong" not in out and "uTex" not in out,
              "uniforms: unplaced kept, located and bound renamed")
        out = minify("in vec3 p; out vec4 c; void main(){ c = vec4(p, 1.0); }",
                     do_rename=True, name="x.glsl")
        check("vec3 p;" in out and "vec4 c;" in out,
              "fail closed: unknown stage keeps unlocated in/out")

    def test_merge():
        out = minify(MERGE_SRC, do_rename=True, name="m.frag", inline=False)
        check("uniform float uA,uB;" in out, "merge: uniform declarations folded")
        check(re.search(r"\{float \w+;float \w+;\}", out),
              "merge: struct members untouched")
        check(re.search(r"float (\w+)=1,(\w+)=\1\*2\.;", out),
              "merge: consecutive locals folded in order")
        check(re.search(r"\)float \w+=3;float \w+=\w+;", out),
              "merge: a declaration that is an if body stays alone")
        check(re.search(r"const vec2 (\w+)=vec2\(1\),\w+=\1;", out),
              "merge: const declarations folded")
        check(re.search(r"layout\(location=0\)out vec4 \w+;struct", out),
              "merge: layout declaration kept alone (a located output renames)")
        check(merge_declarations(tokenize(out)) == tokenize(out),
              "merge: nothing left to fold on a second pass")

    def test_inline():
        def body(src, pre=""):
            out = minify("#version 330 core\nuniform vec4 uP;uniform sampler2D uT;out vec4 o;" + pre + src,
                         do_rename=True, name="t.frag")
            return out.split("\n", 1)[1]
        out = body("void main(){float a=uP.x+uP.y;vec3 n=normalize(uP.xyz);o=vec4(n,a);}")
        check("vec4(normalize(" in out and "x+" in out and "float" not in out,
              "inline: read-once locals fold into their read")
        out = body("void main(){int i=int(uP.x);float a=i/2;o=vec4(a+uP.y);}")
        check(re.search(r"float (\w+)=[^;]*/2;o=vec4\(\1\+", out), "inline: a conversion stays put")
        out = body("void main(){float a=uP.x*2.;o=vec4(0);for(int i=0;i<3;i++)o+=a;}")
        check(re.search(r"float \w+=\w+\.x\*2\.;", out), "inline: never into a loop")
        out = body("void main(){float a=uP.x;o=vec4(1);if(uP.y>0.){o=vec4(a);}}")
        check(re.search(r"float \w+=\w+\.x;", out), "inline: never into an if's branch")
        out = body("void main(){float a=uP.x;if(a>0.)o=vec4(1);}")
        check("if(uP.x>0" in out or re.search(r"if\(\w+\.x>0", out),
              "inline: into an if's condition")
        out = body("void main(){vec4 p=uP;float a=p.x;p.x=1.;o=vec4(a,p.yzw);}")
        check(re.search(r"float \w+=\w+\.x;", out), "inline: not past a write to what it reads")
        out = body("float g;float f(float x){g=x;return x;}"
                   "void main(){float a=g;float b=f(uP.x);o=vec4(a,b,0,1);}")
        check(re.search(r"float \w+=\w+,\w+=\w+\(", out),
              "inline: not past a call that writes a global, nor that call")
        out = body("void main(){vec4 a=texture(uT,uP.xy);if(uP.z>0.)discard;o=a;}")
        check("vec4" in out.split("discard")[0], "inline: derivatives not past a discard")
        out = body("void main(){float a=uP.x+1.,b=uP.y/3.,c=uP.z>0.?1.:2.;o=vec4(uP.w*a,uP.w*b,c,1);}")
        check(re.search(r"float \w+=\w+\.x\+1\.", out) and re.search(r"\w+=\w+\.y/3\.", out)
              and re.search(r"\w+=\w+\.z>0\?", out),
              "inline: no sums, quotients or ?: into a product")
        out = body("void main(){vec3 a=reflect(uP.xyz,vec3(0,1,0));o=vec4(a.y);}")
        check(re.search(r"vec3 \w+=reflect", out), "inline: a swizzled read takes a name only")
        out = body("void main(){float a=sin(uP.x);o=vec4(uP.y*a);}")
        check("*sin(" in out, "inline: a call into a product")
        out = body("void main(){float a=uP.x*uP.z;o=vec4(uP.y*a);}")
        check(re.search(r"\*\(\w+\.x\*\w+\.z\)", out), "inline: a product into a product, bracketed")

    def test_brackets():
        def br(src):
            return emit(drop_brackets(tokenize(src)))
        out = br("void main(){float a=mix(1.,(1.-b),c),d=(a*b)*c,e=a*(b*c),f=a-(b-c),g=(a+b)*c,"
                 "h=-(a*b),i=(a).x,j=(a>b)?c:d,k=(a,b),l=(c=d);if((a)>0.)return(a+b)*c;}")
        check("mix(1.,1.-b,c)" in out and "d=a*b*c" in out and "e=a*(b*c)" in out
              and "f=a-(b-c)" in out and "g=(a+b)*c" in out and "h=-(a*b)" in out
              and "i=a.x" in out and "j=a>b?c:d" in out and "k=(a,b)" in out
              and "l=(c=d)" in out and "if(a>0.)" in out and "return(a+b)*c" in out,
              "brackets: only the ones the parse does not need")

    def test_overloads():
        src = ("#version 330 core\nout vec4 o;float add1(float x){return x+1.;}"
               "float mul2(float x,float y){return x*y*2.;}float sub1(float x){return x-1.;}"
               "void main(){o=vec4(add1(o.x),mul2(o.y,o.z),sub1(o.w),1);}")
        out, mapping = minify_set({"t.frag": src, "u.frag": "#version 330 core\nout vec4 o;"
                                   "float mul2;void main(){o=vec4(mul2);}"}, rename=True)
        check(mapping["add1"] != mapping["sub1"], "overload: one argument count, one name")
        check(mapping["mul2"] != mapping["add1"] and mapping["mul2"] != mapping["sub1"],
              "overload: a name that is also a variable stays alone")
        out, mapping = minify_set({"t.frag": src.replace("float mul2", "float mul3").replace("mul2(o", "mul3(o")},
                                  rename=True)
        check(mapping["mul3"] in (mapping["add1"], mapping["sub1"]),
              "overload: different argument counts share a name")

    def test_uncalled():
        out = emit(drop_uncalled(tokenize(
            "#define M(x) used(x)\nfloat used(float a){return a;}float dead(float a){return a;}"
            "float leaf(float a){return a;}float mid(float a){return leaf(a);}"
            "const float k=init(1.);float init(float a){return a;}"
            "void main(){float b=mid(1.);}")))
        check("dead" not in out and "used" in out and "leaf" in out and "init" in out,
              "uncalled: dropped, unless called, named on a directive or in an initializer")

    def test_int_literals():
        def body(src):
            out = minify("#version 330 core\nout vec4 o;" + src, do_rename=True, name="t.frag",
                         inline=False)
            return out.split("\n", 1)[1]
        out = body("void main(){float a=2.0,b=a*3.0;vec3 v=vec3(1.0,0.0,-2.0);o=vec4(v*b,1.0);}")
        check("=2," in out and "*3.;" in out and "vec3(1,0,-2)" in out and ",1)" in out,
              "intlit: initializers and constructor args; arithmetic kept by default")

        def arith(src):
            return emit(int_literals(tokenize("out vec4 o;" + src), arith=True))
        out = arith("void main(){float a=2.0,b=a*3.0;o=vec4(b);}")
        check("*3;" in out, "intlit(arith): a float partner")
        out = arith("void main(){int i=7;float a=float(i)/2.0;float b=1.0/float(i);o=vec4(a,b,0,1);}")
        check(")/2;" in out and "=1/float(" in out, "intlit(arith): a float() partner divides as float")
        out = arith("void main(){int i=7,j=2;float a=float(i/j)*2.0;int k=i/2;float c=(i+1)*3.0;o=vec4(a,k,c,1);}")
        check("*2;" in out and "=i/2;" in out and ")*3.;" in out,
              "intlit(arith): float(int/int)*lit; int/int and int sums untouched")
        out = arith("void main(){float x=.5;o=vec4(3.0-2.0*x,1.0-abs(x),(x+.5)/8.0,1);}")
        check("3-2*x" in out and "1-abs(x)" in out and "/8," in out,
              "intlit(arith): whole partner sub-expressions are typed")
        out = body("void main(){int i=3;float a=i+1.0;float b=i<2.0?1.0:0.0;o=vec4(a,b,0,1);}")
        check("+1.," in out and "<2.?1.:0;" in out,
              "intlit: int partners keep their floats; one ternary branch converts")
        out = body("float h(float x,int k){return x*2.0;}void main(){int x=1;float v=h(1.0,2);"
                   "o=vec4(v>0.?2.0:v,x>0?3.0:1.0,0,1);}")
        check(re.search(r"return \w+\*2\.;", out) and re.search(r"=\w+\(1,2\)", out)
              and re.search(r"\?2:\w+", out) and "?3.:1," in out and ">0?" in out,
              "intlit: per-function types, user float parameters, one ternary side")
        out = body("float f(float x){return x*2.0;}float f(int x){return 1.0;}"
                   "void main(){o=vec4(f(1.0),max(1,2.0),pow(2.0,3.0),1);}")
        check("(1.)" in out and "return 1.;" in out and "max(1,2.)" in out and "pow(2.,3)" in out,
              "intlit: overloaded user calls, returns (Mesa converts no return value) and "
              "int-overloaded built-ins keep their floats")
        out = body("void main(){vec3 v=vec3(.5);float s=.5;"
                   "o=vec4(smoothstep(0.0,1.0,v),pow(s,2.0))+vec4(pow(v,vec3(2.0)),mix(0.,1.,s));}")
        check("smoothstep(0,1," in out and re.search(r"pow\(\w+,2\)", out)
              and "mix(0.,1.," in out and "vec3(2)" in out, "intlit: float-only built-in slots only")
        out = body("uniform sampler2D t;void main(){float x=.3;o=vec4(textureLod(t,vec2(x),0.0).r,"
                   "x<max(x,1.0)?1:0,clamp(x*2.,0.0,1.0),1);}")
        check(",0).x" in out and "max(" in out and ",1)?1:0" in out and ",0,1)" in out,
              "intlit: textureLod's lod; max/clamp after a float first argument")
        out = arith("void main(){float x=.3;int i=2;o=vec4(1.0-abs(x),2.0*abs(i),0,1);}")
        check("vec4(1-abs(" in out and "2.*abs(" in out,
              "intlit(arith): abs() takes its first argument's type")
        out = arith("void main(){float a=2.0*3.0;o=vec4(a);}")
        check("2.*3;" in out, "intlit(arith): never both sides of one operator")

    def test_float_edges():
        check(shorten_float("1.0") == "1.", "float: 1.0 -> 1.")
        check(shorten_float("0.5") == ".5", "float: 0.5 -> .5")
        check(shorten_float("10.00") == "10.", "float: 10.00 -> 10.")
        check(shorten_float("1.0e3") == "1.e3", "float: 1.0e3 -> 1.e3")
        check(shorten_float("0.0") == "0.", "float: 0.0 -> 0.")
        check(shorten_float("2.0E+3") == "2.E+3", "float: 2.0E+3 -> 2.E+3")
        check(shorten_float(".5") == ".5", "float: .5 stays .5")
        check(shorten_float("1.5") == "1.5", "float: 1.5 stays 1.5")

    tests = [
        ("vertex_shader", test_vertex),
        ("raymarch_fragment", test_fragment),
        ("define_ifdef", test_define),
        ("tricky_ops_floats", test_tricky),
        ("rename_single_file", test_rename),
        ("rename_cross_file", test_rename_set),
        ("int_literals", test_int_literals),
        ("inline_locals", test_inline),
        ("drop_brackets", test_brackets),
        ("overloads", test_overloads),
        ("drop_uncalled", test_uncalled),
        ("rename_fail_closed", test_rename_fail_closed),
        ("merge_declarations", test_merge),
        ("float_shortening", test_float_edges),
    ]
    for name, fn in tests:
        before = failures[0]
        fn()
        status = "PASS" if failures[0] == before else "FAIL"
        print("[%s] %s" % (status, name))

    if failures[0]:
        print("SELFTEST FAILED: %d assertion(s) failed" % failures[0])
        return 1
    print("SELFTEST PASSED: all tests OK")
    return 0


# CLI

def build_parser():
    p = argparse.ArgumentParser(
        prog="glslmin.py",
        description="Safe, deterministic GLSL minifier (standard library only).",
    )
    p.add_argument("input", nargs="*",
                   help='input .vert/.frag/.glsl file(s), or "-" for stdin')
    p.add_argument("-o", "--output", help="output file (stdout if omitted)")
    p.add_argument("--outdir", help="write each input to DIR/<its file name>")
    p.add_argument("--stats", action="store_true",
                   help="print original vs minified byte counts to stderr")
    p.add_argument("--rename", action="store_true",
                   help="rename identifiers with one mapping across all inputs")
    p.add_argument("--protect", default="",
                   help="comma-separated identifiers --rename must never touch")
    p.add_argument("--map", help="write the --rename mapping to this file")
    p.add_argument("--int-arith", action="store_true",
                   help="with --rename, also write integral literals in arithmetic as ints "
                        "(same values; drivers may round fused multiply-adds differently)")
    p.add_argument("--selftest", action="store_true",
                   help="run the built-in test suite and exit")
    return p


def _write(path, text):
    if path:
        with open(path, "w", encoding="utf-8", newline="") as fh:
            fh.write(text)
    else:
        sys.stdout.write(text)


def main(argv=None):
    args = build_parser().parse_args(argv)

    if args.selftest:
        return _selftest()

    if not args.input:
        print("glslmin: error: INPUT is required (use '-' for stdin)",
              file=sys.stderr)
        return 2
    if len(args.input) > 1 and not args.outdir:
        print("glslmin: error: several inputs need --outdir", file=sys.stderr)
        return 2
    if args.outdir and (args.output or "-" in args.input):
        print("glslmin: error: --outdir takes file inputs and no -o", file=sys.stderr)
        return 2
    names = [os.path.basename(x) if x != "-" else "-" for x in args.input]
    if len(set(names)) != len(names):
        print("glslmin: error: input file names must be distinct", file=sys.stderr)
        return 2

    sources = {}
    try:
        for path, name in zip(args.input, names):
            if path == "-":
                sources[name] = sys.stdin.read()
            else:
                with open(path, "r", encoding="utf-8") as fh:
                    sources[name] = fh.read()
    except OSError as exc:
        print("glslmin: error: cannot read %s: %s" % (path, exc),
              file=sys.stderr)
        return 2

    protect = [s for s in args.protect.split(",") if s]
    try:
        out, mapping = minify_set(sources, rename=args.rename, protect=protect,
                                  int_arith=args.int_arith)
    except RenameError as exc:
        print("glslmin: error: cannot rename: %s" % exc, file=sys.stderr)
        return 1

    try:
        if args.outdir:
            os.makedirs(args.outdir, exist_ok=True)
        for name in names:
            _write(os.path.join(args.outdir, name) if args.outdir else args.output,
                   out[name])
        if args.map:
            _write(args.map, "".join("%s %s\n" % kv for kv in sorted(mapping.items())))
    except OSError as exc:
        print("glslmin: error: cannot write output: %s" % exc, file=sys.stderr)
        return 2

    if args.stats:
        orig = sum(len(x.encode("utf-8")) for x in sources.values())
        mini = sum(len(x.encode("utf-8")) for x in out.values())
        pct = (100.0 * mini / orig) if orig else 0.0
        print("glslmin: %d -> %d bytes (%.1f%%)" % (orig, mini, pct),
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
