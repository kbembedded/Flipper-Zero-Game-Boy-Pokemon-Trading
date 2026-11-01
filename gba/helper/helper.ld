/* The helper runs from EWRAM, where the BIOS loads it, with its .iwram code copied to IWRAM by
 * crt0.S (multiboot-and-save.md §4.4). crt0.o comes first on the link line. */
OUTPUT_FORMAT("elf32-littlearm")
OUTPUT_ARCH(arm)
ENTRY(_start)
MEMORY {
    ewram (rwx) : ORIGIN = 0x02000000, LENGTH = 256K
    iwram (rwx) : ORIGIN = 0x03000000, LENGTH = 0x6000  /* 0x03006000..0x03007EFF: stack */
}
SECTIONS {
    /* The interworking glue and veneers the linker makes for ARM <-> Thumb calls live with the
     * code. */
    .text : {
        KEEP(*(.header))
        *(.text .text.*)
        *(.glue_7 .glue_7t .vfp11_veneer .v4_bx)
        *(.rodata .rodata.*)
        . = ALIGN(4);
    } > ewram
    .iwram : { __iwram_start = .; *(.iwram .iwram.*) . = ALIGN(4); __iwram_end = .; } > iwram AT > ewram
    __iwram_lma = LOADADDR(.iwram);
    .data : { *(.data .data.*) . = ALIGN(16); } > ewram   /* payload length a multiple of 0x10 */
    __image_end = .;
    .bss (NOLOAD) : { __bss_start = .; *(.bss .bss.* COMMON) . = ALIGN(4); __bss_end = .; } > ewram
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.attributes) *(.comment) }
}
