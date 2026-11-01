/* The helper image header and start-up code (multiboot-and-save.md §4.3). The BIOS loads the image
 * at 0x02000000 and enters it at 0xC0 in ARM state, system mode. Header fields follow
 * multiboot-and-save.md §2.1; fix_header.py writes the logo and the complement. */

#include "hp_version.h"

    .syntax unified
    .section .header, "ax", %progbits
    .arm
    .global _start
_start:
    b       mb_start                @ 0x00 ROM entry, unused by multiboot
    .fill   156, 1, 0               @ 0x04 logo, written by fix_header.py
    .ascii  "FLIPPERGEN3 "          @ 0xA0 title, 12 bytes
    .ascii  "ZFGE"                  @ 0xAC game code, not a real one
    .ascii  "00"                    @ 0xB0 maker
    .byte   0x96                    @ 0xB2 fixed value
    .byte   0x00, 0x00              @ 0xB3 unit code, 0xB4 device type
    .fill   7, 1, 0                 @ 0xB5 reserved
    .byte   HP_PROTO_VERSION        @ 0xBC version
    .byte   0x00                    @ 0xBD complement, written by fix_header.py
    .fill   2, 1, 0                 @ 0xBE reserved
    b       mb_start                @ 0xC0 MultiPlay and Normal entry
    .byte   0, 0                    @ 0xC4 boot mode, 0xC5 slave id: the BIOS writes 3 (MultiPlay) or 2 (Normal), and 1
    .fill   26, 1, 0                @ 0xC6 .. 0xDF
    b       mb_start                @ 0xE0 JOY Bus entry
mb_start:
    mov     r0, #0x04000000
    mov     r1, #0
    str     r1, [r0, #0x208]        @ IME = 0
    msr     cpsr_c, #0xD2           @ IRQ mode, I and F masked
    ldr     sp, =0x03007FA0
    msr     cpsr_c, #0xDF           @ system mode, I and F masked
    ldr     sp, =0x03007F00
    ldr     r0, =__iwram_lma        @ copy .iwram from its load address in EWRAM
    ldr     r1, =__iwram_start
    ldr     r2, =__iwram_end
1:  cmp     r1, r2
    ldrlo   r3, [r0], #4
    strlo   r3, [r1], #4
    blo     1b
    ldr     r1, =__bss_start        @ zero .bss: it lies past the image, and the BIOS leaves it
    ldr     r2, =__bss_end
    mov     r3, #0
2:  cmp     r1, r2
    strlo   r3, [r1], #4
    blo     2b
    ldr     r0, =main
    mov     lr, pc
    bx      r0                      @ into main, in Thumb state
3:  b       3b
    .pool
