#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_rules.h>

/* Editor model. Every mutator leaves the Pokemon game-valid: the ability bit is 0 for a
 * one-ability species (a two-ability species keeps its stored slot; a new PID sets it to the
 * slot it was found for, pid & 1), the party stats match exp, IVs, EVs and nature
 * (gen3_recalc_party, so Hp = MaxHp), and the checksum is fixed.
 *
 * Species names and default movesets come from the bulk file (gen3_data.h). A mutator that needs
 * one and can't read it fails closed: it returns false and the Pokemon is unchanged.
 *
 * pristine (plan 3, D7): true from a successful gen3_edit_init until a mutator (gen3_edit_load
 * and gen3_edit_use_trainer_ot included) changes the Pokemon's bytes; nothing sets it again. A
 * call that refuses, fails or changes nothing keeps it, and gen3_edit_set_trainer never changes
 * it. */

#define GEN3_EDIT_DEFAULT_NATIONAL 1
#define GEN3_EDIT_DEFAULT_LEVEL    5
/* same OT as plan 1's GEN3_TEST_TRAINER_*; plan 3 uses the player's */
#define GEN3_EDIT_DEFAULT_OT_NAME  "FLIPPER"
#define GEN3_EDIT_DEFAULT_TID      22222
#define GEN3_EDIT_DEFAULT_SID      0
#define GEN3_EDIT_MET_GAME         2 /* Ruby */
#define GEN3_EDIT_BALL             4 /* POKE BALL */
#define GEN3_EDIT_BALL_MAX         12 /* balls are items 1..12, MASTER BALL..PREMIER BALL */
#define GEN3_EDIT_MAX_EV           85 /* 6 x 85 = 510, the game's MAX_TOTAL_EVS */

typedef enum { /* same order and values as EvIv in src/include/stat_nl.h */
    Gen3PresetRandIvZeroEv,
    Gen3PresetRandIvLevelEv,
    Gen3PresetRandIvMaxEv,
    Gen3PresetMaxIvZeroEv,
    Gen3PresetMaxIvLevelEv,
    Gen3PresetMaxIvMaxEv,
} Gen3Preset;
#define GEN3_PRESET_COUNT 6

/* The GBA player (D7), as an OT: the Pokemon obeys them and counts as theirs */
typedef struct {
    /* game encoding, as the GBA's LinkPlayer name[0..6] (Ruby's OT name bytes) */
    uint8_t name[GEN3_OT_NAME_LEN];
    uint16_t tid;
    uint16_t sid;
    uint8_t gender; /* 0 male, 1 female */
} Gen3Trainer;

typedef struct Gen3Editor {
    Gen3Pkm pkm;
    Gen3Preset preset;
    bool preset_applied; /* false after gen3_edit_load until a preset or species is chosen */
    Gen3Rng rng;
    bool pristine; /* still the initial default (see above) */
    bool has_trainer;
    Gen3Trainer trainer; /* the GBA player, once has_trainer */
} Gen3Editor;

/* What gen3_normalise changed. Party stats are always recalculated, and HasSpecies set, without
 * being reported. */
typedef struct {
    bool moves_defaulted; /* no move was left: the default moveset, full PP, PP Ups 0 */
    bool duplicates_removed; /* a repeated move was dropped with its PP and PP Ups */
    bool gaps_packed; /* the input had an empty move slot before a used one */
    bool exp_clamped; /* exp was outside the level 1..100 range */
    bool ability_fixed; /* ability bit 1 on a one-ability species, now 0 */
    bool mail_item_removed; /* a held Mail item (GEN3_ITEM_MAIL_FIRST..LAST), now no item */
    bool mail_cleared; /* the party mail index wasn't 0xFF, now 0xFF */
} Gen3NormaliseInfo;

/* Makes a validated Pokemon (gen3_pkm_check Ok: a real species, not an egg, move ids < 355,
 * item < 377) game-valid, for loads and imports: moves packed with duplicates dropped, exp
 * clamped to the level 1..100 range, the default moveset if no move is left, ability bit 0 on a
 * one-ability species, no mail (a held Mail item removed, the mail index 0xFF: no mail record
 * exists behind it), HasSpecies 1, party stats recalculated with the current HP kept but clamped
 * to MaxHp, checksum fixed. Every other byte is kept. info (may be NULL) is always reset, then
 * filled. false, with *pkm untouched and info reset, if the species maps to no national, or if
 * no move is left and the default moveset can't be read from the bulk file. */
bool gen3_normalise(Gen3Pkm* pkm, Gen3NormaliseInfo* info);

/* Bulbasaur L5, default OT, preset RandIvZeroEv; pristine, no trainer. false if the bulk file
 * can't be read; *ed is then not a usable editor. */
bool gen3_edit_init(Gen3Editor* ed, uint32_t seed);
/* Takes pkm if gen3_pkm_check accepts it (gen3_pk3.h: not a Bad Egg or an egg, a real species,
 * moves < 355, item < 377; the checksum isn't checked) and runs gen3_normalise on it;
 * preset_applied = false. false, with the editor unchanged, if the check refuses it or
 * gen3_normalise fails. */
bool gen3_edit_load(Gen3Editor* ed, const Gen3Pkm* pkm);
uint16_t gen3_edit_national(const Gen3Editor* ed);
/* D5, amended by plan 3 D7: a fresh Pokemon of that species, keeping level and preset. Its OT
 * name (raw bytes), TID, SID and OT gender are the trainer's once one is set, else the current
 * Pokemon's; its PID is the first non-shiny one for that OT ID. true and no change if it is the
 * current species. false (unchanged) for an invalid species or if its name or default moveset
 * can't be read from the bulk file. */
bool gen3_edit_set_species(Gen3Editor* ed, uint16_t national);
/* Clamped 1..100; exp = exp_for_level. Nothing changes if that is the current level (exp stays
 * inside it, and a loaded Pokemon keeps its current HP). */
void gen3_edit_set_level(Gen3Editor* ed, uint8_t level);
/* Applies IVs (re-rolls random ones) and EVs. No-op for a value >= GEN3_PRESET_COUNT. */
void gen3_edit_set_preset(Gen3Editor* ed, Gen3Preset preset);
/* "" or NULL -> species name (game encoding). false (unchanged) if that name can't be read from
 * the bulk file. true and nothing changes if the name equals gen3_pkm_nickname's, so confirming
 * the name shown keeps bytes it shows as '?'. */
bool gen3_edit_set_nickname(Gen3Editor* ed, const char* ascii);
/* NULL -> "". Nothing changes if the name equals gen3_pkm_ot_name's, as for the nickname. */
void gen3_edit_set_ot_name(Gen3Editor* ed, const char* ascii);

typedef enum {
    Gen3OtIdOk, /* applied, shininess kept */
    /* Applied, but no shiny PID with the Pokemon's other traits exists for the new OT (only a
     * shiny Unown's nature and letter can do this; gen3_unown_shiny_possible). The PID is kept:
     * it isn't shiny for the new OT, so the Pokemon is no longer shiny. */
    Gen3OtIdShinyLost,
} Gen3OtIdResult;
/* Keeps shininess (D6) by finding a new PID when needed, and the stored ability slot. The OT ID
 * is always applied. */
Gen3OtIdResult gen3_edit_set_ot_id(Gen3Editor* ed, uint16_t tid, uint16_t sid);
/* 0 (no item) or an id gen3_item_holdable accepts; anything else is refused (no-op). */
void gen3_edit_set_item(Gen3Editor* ed, uint16_t item);
/* A move: full base PP, that slot's PP Ups cleared. 0: clears the slot. Then the moves are packed
 * toward slot 0, carrying their PP and PP Ups. false (unchanged): slot >= 4, move >= 355, a
 * duplicate, or removing the last move. */
bool gen3_edit_set_move(Gen3Editor* ed, uint8_t slot, uint16_t move);
/* gen3_default_moves for the species and level, each with full base PP; PP Ups 0. false (the
 * current moves kept) if the moveset can't be read from the bulk file. */
bool gen3_edit_default_moves(Gen3Editor* ed);
/* 1..GEN3_EDIT_BALL_MAX; anything else is a no-op. */
void gen3_edit_set_ball(Gen3Editor* ed, uint8_t ball);
/* raw byte: strain << 4 | days */
void gen3_edit_set_pokerus(Gen3Editor* ed, uint8_t value);
/* The fateful-encounter bit (Gen3FieldFateful, ribbons bit 31), on any species. FireRed/LeafGreen
 * won't trade a Mew or Deoxys without it, and a .pk3 or a Ruby trade can bring one so; a species
 * change sets it for them. Like every mutator, it keeps pristine when no byte changes. */
void gen3_edit_set_fateful(Gen3Editor* ed, bool on);
/* New PID with these traits (normalised as gen3_pid_find does); changes only the PID, the ability
 * bit, party stats and the checksum. If the normalised want already equals gen3_traits_of, true
 * and nothing changes (the PID and stored ability slot are kept). false (unchanged) only if
 * gen3_pid_find fails: an out-of-range want, or a shiny Unown nature and letter that no shiny PID
 * has for this OT (gen3_unown_shiny_possible). Both are refused before any PID is tried. */
bool gen3_edit_set_traits(Gen3Editor* ed, const Gen3Traits* want);

/* Remember the GBA player (D7), any non-zero gender stored as 1. A pristine editor (still the
 * initial default) takes it as OT at once, as gen3_edit_use_trainer_ot does, and stays pristine;
 * an edited Pokemon is left alone. */
void gen3_edit_set_trainer(Gen3Editor* ed, const Gen3Trainer* t);
const Gen3Trainer* gen3_edit_trainer(const Gen3Editor* ed); /* NULL until set */
/* The OT name bytes, TID, SID and OT gender equal the trainer's. false without a trainer. */
bool gen3_edit_ot_is_trainer(const Gen3Editor* ed);

typedef enum {
    Gen3UseTrainerOk, /* OT name (raw), OT gender, TID and SID applied; shininess kept */
    /* applied, but no longer shiny (gen3_edit_set_ot_id gave Gen3OtIdShinyLost) */
    Gen3UseTrainerShinyLost,
    Gen3UseTrainerNoTrainer, /* no trainer set: nothing changed */
} Gen3UseTrainerResult;
/* The trainer's OT on the current Pokemon: OT name (raw), OT gender, then gen3_edit_set_ot_id(tid,
 * sid), so shininess is kept where it can be (plan 2 D6). Nothing changes if it is already the
 * trainer's. */
Gen3UseTrainerResult gen3_edit_use_trainer_ot(Gen3Editor* ed);

/* 0 "Clean"; days != 0 "Infected"; else "Cured". */
const char* gen3_pokerus_status(uint8_t value);
