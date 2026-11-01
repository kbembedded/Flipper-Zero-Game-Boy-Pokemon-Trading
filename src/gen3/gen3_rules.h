#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "gen3_pkm.h"

/* Total exp for a level, growth is a Gen3Growth. Level is clamped to 100; 0 -> 0, 1 -> 1. */
uint32_t gen3_exp_for_level(uint8_t growth, uint8_t level);
/* Largest level L <= 100 with gen3_exp_for_level(growth, L) <= exp (0 for exp 0). */
uint8_t gen3_level_for_exp(uint8_t growth, uint32_t exp);

/* +1 raises, -1 lowers, 0 neutral. HP is never affected. */
int8_t gen3_nature_effect(uint8_t nature, Gen3Stat stat);
uint16_t gen3_stat_value(
    Gen3Stat stat,
    uint8_t base,
    uint8_t iv,
    uint8_t ev,
    uint8_t level,
    uint8_t nature);
/* Level comes from exp. Shedinja's max HP is 1. All zero for an unknown species. */
void gen3_calc_stats(const Gen3Pkm* p, uint16_t out[Gen3StatCount]);
/* Sets Level, MaxHp..Spd and Hp = MaxHp. Status and Mail are untouched. No-op for an
 * unknown species. */
void gen3_recalc_party(Gen3Pkm* p);

typedef enum {
    Gen3GenderMale,
    Gen3GenderFemale,
    Gen3GenderNone
} Gen3Gender;

Gen3Gender gen3_gender_of(uint8_t gender_ratio, uint32_t pid);
bool gen3_is_shiny(uint32_t ot_id, uint32_t pid);
#define GEN3_UNOWN_LETTER_COUNT 28 /* A..Z, '!', '?' */
/* 0..25 = A..Z, 26 = '!', 27 = '?'. */
uint8_t gen3_unown_letter(uint32_t pid);
/* The letter as shown: 'A'..'Z', '!', '?'; '?' for anything past 27 too. */
char gen3_unown_char(uint8_t letter);

typedef struct {
    uint8_t nature; /* pid % 25 */
    Gen3Gender gender;
    bool shiny;
    uint8_t ability_slot; /* the stored ability bit */
    uint8_t unown_letter;
} Gen3Traits;

/* Gen3FieldSpecies holds the internal id. Gender is None for an unknown species. */
void gen3_traits_of(const Gen3Pkm* p, Gen3Traits* out);

typedef struct {
    uint32_t state;
} Gen3Rng; /* xorshift32 (13, 17, 5) */

void gen3_rng_seed(Gen3Rng* rng, uint32_t seed); /* seed 0 -> 0x6D2B79F5 */
uint32_t gen3_rng_next(Gen3Rng* rng);

/* Whether a shiny PID for OT ot_id has this nature and Unown letter. A shiny PID's high half is
 * tid ^ sid ^ (low half) ^ k with k < 8, so PID bits 24-25 (the letter's bits 6-7) are PID bits
 * 8-9 (its bits 2-3) xor bits 8-9 of tid ^ sid. With the nature, some pairs then have no shiny
 * PID at all: for OT 64893/0, 40 of the 700, HARDY/P among them; for 22222/0, none. Exact: it
 * walks every shiny PID with matching letter bits (at most 4 x 8192), stopping at the first with
 * the nature. false for nature >= 25 or letter >= 28. */
bool gen3_unown_shiny_possible(uint32_t ot_id, uint8_t nature, uint8_t letter);

#define GEN3_PID_MAX_TRIES 1000000u
/* Finds a PID for species `national` (1..386) and OT ot_id whose traits equal want, after
 * normalising want: gender is ignored for ratios 0/254/255, ability_slot for one-ability
 * species, unown_letter unless Unown. false (and *pid untouched) for an unknown species, an
 * out-of-range want (nature >= 25; ability_slot > 1 on a two-ability species; unown_letter
 * >= 28 on Unown; gender not Male or Female on a gendered species), a shiny Unown want that
 * gen3_unown_shiny_possible rules out (these fail at once, with no iteration), or after
 * GEN3_PID_MAX_TRIES. Every other want has at least 100 of the 2^19 shiny PIDs, or far more
 * non-shiny ones, for any OT (checked exhaustively when this was written), so the tries never
 * run out in practice. */
bool gen3_pid_find(
    Gen3Rng* rng,
    uint16_t national,
    uint32_t ot_id,
    const Gen3Traits* want,
    uint32_t* pid);

/* GiveBoxMonInitialMoveset: walk the learnset entries with level <= `level` in order; skip a
 * move already in moves[]; else put it in the first empty slot; when all 4 are full, shift
 * slots 1..3 down and put it in slot 3. Returns the number of moves filled (packed from slot 0;
 * the rest are 0). 0 (all 0) for an unknown species or if the learnset can't be read from the
 * bulk file. */
size_t gen3_default_moves(uint16_t national, uint8_t level, uint16_t moves[4]);
