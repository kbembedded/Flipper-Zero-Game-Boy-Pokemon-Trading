#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <src/gen3/gen3_partner.h>
#include <src/gen3/gen3_pkm.h>

/* The Gen III save on its 128 KB flash (multiboot-and-save.md §5): sector footers and checksums,
 * the slot the game loads, the plan of a safe write, and where the trainer, the party and the
 * boxes sit. Pure C, no heap, no file-scope state. Multi-byte fields are read and written byte by
 * byte, little-endian.
 *
 * The layout is the game's, and the game comes from the cart header (gen3_cart_id.h), never from
 * the save (multiboot-and-save.md §5.6). Gen3LinkGameUnknown has no layout: no section sizes, so
 * no valid sector, and no spans. */

#define GEN3_SAVE_SECTOR_SIZE 4096u
#define GEN3_SAVE_SECTOR_DATA 0xF80u /* a sector's data area; its footer is at 0xFF4 */
#define GEN3_SAVE_SECTORS 32u
#define GEN3_SAVE_SLOT_SECTORS 14u /* slot 0 is sectors 0..13, slot 1 14..27 */
#define GEN3_SAVE_SIZE 131072u
#define GEN3_SAVE_SIGNATURE 0x08012025u
#define GEN3_SAVE_BOXES 14u
#define GEN3_SAVE_BOX_SLOTS 30u
#define GEN3_SAVE_PARTY_MAX 6u
#define GEN3_SAVE_PARTY 0xFFu /* the "box" value for the party */
#define GEN3_SAVE_NO_SECTOR 0xFFu /* Gen3SaveSlots.phys, Gen3SaveWritePlan.src: none */

/* Section id's used length in bytes (multiboot-and-save.md §5.4): RS, FRLG and E differ in ids 0
 * and 4. 0 for Unknown or an id over 13. */
uint16_t gen3_save_section_size(Gen3LinkGame g, uint16_t id);
/* The game's checksum (multiboot-and-save.md §5.3; pokeruby: src/save.c: CalculateChecksum): the
 * 32-bit sum of the first size / 4 little-endian words, folded to 16 bits as high + low. */
uint16_t gen3_save_checksum(const uint8_t* data, uint16_t size);

/* A sector's footer at 0xFF4 (multiboot-and-save.md §5.2) */
typedef struct {
    uint16_t id, checksum;
    uint32_t signature, counter;
} Gen3SaveFooter;
void gen3_save_footer_get(const uint8_t sector[GEN3_SAVE_SECTOR_SIZE], Gen3SaveFooter* out);
void gen3_save_footer_set(uint8_t sector[GEN3_SAVE_SECTOR_SIZE], const Gen3SaveFooter* f);

typedef struct {
    Gen3SaveFooter f;
    /* As the game checks a sector: signature 0x08012025, id 0..13, and the checksum over the
     * section's size for the game */
    bool valid;
    /* The checksum over all 0xF80 data bytes equals the stored one, whatever the id or signature.
     * The helper checks the slot it keeps this way. True for every sector a game writes, since the
     * game clears the bytes past its section (pokeruby: src/save.c: WriteSingleChunk). */
    bool full_ok;
} Gen3SaveSectorInfo;
void gen3_save_sector_info(
    Gen3LinkGame g,
    const uint8_t sector[GEN3_SAVE_SECTOR_SIZE],
    Gen3SaveSectorInfo* out);

typedef enum {
    Gen3SlotEmpty, /* no sector with a valid signature */
    Gen3SlotOk, /* its valid sectors cover ids 0..13 */
    Gen3SlotError,
} Gen3SlotStatus;

/* What the game makes of the save (pokeruby: src/save.c: GetSaveValidStatus). Error: one slot is
 * Ok, the other damaged ("The save file is corrupt. The previous save file will be loaded.").
 * Corrupt: no Ok slot, and not both Empty (the game's 2, "The save file has been deleted"). */
typedef enum {
    Gen3SaveOk,
    Gen3SaveError,
    Gen3SaveEmpty,
    Gen3SaveCorrupt,
} Gen3SaveResult;

typedef struct {
    Gen3SlotStatus slot[2]; /* slot 0 = sectors 0..13, slot 1 = 14..27 */
    uint32_t counter[2]; /* each slot's last valid sector's counter, in physical order; 0: none */
    Gen3SaveResult result;
    uint32_t save_counter; /* the newer Ok slot's counter (0 is newer than 0xFFFFFFFF); 0 if none */
    /* The slot the game loads: save_counter % 2, the counter's parity, not where it was found
     * (pokeruby: src/save.c: sub_81258BC). 0 when Empty or Corrupt. */
    uint8_t active;
    /* Stricter than the game, before a write (multiboot-and-save.md §5.5): the active slot's 14
     * sectors are all valid, hold ids 0..13 once each, and all carry save_counter. */
    bool strict_ok;
    /* A sector the game loads (phys[k]) fails the full-length checksum: a byte past its section
     * isn't zero, as no game writes it. The helper refuses to keep such a slot, so the cart isn't
     * writable until the game saves once. */
    bool padded;
    /* As the game sets it for a save it loads: the physical index 0..13 of the active slot's last
     * sector, in physical order, whose footer says id 0, valid or not; 0 when there is none. 0
     * here when Empty or Corrupt, where the game, with its counter 0, still reads slot A and
     * takes slot A's last id-0 footer the same way; nothing plans a write from such a save. */
    uint8_t first;
    /* id -> sector 0..27 the game loads it from: the active slot's last valid sector of that id in
     * physical order. GEN3_SAVE_NO_SECTOR when it has none (every id when Empty or Corrupt). */
    uint8_t phys[GEN3_SAVE_SLOT_SECTORS];
} Gen3SaveSlots;
/* multiboot-and-save.md §5.5 exactly, over sectors 0..27 as gen3_save_sector_info describes them
 * for the cart's game, plus strict_ok and padded */
void gen3_save_analyze(const Gen3SaveSectorInfo info[28], Gen3SaveSlots* out);

/* A new save for the other slot (multiboot-and-save.md §5.8): id k goes to sector dst[k], with
 * counter `counter`; src[k] is the sector it comes from now. */
typedef struct {
    uint8_t target; /* counter % 2 */
    uint32_t counter;
    uint8_t first; /* the physical index of id 0 in the target slot */
    uint8_t dst[GEN3_SAVE_SLOT_SECTORS]; /* 14 * target + (k + first) % 14 */
    uint8_t src[GEN3_SAVE_SLOT_SECTORS];
} Gen3SaveWritePlan;
/* multiboot-and-save.md §5.8 step 2: counter save_counter + 1 (it wraps), first + 1, so the target
 * is the slot the game doesn't load, and src = phys. false, *out untouched, unless strict_ok. */
bool gen3_save_plan_write(const Gen3SaveSlots* s, Gen3SaveWritePlan* out);
/* A restore onto a cart with no valid save: the backup's own counter and first (taken % 14),
 * target counter % 2. src is all GEN3_SAVE_NO_SECTOR. */
void gen3_save_plan_fresh(uint32_t counter, uint8_t first, Gen3SaveWritePlan* out);

/* len bytes at offset off of section id */
typedef struct {
    uint8_t id;
    uint16_t off;
    uint8_t len;
} Gen3SaveSpan;
/* Where a Pokemon sits (multiboot-and-save.md §5.6): box 0..13 slot 0..29 is 80 bytes in ids
 * 5..13, in two spans for the eight that straddle two sections; box GEN3_SAVE_PARTY slot 0..5 is
 * 100 bytes in id 1. Returns the number of spans, 1 or 2; 0 for an invalid box or slot, or
 * Unknown. */
uint8_t gen3_save_mon_spans(Gen3LinkGame g, uint8_t box, uint8_t slot, Gen3SaveSpan out[2]);
/* The party count byte: RS and E {1, 0x234, 1}, FRLG {1, 0x034, 1}; len 0 for Unknown */
Gen3SaveSpan gen3_save_party_count_span(Gen3LinkGame g);
/* A box's name, 8 characters and a terminator: {13, 0x744 + 9 * box, 9}; len 0 for a box over
 * 13. The same in every game. */
Gen3SaveSpan gen3_save_box_name_span(uint8_t box);

/* The trainer, from section 0 (SaveBlock2), the same in every game (multiboot-and-save.md §5.6) */
typedef struct {
    char name[8]; /* decoded (gen3_text_decode) */
    uint8_t name_raw[8]; /* the 8 bytes at 0x00, as stored */
    uint8_t gender; /* 0 male, 1 female, as stored */
    uint16_t tid, sid;
    uint16_t hours;
    uint8_t minutes;
} Gen3SaveTrainer;
void gen3_save_trainer(const uint8_t sec0[GEN3_SAVE_SECTOR_SIZE], Gen3SaveTrainer* out);

typedef enum {
    Gen3MonEmpty,
    Gen3MonOk,
    Gen3MonEgg,
    Gen3MonBad, /* the game shows it as a Bad Egg, or it holds no species the game has */
} Gen3MonKind;
typedef struct {
    Gen3MonKind kind;
    uint16_t national; /* 0 unless Ok or Egg */
    uint8_t level; /* from exp and the species' growth; 0 unless Ok or Egg */
    char nickname[11]; /* "" unless Ok or Egg */
} Gen3MonSummary;
/* mon is len 80 (box) or 100 (party) bytes, game form. In order, as the game reads a slot: any
 * other len, a checksum that fails, or the Bad Egg flag -> Bad; species 0 -> Empty (all bytes 0
 * decrypt to it); a species the game doesn't have -> Bad; IsEgg or the egg flag -> Egg; else Ok.
 * Flash tables only: no bulk file read. */
void gen3_save_mon_summary(const uint8_t* mon, size_t len, Gen3MonSummary* out);
/* Decrypts mon (len 80 or 100, game form) into *out. For len 80, the party fields are derived:
 * Status 0, Mail 0xFF, and gen3_recalc_party. false, *out untouched, for any other len or a
 * checksum that fails. */
bool gen3_save_mon_to_pkm(const uint8_t* mon, size_t len, Gen3Pkm* out);
/* Game form (gen3_pkm_encrypt: the checksum recomputed), its first len bytes: 80 for a box, 100
 * for the party. A len over 100 writes 100. */
void gen3_save_pkm_to_mon(const Gen3Pkm* p, size_t len, uint8_t* out);
