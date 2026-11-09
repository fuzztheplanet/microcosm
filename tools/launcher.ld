/* launcher.ld - lay MICROCOSM's launcher (the first stage, tools/s1dec.S) out as
 * a raw image of a single read-write-execute PT_LOAD at 0x400000. pack.py links
 * it with --oformat=binary. s1dec.S's own ELF header comes first, then code and
 * data, with no page padding and no interpreter, dynamic section, notes or
 * unwind tables. The .bss tables, and past them the second stage at S2_ADDR with
 * its own .bss (pack.py passes its end as s2_mem_end), only add to the segment's
 * memory size. The header reads both sizes from the symbols below. */
ENTRY(_start)
SECTIONS {
    . = 0x400000;
    .text : { KEEP(*(.elfhdr)) *(.text .text.*) *(.rodata .rodata.*) *(.data .data.*) }
    mc_file_size = . - 0x400000;
    .bss : { *(.bss .bss.*) *(COMMON) }
    ASSERT(. <= 0x800000, "the first stage runs into the second (S2_ADDR)")
    mc_mem_size = MAX(., s2_mem_end) - 0x400000;
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.sframe) *(.interp) *(.dynamic) }
}
