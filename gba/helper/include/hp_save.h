#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The Gen III save's two slots, as the helper checks them before it arms a write
 * (multiboot-and-save.md §5.1-§5.5). Slot 0 is sectors 0..13, slot 1 sectors 14..27.
 *
 * Two views of a sector's checksum (multiboot-and-save.md §5.3):
 * - the game's: over its section's size (§5.4), which depends on the game. This is how the game
 *   picks the slot it loads, so hp_save_active_slot uses it;
 * - the full length: over the whole 0xF80-byte data area. It equals the game's when the bytes past
 *   the section are zero, as the game writes them (pokeruby: src/save.c: WriteSingleChunk clears
 *   the sector first). hp_save_slot_ok uses it, so a slot it accepts is exactly what the game
 *   would have written.
 *
 * Every function that reads reads the save chip through hp_flash_read: call them only once the
 * probe has said HpChipFlash128. */

#define HP_SAVE_SLOT_SECTORS 14u
#define HP_SAVE_SIGNATURE 0x08012025u
/* hp_save_active_slot for a game whose save layout the helper doesn't know */
#define HP_SAVE_NO_LAYOUT (-2)

/* A save layout (multiboot-and-save.md §5.4). Only the two the helper writes. */
typedef enum {
    HpSaveGameUnknown,
    HpSaveGameRS, /* Ruby and Sapphire */
    HpSaveGameFRLG, /* FireRed and LeafGreen */
} HpSaveGame;

/* The layout of a cart header's game code (0xAC..0xAF), from its first three letters: AXV and AXP
 * are Ruby and Sapphire, BPR and BPG FireRed and LeafGreen. Every other code, Emerald's included,
 * is HpSaveGameUnknown. */
HpSaveGame hp_save_game(const uint8_t code[4]);
/* Section id's size in bytes for that layout; 0 for an unknown layout or an id over 13. */
uint16_t hp_save_size(HpSaveGame game, uint16_t id);

/* A sector's footer (multiboot-and-save.md §5.2), and its full-length checksum */
typedef struct {
    uint16_t id; /* 0xFF4 */
    uint16_t checksum; /* 0xFF6 */
    uint32_t signature; /* 0xFF8 */
    uint32_t counter; /* 0xFFC */
    uint16_t full_checksum; /* multiboot-and-save.md §5.3 over all 0xF80 data bytes */
} HpFooter;

/* Sector 0..31 */
void hp_save_footer(uint8_t sector, HpFooter* out);
/* Slot s (0/1) holds 14 sectors with signature 0x08012025, full-length checksum ok (§5.3 over
 * 0xF80 bytes), ids 0..13 once each, one counter. Writes it to *counter. */
bool hp_save_slot_ok(uint8_t slot, uint32_t* counter);
/* multiboot-and-save.md §5.5 over both slots with the game's own section sizes, exactly as the game
 * picks the slot it loads: 0/1 = that slot, -1 = none valid, HP_SAVE_NO_LAYOUT for
 * HpSaveGameUnknown. As the game does, the slot is the parity of the winning counter, wherever
 * the counter was found. */
int hp_save_active_slot(HpSaveGame game);
