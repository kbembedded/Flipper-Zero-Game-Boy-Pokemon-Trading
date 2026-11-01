#include "hp_cart.h"
#include "hp_hal.h"

#define HP_MIB 0x100000u
#define HP_ROM_MAX (32u * HP_MIB) /* the cart bus's ROM window, 0x08000000..0x09FFFFFF */
/* The logo's first word (multiboot-and-save.md §2.1): no cart without it (FIX94 main.c:
 * getGameSize) */
#define HP_LOGO_WORD0 0x51AEFF24u
/* Halfwords compared at each power of two for the ROM's end (FIX94 main.c: getGameSize) */
#define HP_END_HALFWORDS 0x1000u

/* The first word of each library ID string, as rom32 reads it (multiboot-and-save.md §3.3) */
#define HP_WORD_FLAS 0x53414C46u /* "FLAS" */
#define HP_WORD_SRAM 0x4D415253u /* "SRAM" */
#define HP_WORD_EEPR 0x52504545u /* "EEPR" */

static uint8_t hp_cart_byte(uint32_t off) {
    const uint16_t hw = hp_hal_rom16(off & ~1u);
    return (off & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

/* Past the ROM's end the cart bus returns the halfword address, (addr / 2) & 0xFFFF (GBATEK "GBA
 * Unpredictable Things"); a ROM may instead mirror itself there. */
static bool hp_cart_end_at(uint32_t off) {
    bool open_bus = true;
    bool mirror = true;
    for(uint32_t j = 0; j < HP_END_HALFWORDS && (open_bus || mirror); j++) {
        const uint16_t hw = hp_hal_rom16(off + 2u * j);
        if(hw != (uint16_t)((off >> 1) + j)) open_bus = false;
        if(hw != hp_hal_rom16(2u * j)) mirror = false;
    }
    return open_bus || mirror;
}

/* multiboot-and-save.md §3.7: the first power of two from 1 MiB at which the ROM ends. */
static uint32_t hp_cart_rom_size(void) {
    if(hp_hal_rom32(4) != HP_LOGO_WORD0) return 0;
    for(uint32_t size = HP_MIB; size < HP_ROM_MAX; size <<= 1) {
        if(hp_cart_end_at(size)) return size;
    }
    return HP_ROM_MAX;
}

static bool hp_cart_match(uint32_t off, uint32_t rom_size, const char* s) {
    uint32_t len = 0;
    while(s[len]) len++;
    if(off > rom_size || len > rom_size - off) return false;
    for(uint32_t i = 0; i < len; i++) {
        if(hp_cart_byte(off + i) != (uint8_t)s[i]) return false;
    }
    return true;
}

/* The library a string at off names (GBATEK "GBA Cart Backup IDs"), or HpLibNone. */
static HpLib hp_cart_lib_at(uint32_t off, uint32_t rom_size) {
    if(hp_cart_match(off, rom_size, "FLASH1M_V")) return HpLibFlash1M;
    if(hp_cart_match(off, rom_size, "FLASH512_V")) return HpLibFlash512;
    if(hp_cart_match(off, rom_size, "FLASH_V")) return HpLibFlash;
    if(hp_cart_match(off, rom_size, "SRAM_V")) return HpLibSram;
    if(hp_cart_match(off, rom_size, "SRAM_F_V")) return HpLibSram;
    if(hp_cart_match(off, rom_size, "EEPROM_V")) return HpLibEeprom;
    return HpLibNone;
}

/* The first 32-bit step in [off, end) whose word starts "FLAS", "SRAM" or "EEPR", or end. This is
 * the whole-ROM scan's inner loop, one rom32 per word (multiboot-and-save.md §3.3 step 1), so on
 * the GBA it runs from IWRAM, and a first letter outside 'E'..'S' costs one test. */
HP_IWRAM static uint32_t hp_cart_next_candidate(uint32_t off, uint32_t end) {
    if(off >= end) return end;
    do {
        const uint32_t w = hp_hal_rom32(off);
        if((uint8_t)(w - 'E') <= 'S' - 'E' &&
           (w == HP_WORD_FLAS || w == HP_WORD_SRAM || w == HP_WORD_EEPR)) {
            return off;
        }
        off += 4;
    } while(off < end);
    return end;
}

static void hp_cart_scan_libs(HpCart* c) {
    const uint32_t end = c->rom_size;
    for(uint32_t off = hp_cart_next_candidate(0, end); off < end;
        off = hp_cart_next_candidate(off + 4u, end)) {
        const HpLib lib = hp_cart_lib_at(off, end);
        if(lib == HpLibNone) continue;
        if(lib != HpLibFlash1M) c->other_lib = true;
        if(c->lib == HpLibFlash1M) continue;
        if(lib == HpLibFlash1M || c->lib == HpLibNone) {
            c->lib = lib;
            c->lib_off = off;
        }
    }
}

/* The English Ruby, Sapphire, FireRed and LeafGreen (multiboot-and-save.md §3.2). */
static bool hp_cart_write_game(const uint8_t code[4]) {
    static const char games[4][5] = {"AXVE", "AXPE", "BPRE", "BPGE"};
    for(uint32_t g = 0; g < 4; g++) {
        if(code[0] == games[g][0] && code[1] == games[g][1] && code[2] == games[g][2] &&
           code[3] == games[g][3]) {
            return true;
        }
    }
    return false;
}

void hp_cart_scan(HpCart* out) {
    const uint8_t* own = hp_hal_own_header();
    out->logo_ok = true;
    for(uint32_t off = 0x04; off < 0xA0; off += 2) {
        if(hp_hal_rom16(off) != (uint16_t)(own[off] | own[off + 1] << 8)) out->logo_ok = false;
    }
    out->fixed_ok = hp_cart_byte(0xB2) == 0x96;
    uint8_t chk = 0;
    for(uint32_t off = 0xA0; off <= 0xBC; off++) chk = (uint8_t)(chk - hp_cart_byte(off));
    out->complement_ok = hp_cart_byte(0xBD) == (uint8_t)(chk - 0x19u);
    for(uint32_t i = 0; i < 4; i++) out->code[i] = hp_cart_byte(0xAC + i);
    out->write_game = hp_cart_write_game(out->code);
    out->rom_size = hp_cart_rom_size();
    out->lib = HpLibNone;
    out->lib_off = 0;
    out->other_lib = false;
    hp_cart_scan_libs(out);
}
