#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Identifies the cart in the slot (multiboot-and-save.md §3.2, §3.3 step 1, §3.7). It only reads
 * the ROM, through hp_hal_rom16/rom32, and never writes anywhere in the ROM area. */

/* The save library a ROM links, from its ID string (GBATEK "GBA Cart Backup IDs"). The values are
 * CART_INFO's wire values. */
typedef enum {
    HpLibNone, /* no ID string */
    HpLibFlash1M, /* "FLASH1M_V": 128 KB flash, what every Gen III Pokemon game links */
    HpLibFlash, /* "FLASH_V": 64 KB flash */
    HpLibFlash512, /* "FLASH512_V": 64 KB flash */
    HpLibSram, /* "SRAM_V" or "SRAM_F_V" */
    HpLibEeprom, /* "EEPROM_V" */
} HpLib;

typedef struct {
    bool logo_ok; /* 0x04..0x9F equal the helper's own header's logo, which the BIOS checked */
    bool fixed_ok; /* [0xB2] == 0x96 */
    bool complement_ok; /* [0xBD] is the header complement (multiboot-and-save.md §2.1) */
    uint32_t rom_size; /* multiboot-and-save.md §3.7; 0 if the logo's first word isn't 0x51AEFF24 */
    HpLib lib; /* the first FLASH1M_V string, else the first other library string */
    uint32_t lib_off; /* its offset; 0 with HpLibNone */
    bool other_lib; /* a library string other than FLASH1M_V was found */
    bool write_game; /* game code AXVE, AXPE, BPRE or BPGE: the English Ruby, Sapphire, FireRed
                      * and LeafGreen */
    uint8_t code[4]; /* the game code, 0xAC..0xAF */
} HpCart;

/* Checks the header and scans the whole ROM for library strings, in 32-bit steps. On the GBA this
 * takes several seconds (16 MiB). */
void hp_cart_scan(HpCart* out);
