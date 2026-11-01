#include "hp_save.h"
#include "hp_flash.h"

/* A sector: 0xF80 data bytes, then the footer from 0xFF4 (pokeruby: src/save.c: struct SaveSector;
 * em: include/save.h: SECTOR_DATA_SIZE) */
#define HP_SAVE_DATA_LEN 0xF80u
#define HP_SAVE_FOOTER_OFF 0xFF4u
#define HP_SAVE_FOOTER_LEN 12u
#define HP_SAVE_ALL_IDS 0x3FFFu /* ids 0..13 */
#define HP_SAVE_CHUNK 64u /* bytes summed per hp_flash_read */

_Static_assert(HP_SAVE_DATA_LEN % HP_SAVE_CHUNK == 0, "whole chunks");

/* Keyed on the code's first three letters only, so every region of a game maps to its layout.
 * Only the English codes can ever be armed, though: the write checks (hp_core.c) require the
 * cart's write_game (hp_cart_write_game: AXVE, AXPE, BPRE, BPGE) before they look at the layout.
 * The Japanese games' layouts have not been checked against pret. */
HpSaveGame hp_save_game(const uint8_t code[4]) {
    if(code[0] == 'A' && code[1] == 'X' && (code[2] == 'V' || code[2] == 'P')) {
        return HpSaveGameRS;
    }
    if(code[0] == 'B' && code[1] == 'P' && (code[2] == 'R' || code[2] == 'G')) {
        return HpSaveGameFRLG;
    }
    return HpSaveGameUnknown;
}

/* multiboot-and-save.md §5.4: ids 0 and 4 differ between the games (SaveBlock2, and the last
 * chunk of SaveBlock1: pokeruby: src/save.c: sSaveBlockChunks; fr: src/save.c: sSaveSlotLayout);
 * 13 is PokemonStorage's last chunk in both; the rest are whole 0xF80-byte chunks. */
uint16_t hp_save_size(HpSaveGame game, uint16_t id) {
    if(id >= HP_SAVE_SLOT_SECTORS) return 0;
    switch(game) {
    case HpSaveGameRS:
        if(id == 0) return 0x890;
        if(id == 4) return 0xC40;
        break;
    case HpSaveGameFRLG:
        if(id == 0) return 0xF24;
        if(id == 4) return 0xEE8;
        break;
    case HpSaveGameUnknown:
        return 0;
    }
    return id == 13 ? 0x7D0 : HP_SAVE_DATA_LEN;
}

static uint32_t hp_save_le32(const uint8_t* b) {
    return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

/* pokeruby: src/save.c: CalculateChecksum: a 32-bit sum of little-endian words, folded */
static uint16_t hp_save_fold(uint32_t sum) {
    return (uint16_t)((sum >> 16) + sum);
}

/* Reads a sector's footer and its full-length checksum. Returns the checksum over the section's
 * size in game's layout (a multiple of 4), or 0 when that size isn't known: then the sector can't
 * be valid for the game, and the caller doesn't compare. */
static uint16_t hp_save_scan(uint8_t sector, HpSaveGame game, HpFooter* out) {
    uint8_t buf[HP_SAVE_CHUNK];
    hp_flash_read(sector, HP_SAVE_FOOTER_OFF, buf, HP_SAVE_FOOTER_LEN);
    out->id = (uint16_t)(buf[0] | buf[1] << 8);
    out->checksum = (uint16_t)(buf[2] | buf[3] << 8);
    out->signature = hp_save_le32(buf + 4);
    out->counter = hp_save_le32(buf + 8);
    const uint32_t len = hp_save_size(game, out->id);
    uint16_t part = 0;
    uint32_t sum = 0;
    for(uint32_t off = 0; off < HP_SAVE_DATA_LEN; off += HP_SAVE_CHUNK) {
        hp_flash_read(sector, (uint16_t)off, buf, HP_SAVE_CHUNK);
        for(uint32_t i = 0; i < HP_SAVE_CHUNK; i += 4) {
            if(off + i == len) part = hp_save_fold(sum);
            sum += hp_save_le32(buf + i);
        }
    }
    out->full_checksum = hp_save_fold(sum);
    return len == HP_SAVE_DATA_LEN ? out->full_checksum : part;
}

void hp_save_footer(uint8_t sector, HpFooter* out) {
    hp_save_scan(sector, HpSaveGameUnknown, out);
}

bool hp_save_slot_ok(uint8_t slot, uint32_t* counter) {
    if(slot > 1) return false;
    uint32_t seen = 0;
    uint32_t ctr = 0;
    for(uint32_t i = 0; i < HP_SAVE_SLOT_SECTORS; i++) {
        HpFooter f;
        hp_save_footer((uint8_t)(slot * HP_SAVE_SLOT_SECTORS + i), &f);
        if(f.signature != HP_SAVE_SIGNATURE || f.checksum != f.full_checksum) return false;
        if(f.id >= HP_SAVE_SLOT_SECTORS || (seen & (1u << f.id))) return false;
        if(i > 0 && f.counter != ctr) return false;
        seen |= 1u << f.id;
        ctr = f.counter;
    }
    *counter = ctr;
    return true;
}

/* One slot as pokeruby: src/save.c: GetSaveValidStatus sees it: OK when the sectors with a valid
 * signature and a checksum over their section's size cover ids 0..13, with the counter of the last
 * of them in physical order. The game reads the size of an id over 13 from past its table; such a
 * sector, valid or not, leaves 13 sectors for 14 ids, so its slot is never OK: one with a valid
 * signature sets a bit outside 0..13. */
static bool hp_save_slot_loads(HpSaveGame game, uint8_t slot, uint32_t* counter) {
    uint32_t valid = 0;
    uint32_t ctr = 0;
    for(uint32_t i = 0; i < HP_SAVE_SLOT_SECTORS; i++) {
        HpFooter f;
        const uint16_t sum = hp_save_scan((uint8_t)(slot * HP_SAVE_SLOT_SECTORS + i), game, &f);
        if(f.signature != HP_SAVE_SIGNATURE) continue;
        if(f.id >= HP_SAVE_SLOT_SECTORS) {
            valid |= 1u << HP_SAVE_SLOT_SECTORS;
            continue;
        }
        if(f.checksum != sum) continue;
        ctr = f.counter;
        valid |= 1u << f.id;
    }
    *counter = ctr;
    return valid == HP_SAVE_ALL_IDS;
}

int hp_save_active_slot(HpSaveGame game) {
    if(game == HpSaveGameUnknown) return HP_SAVE_NO_LAYOUT;
    uint32_t a = 0;
    uint32_t b = 0;
    const bool a_ok = hp_save_slot_loads(game, 0, &a);
    const bool b_ok = hp_save_slot_loads(game, 1, &b);
    uint32_t ctr;
    if(a_ok && b_ok) {
        /* GetSaveValidStatus: the newer counter; 0 is newer than 0xFFFFFFFF */
        if((a == 0xFFFFFFFFu && b == 0) || (a == 0 && b == 0xFFFFFFFFu)) {
            ctr = (a + 1u) < (b + 1u) ? b : a;
        } else {
            ctr = a < b ? b : a;
        }
    } else if(a_ok) {
        ctr = a;
    } else if(b_ok) {
        ctr = b;
    } else {
        return -1;
    }
    /* pokeruby: src/save.c: sub_81258BC (em: CopySaveSlotData) loads slot gSaveCounter % 2 */
    return (int)(ctr % 2u);
}
