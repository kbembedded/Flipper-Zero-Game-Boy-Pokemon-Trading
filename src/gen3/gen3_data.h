#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Gen III game data, indexed by National Dex number. The game's internal species ids appear only
 * in the Pokemon bytes and in gen3_internal_to_national_tab. Names keep the game's spelling,
 * mapped to ASCII: e-acute -> 'E', female '\200', male '\201'.
 *
 * Species, move and item names, the name-sorted move and holdable lists, and learnsets are read
 * rarely, so they live in files/gen3_bulk.bin on the SD card rather than in RAM. All come from
 * tools/gen3_tables.py. The bulk accessors read through the installed Gen3BulkRead source; call
 * them from the UI thread only, never from the link ISR. */

#define GEN3_NATIONAL_COUNT      386 /* National Dex 1..386 */
#define GEN3_INTERNAL_COUNT      412 /* game species ids 0..411 (412 is the egg, excluded) */
#define GEN3_MOVE_COUNT          355 /* move ids 0..354 */
#define GEN3_ITEM_COUNT          377 /* item ids 0..376 */
#define GEN3_ABILITY_COUNT       78
#define GEN3_NATURE_COUNT        25
#define GEN3_TYPE_COUNT          18
#define GEN3_LEARNSET_COUNT      3947 /* all species' learnset entries */
#define GEN3_LEARNSET_MAX        17 /* the most entries one species has */
#define GEN3_SPECIES_NAME_LEN    10
#define GEN3_MOVE_NAME_LEN       12 /* longest move name; buffers hold GEN3_MOVE_NAME_LEN + 1 */
#define GEN3_ITEM_NAME_LEN       14 /* longest item name; buffers hold GEN3_ITEM_NAME_LEN + 1 */
#define GEN3_HOLDABLE_COUNT      231 /* no key items, HMs, Enigma Berry or mail */
#define GEN3_ITEM_MAIL_FIRST     121 /* ITEM_ORANGE_MAIL */
#define GEN3_ITEM_MAIL_LAST      132 /* ITEM_RETRO_MAIL */
#define GEN3_SPRITE_MAX_NATIONAL 386 /* files/all_sprites.fxbm covers 1..386 */
#define GEN3_NATIONAL_MEW        151
#define GEN3_NATIONAL_UNOWN      201
#define GEN3_NATIONAL_SHEDINJA   292
#define GEN3_NATIONAL_DEOXYS     386
#define GEN3_GENDER_MALE_ONLY    0
#define GEN3_GENDER_FEMALE_ONLY  254
#define GEN3_GENDERLESS          255

typedef enum {
    Gen3GrowthMediumFast,
    Gen3GrowthErratic,
    Gen3GrowthFluctuating,
    Gen3GrowthMediumSlow,
    Gen3GrowthFast,
    Gen3GrowthSlow
} Gen3Growth; /* game values 0..5 */

typedef struct {
    uint8_t base[6]; /* HP, Atk, Def, Spe, SpA, SpD */
    uint8_t type[2]; /* game type ids; one-type species repeat it */
    /* 0 male only, 254 female only, 255 genderless, else female if (pid & 0xFF) < ratio */
    uint8_t gender_ratio;
    uint8_t growth; /* Gen3Growth */
    uint8_t ability[2]; /* ability[1] == 0: one ability */
    uint8_t friendship; /* base friendship */
    uint16_t internal; /* game species id */
} Gen3SpeciesInfo; /* 16 bytes */

const Gen3SpeciesInfo* gen3_species(uint16_t national); /* NULL unless 1..386 */
/* ability[1] != 0. false for an unknown species. */
bool gen3_species_two_abilities(uint16_t national);
/* Male only, female only or genderless (ratio 0, 254 or 255): the PID can't choose the gender.
 * true for an unknown species, which has no gender to choose either. */
bool gen3_species_gender_fixed(uint16_t national);
uint16_t gen3_national_from_internal(uint16_t internal); /* 0 = not a real species */
uint16_t gen3_internal_from_national(uint16_t national); /* 0 unless 1..386 */
uint8_t gen3_move_pp(uint16_t move); /* base PP; 0 if out of range */
/* No key items, HMs, Enigma Berry or mail. From flash: safe in the link ISR. */
bool gen3_item_holdable(uint16_t item);
const char* gen3_ability_name(uint8_t ability); /* "???" if out of range */
const char* gen3_nature_name(uint8_t nature); /* "???" if >= 25 */
const char* gen3_type_name(uint8_t type); /* "???" if >= 18 */

/* Bulk tables (files/gen3_bulk.bin). The source reads len bytes at offset into buf; false if it
 * can't, including past the end of the file. */
typedef bool (*Gen3BulkRead)(void* ctx, uint32_t offset, void* buf, uint32_t len);
/* Install the source. Validates magic, version, counts and CRC-32 of the payload. false = unusable,
 * and no source is installed. */
bool gen3_bulk_open(Gen3BulkRead read, void* ctx);
void gen3_bulk_close(void);
/* A source is installed. A read that fails after a good open (or reads data the open checks
 * would have refused) uninstalls it, as gen3_bulk_close does, so this turns false and the caller
 * can open the file again. */
bool gen3_bulk_ready(void);
/* Copies the uppercase ASCII name (female '\200', male '\201') + NUL. false (out = "") if not
 * ready, out of range (1..386) or unreadable. */
bool gen3_species_name_get(uint16_t national, char out[GEN3_SPECIES_NAME_LEN + 1]);
/* Fills up to max packed (level << 9) | move entries in level order; returns the count (0 if not
 * ready, out of range or unreadable). GEN3_LEARNSET_MAX entries always hold a whole learnset. */
size_t gen3_learnset_get(uint16_t national, uint16_t* out, size_t max);
/* Copies the move's name + NUL (move 0 is "-"). false (out = "") if not ready, move >=
 * GEN3_MOVE_COUNT (no read: the source stays open) or unreadable. */
bool gen3_move_name_get(uint16_t move, char out[GEN3_MOVE_NAME_LEN + 1]);
/* Copies the item's name + NUL; an id without a name (0, 52, ...) reads true as "???". false
 * (out = "") if not ready, item >= GEN3_ITEM_COUNT (no read) or unreadable. */
bool gen3_item_name_get(uint16_t item, char out[GEN3_ITEM_NAME_LEN + 1]);
/* i < GEN3_MOVE_COUNT - 1: move ids 1..354 sorted by name. 0 if i is out of range (no read),
 * not ready or unreadable. An id read outside 1..354 closes the source and gives 0. */
uint16_t gen3_move_by_name_get(size_t i);
/* i < GEN3_HOLDABLE_COUNT: holdable item ids sorted by name. 0 if i is out of range (no read),
 * not ready or unreadable. An id read that gen3_item_holdable rejects closes the source and
 * gives 0. */
uint16_t gen3_holdable_by_name_get(size_t i);

/* Generated arrays (gen3_data_gen.c). Use the functions above. */
extern const Gen3SpeciesInfo gen3_species_info_tab[GEN3_NATIONAL_COUNT + 1];
extern const uint16_t gen3_internal_to_national_tab[GEN3_INTERNAL_COUNT];
extern const uint8_t gen3_move_pp_tab[GEN3_MOVE_COUNT];
/* Generated (gen3_data_gen.c): bit (item & 7) of byte (item >> 3) is set for a holdable item. */
extern const uint8_t gen3_item_holdable_tab[(GEN3_ITEM_COUNT + 7) / 8];
extern const char gen3_ability_name_tab[GEN3_ABILITY_COUNT][13];
extern const char gen3_nature_name_tab[GEN3_NATURE_COUNT][8];
extern const char gen3_type_name_tab[GEN3_TYPE_COUNT][7];
