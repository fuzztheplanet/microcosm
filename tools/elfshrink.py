#!/usr/bin/env python3
"""elfshrink.py - drop the ELF section-header table (what sstrip does, in Python).

The section-header table is bookkeeping for linkers and debuggers, and the
kernel loader never reads it. A release binary can lose it, along with the tail
of the file it sat in, as long as every program segment and the program-header
table stay byte-for-byte intact. The tool checks the ELF header first, and it
refuses anything but a 64-bit little-endian x86-64 ELF rather than risk
corrupting it.

With --golf it also blanks what the loaders never need in a fixed-address
executable that imports nothing by name, such as the payload (src/mc_start.c
finds its imports by hash). The PT_PHDR entry goes, since the kernel passes
AT_PHDR anyway and the load bias it gives ld.so is 0 for ET_EXEC. The empty
dynamic symbol table loses its hash table (the DT_HASH and DT_SYMENT entries
and the table's bytes), since ld.so skips a map without hash buckets. DT_SYMTAB
itself stays, because glibc's ld.so crashes at startup without it. Blanked bytes
become zeros, which cost the packer next to nothing, and no offset moves.

Usage:
    python3 elfshrink.py INPUT -o OUTPUT [--verify] [--golf]
"""

import argparse
import os
import struct
import sys

# --- ELF64 header layout (little-endian) -----------------------------------
# e_ident[16], then:
#   H  e_type        @16
#   H  e_machine     @18
#   I  e_version     @20
#   Q  e_entry       @24
#   Q  e_phoff       @32
#   Q  e_shoff       @40
#   I  e_flags       @48
#   H  e_ehsize      @52
#   H  e_phentsize   @54
#   H  e_phnum       @56
#   H  e_shentsize   @58
#   H  e_shnum       @60
#   H  e_shstrndx    @62
EI_NIDENT = 16
E_SHOFF_OFF = 40
E_SHENTSIZE_OFF = 58
E_SHNUM_OFF = 60
E_SHSTRNDX_OFF = 62

EM_X86_64 = 0x3E
ELFCLASS64 = 2
ELFDATA2LSB = 1

# ELF64 program-header layout: p_type, p_flags, p_offset, p_vaddr,
# p_paddr, p_filesz, p_memsz, p_align  ->  I I Q Q Q Q Q Q (56 bytes)
PHDR_FMT = "<IIQQQQQQ"


class ElfError(Exception):
    """Raised when the input is not a 64-bit little-endian x86-64 ELF."""


def _u16(buf, off):
    return struct.unpack_from("<H", buf, off)[0]


def _u64(buf, off):
    return struct.unpack_from("<Q", buf, off)[0]


def parse_elf64(data):
    """Validate the ELF64 header and return the fields we care about.

    Raises ElfError (with a clear message) if the file is not a 64-bit
    little-endian x86-64 ELF, so we never silently corrupt it.
    """
    if len(data) < 64:
        raise ElfError("file too small to be an ELF64 (%d bytes)" % len(data))
    if data[0:4] != b"\x7fELF":
        raise ElfError("bad ELF magic (not an ELF file)")
    if data[4] != ELFCLASS64:
        raise ElfError("not a 64-bit ELF (EI_CLASS=%d, expected 2)" % data[4])
    if data[5] != ELFDATA2LSB:
        raise ElfError("not little-endian (EI_DATA=%d, expected 1)" % data[5])
    e_machine = _u16(data, 18)
    if e_machine != EM_X86_64:
        raise ElfError(
            "not x86-64 (e_machine=0x%X, expected 0x3E)" % e_machine)

    return {
        "e_phoff": _u64(data, 32),
        "e_shoff": _u64(data, E_SHOFF_OFF),
        "e_ehsize": _u16(data, 52),
        "e_phentsize": _u16(data, 54),
        "e_phnum": _u16(data, 56),
        "e_shentsize": _u16(data, E_SHENTSIZE_OFF),
        "e_shnum": _u16(data, E_SHNUM_OFF),
    }


def compute_min_size(data, hdr):
    """Minimal file size that keeps all program segments and the phdr table."""
    min_size = hdr["e_ehsize"] or 64  # never drop the ELF header itself

    phoff = hdr["e_phoff"]
    phnum = hdr["e_phnum"]
    phentsize = hdr["e_phentsize"]
    if phoff and phnum:
        # Keep the whole program-header table.
        min_size = max(min_size, phoff + phnum * phentsize)
        # Each segment's file bytes must remain present.
        for i in range(phnum):
            off = phoff + i * phentsize
            if off + 56 > len(data):
                raise ElfError(
                    "program header %d runs past end of file" % i)
            ph = struct.unpack_from(PHDR_FMT, data, off)
            p_offset, p_filesz = ph[2], ph[5]
            min_size = max(min_size, p_offset + p_filesz)
    return min_size


PT_DYNAMIC, PT_PHDR, ET_EXEC = 2, 6, 2
DT_NULL, DT_HASH, DT_SYMTAB, DT_SYMENT = 0, 4, 6, 11
DT_RELOCS = {7, 17, 23}         # DT_RELA, DT_REL, DT_JMPREL: something to resolve by name


def golf(data):
    """The --golf blanking described above. Returns (new_bytes, notes). A file
    that isn't ET_EXEC comes back unchanged, and each blanking step is skipped
    when the input doesn't look like the expected payload."""
    out = bytearray(data)
    hdr = parse_elf64(data)
    notes = []
    if _u16(data, 16) != ET_EXEC:
        return bytes(out), ["not ET_EXEC: no golf"]
    phoff, phnum, phsz = hdr["e_phoff"], hdr["e_phnum"], hdr["e_phentsize"]
    phs = [struct.unpack_from(PHDR_FMT, data, phoff + i * phsz) for i in range(phnum)]

    def file_off(vaddr, size):
        for ph in phs:
            if ph[0] == 1 and ph[3] <= vaddr and vaddr + size <= ph[3] + ph[5]:
                return ph[2] + vaddr - ph[3]
        return None

    dyn = [ph for ph in phs if ph[0] == PT_DYNAMIC]
    if len(dyn) == 1:
        off, size = dyn[0][2], dyn[0][5]
        ents = [struct.unpack_from("<qQ", data, off + k) for k in range(0, size - 15, 16)]
        ents = ents[:next(i for i, e in enumerate(ents) if e[0] == DT_NULL)]
        tags = {t: v for t, v in ents}
        h = file_off(tags.get(DT_HASH, 0), 8)
        if (DT_HASH in tags and DT_SYMTAB in tags and not DT_RELOCS & tags.keys() and h is not None
                and struct.unpack_from("<I", data, h + 4)[0] == 1):     # nchain 1: the null symbol only
            nbucket = struct.unpack_from("<I", data, h)[0]
            keep = [e for e in ents if e[0] not in (DT_HASH, DT_SYMENT)]
            out[off:off + size] = bytes(size)
            for k, e in enumerate(keep):
                struct.pack_into("<qQ", out, off + 16 * k, *e)
            out[h:h + 8 + 4 * (nbucket + 1)] = bytes(8 + 4 * (nbucket + 1))
            notes.append("dropped the empty symbol table's hash (DT_HASH, DT_SYMENT)")
    keep = [ph for ph in phs if ph[0] != PT_PHDR]
    if len(keep) == phnum - 1:
        out[phoff:phoff + phnum * phsz] = bytes(phnum * phsz)
        for i, ph in enumerate(keep):
            struct.pack_into(PHDR_FMT, out, phoff + i * phsz, *ph)
        struct.pack_into("<H", out, 56, phnum - 1)
        notes.append("dropped PT_PHDR")
    return bytes(out), notes


def shrink(data):
    """Return (new_bytes, info) with the section-header table removed.

    info is a dict describing what happened (for reporting / idempotency).
    """
    hdr = parse_elf64(data)
    min_size = compute_min_size(data, hdr)

    out = bytearray(data)
    # Remove the section-header table by zeroing the e_sh* header fields.
    struct.pack_into("<Q", out, E_SHOFF_OFF, 0)      # e_shoff   = 0
    struct.pack_into("<H", out, E_SHENTSIZE_OFF, 0)  # e_shentsize = 0
    struct.pack_into("<H", out, E_SHNUM_OFF, 0)      # e_shnum   = 0
    struct.pack_into("<H", out, E_SHSTRNDX_OFF, 0)   # e_shstrndx = 0

    e_shoff = hdr["e_shoff"]
    truncated = False
    note = ""

    if min_size > len(data):
        # Corrupt / already-truncated input: never grow or cut into data.
        note = "min size exceeds file length; refusing to truncate"
        new = bytes(out)
    elif e_shoff == 0:
        # Already stripped. Truncate any trailing slack for idempotency.
        new = bytes(out[:min_size])
        truncated = len(new) < len(data)
        note = ("already section-header-less; "
                + ("trimmed trailing bytes" if truncated else "no change"))
    elif e_shoff >= min_size:
        # Normal case: the section-header table (and .shstrtab etc.) sits in
        # the droppable tail, after every segment. Safe to truncate.
        new = bytes(out[:min_size])
        truncated = True
        note = "removed section headers and truncated tail"
    else:
        # SAFETY: the section headers are not at the very end (loadable data
        # or the phdr table follows them). Truncating to drop them would cut
        # into required bytes, so we only zero the fields and keep the size.
        new = bytes(out)
        note = ("section headers are not at the end of the file; "
                "zeroed e_sh* fields only, did NOT truncate (no size win)")

    info = {
        "min_size": min_size,
        "orig_shoff": e_shoff,
        "truncated": truncated,
        "note": note,
    }
    return new, info


def verify(path, expected_phnum=None):
    """Re-parse OUTPUT: phdr table present, every segment fits in the file."""
    with open(path, "rb") as fh:
        data = fh.read()
    hdr = parse_elf64(data)
    size = len(data)

    phoff = hdr["e_phoff"]
    phnum = hdr["e_phnum"]
    phentsize = hdr["e_phentsize"]
    if phoff + phnum * phentsize > size:
        raise ElfError("program-header table extends past end of file")
    if expected_phnum is not None and phnum != expected_phnum:
        raise ElfError("program-header count changed (%d != %d)"
                       % (phnum, expected_phnum))
    for i in range(phnum):
        off = phoff + i * phentsize
        ph = struct.unpack_from(PHDR_FMT, data, off)
        p_offset, p_filesz = ph[2], ph[5]
        if p_offset + p_filesz > size:
            raise ElfError(
                "segment %d file range [%d,%d) exceeds file size %d"
                % (i, p_offset, p_offset + p_filesz, size))
    return phnum


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Strip the ELF64 section-header table (sstrip equivalent).")
    ap.add_argument("input", help="input ELF64 file")
    ap.add_argument("-o", "--output", required=True, help="output file")
    ap.add_argument("--verify", action="store_true",
                    help="re-parse OUTPUT and check segments are intact")
    ap.add_argument("--golf", action="store_true",
                    help="also blank PT_PHDR and an empty dynamic symbol table (see above)")
    args = ap.parse_args(argv)

    try:
        with open(args.input, "rb") as fh:
            data = fh.read()
    except OSError as exc:
        print("error: cannot read input: %s" % exc, file=sys.stderr)
        return 1

    try:
        hdr = parse_elf64(data)
        new, info = shrink(data)
        golfed = []
        if args.golf:
            new, golfed = golf(new)
        expected_phnum = parse_elf64(new)["e_phnum"]
    except ElfError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 2

    try:
        with open(args.output, "wb") as fh:
            fh.write(new)
    except OSError as exc:
        print("error: cannot write output: %s" % exc, file=sys.stderr)
        return 1

    # Preserve the executable permission bit (chmod +x, mirroring input).
    try:
        in_mode = os.stat(args.input).st_mode
    except OSError:
        in_mode = 0o755
    exec_bits = in_mode & 0o111
    try:
        cur = os.stat(args.output).st_mode
        os.chmod(args.output, cur | exec_bits | 0o100)
    except OSError:
        pass

    before, after = len(data), len(new)
    saved = before - after
    print("elfshrink: %s -> %s" % (args.input, args.output))
    print("  %s" % info["note"])
    for g in golfed:
        print("  golf: %s" % g)
    print("  before: %d bytes" % before)
    print("  after:  %d bytes" % after)
    print("  saved:  %d bytes (%.1f%%)"
          % (saved, (100.0 * saved / before) if before else 0.0))

    if args.verify:
        try:
            phnum = verify(args.output, expected_phnum)
            print("  verify: OK (%d program headers, all segments fit)"
                  % phnum)
        except ElfError as exc:
            print("  verify: FAILED - %s" % exc, file=sys.stderr)
            return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
