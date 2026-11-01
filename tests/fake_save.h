#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Test-only: synthetic 128 KB Gen III flash images (multiboot-and-save.md §5.1-§5.4, §5.6), as the
 * games write them. Written from the doc alone: its section sizes, checksum, offsets and text
 * encoding are its own, and it shares nothing with the Flipper's save code.
 *
 * A present slot s holds ids 0..13, id k at physical sector 14 * s + (k + first[s]) % 14. Each
 * sector: data bytes [0, size[id]) from an LCG on the seed, zero up to 0xFF4 (the game clears the
 * whole sector before it fills it: pokeruby: src/save.c: WriteSingleChunk), then the footer: id at
 * 0xFF4, the checksum over size[id] bytes at 0xFF6, the signature 0x08012025 at 0xFF8 and the
 * counter at 0xFFC, all little-endian. With no_mons, the party (its count and 6 Pokemon, in id 1)
 * and the boxes (the current box and all 420 Pokemon, ids 5..13) are zero: no Pokemon anywhere;
 * box names and the rest keep the LCG's bytes. Without it, every data byte is the LCG's. */

#define FAKE_SAVE_LEN 131072u
#define FAKE_SAVE_SECTOR_LEN 4096u
#define FAKE_SAVE_SLOT_SECTORS 14u

typedef enum {
    FakeGameRS,
    FakeGameFRLG,
    FakeGameE,
} FakeGame;

typedef struct {
    FakeGame game;
    bool slot_present[2];
    uint32_t counter[2];
    uint8_t first[2]; /* physical index of id 0 */
    uint32_t seed; /* data bytes within size[id] from an LCG; past size[id] zero */
    bool no_mons; /* the party and the boxes zero, as a save with no Pokemon */
} FakeSaveSpec;

/* Absent slots and sectors 28..31 all 0xFF */
void fake_save_build(uint8_t img[131072], const FakeSaveSpec* spec);
/* Sector number 0..27 */
uint32_t fake_save_phys(const FakeSaveSpec* spec, uint8_t slot, uint8_t id);
/* The section's used length: multiboot-and-save.md §5.4 */
uint16_t fake_save_size(FakeGame game, uint8_t id);
/* Recomputes that sector's checksum over size[id] bytes, id from its footer (over all 0xF80 data
 * bytes for an id over 13). */
void fake_save_fix(uint8_t img[131072], uint8_t sector, FakeGame game);
void fake_save_set_counter(uint8_t img[131072], uint8_t sector, uint32_t counter);
/* The id only: the checksum is left as it is. */
void fake_save_set_id(uint8_t img[131072], uint8_t sector, uint16_t id);
/* Makes the stored checksum wrong. */
void fake_save_break_checksum(uint8_t img[131072], uint8_t sector);

/* Each of these writes into the sectors of a slot the spec built as the game lays them out
 * (multiboot-and-save.md §5.6), then refixes the checksum of every sector it touched. Names are
 * 'A'-'Z', 'a'-'z', '0'-'9' and ' ' in the game's encoding; anything else, or a slot not built,
 * aborts. */
/* SaveBlock2 (id 0): the name at 0x00 (7 characters at most, then 0xFF up to 8 bytes), the gender
 * at 0x08, TID at 0x0A and SID at 0x0C. */
void fake_save_put_trainer(
    uint8_t img[131072],
    const FakeSaveSpec* s,
    uint8_t slot,
    const char* name,
    uint8_t gender,
    uint16_t tid,
    uint16_t sid);
/* SaveBlock1 (id 1): the count, then mons[0..count-1] (game form) in party slots 0.., and the
 * slots after them zeroed. count 0..6. */
void fake_save_put_party(
    uint8_t img[131072],
    const FakeSaveSpec* s,
    uint8_t slot,
    uint8_t count,
    const uint8_t mons[][100]);
/* PokemonStorage's box 0..13, position 0..29: 80 bytes from storage offset 4 + 80 * (box * 30 +
 * pos), across two sections for the eight that straddle. */
void fake_save_put_box_mon(
    uint8_t img[131072],
    const FakeSaveSpec* s,
    uint8_t slot,
    uint8_t box,
    uint8_t pos,
    const uint8_t mon[80]);
/* PokemonStorage's name of box 0..13: 8 characters at most, 0xFF, then zeros up to 9 bytes. */
void fake_save_put_box_name(
    uint8_t img[131072],
    const FakeSaveSpec* s,
    uint8_t slot,
    uint8_t box,
    const char* name);
