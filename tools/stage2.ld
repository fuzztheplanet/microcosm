/* stage2.ld - lay the launcher's second stage out at S2_ADDR, as the raw image
 * the first stage decodes into place. The second stage is the payload decoder
 * and the memfd glue. The shipped one is tools/s2dec.S, which pack.py assembles
 * and links with this script. The C reference and fallback, tools/mcstub.c
 * built with -DMC_STAGE2, links the same way. The entry (s2_main, in
 * .text.entry) comes first, then code and data. The .bss follows the image, and
 * s2_mem_end tells the first stage's header how much memory the whole launcher
 * needs. */
ENTRY(s2_main)
SECTIONS {
    . = 0x800000;
    .text : { *(.text.entry) *(.text .text.*) *(.rodata .rodata.*) *(.data .data.*) }
    .bss : { *(.bss .bss.*) *(COMMON) }
    s2_mem_end = .;
    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) *(.sframe) *(.interp) *(.dynamic) }
}
