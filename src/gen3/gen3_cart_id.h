#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_partner.h>
#include <src/gen3/gen3_save.h>

/* Which cart is in the GBA, and what the Flipper may do with its save (multiboot-and-save.md
 * §3.2, §3.3, §3.6), from the helper's CART_INFO and SAVE_PROBE; and a new Pokemon for that save.
 * Pure C, no heap. The helper checks every write rule again on its own. */

/* The two 128 KB flash chips the games accept (pokeruby: src/libs/agb_flash_1m.c: sSetupInfos),
 * device << 8 | maker: Macronix MX29L010 and Sanyo LE26FV10N1TS */
#define GEN3_FLASH_ID_MX 0x09C2u
#define GEN3_FLASH_ID_SANYO 0x1362u

/* Why a cart's save may not be written, checked in this order; the first that fails is the
 * reason. None: it may. */
typedef enum {
    Gen3CartRefuseNone,
    Gen3CartRefuseHeader, /* logo, fixed byte or complement wrong */
    Gen3CartRefuseNotPokemon, /* not AXV, AXP, BPR, BPG or BPE */
    Gen3CartRefuseJapanese, /* region J: backup only */
    Gen3CartRefuseSram, /* the probe's write landed */
    /* not a 128 KB flash, or not one of the two chips above (German Sapphire version 0's own
     * 0xD4BF passes here, and its region refuses it) */
    Gen3CartRefuseChip,
    Gen3CartRefuseLibrary, /* the ROM links no FLASH1M_V, or another save library too */
    Gen3CartRefuseRegion, /* not region E */
    Gen3CartRefuseEmerald, /* BPE */
    Gen3CartRefuseSave, /* set by the job layer: the save isn't one it can write */
} Gen3CartRefuse;

typedef struct {
    char code[5]; /* the game code at 0xAC: A-Z and 0-9 kept, any other byte '_' */
    char title[13]; /* the title at 0xA0, up to its first NUL: printable ASCII kept, else '?' */
    uint8_t version; /* 0xBC, the revision */
    Gen3LinkGame game; /* from the code's first three letters; Unknown for any other cart */
    /* The game's version id, for MetGame: 1 Sapphire, 2 Ruby, 3 Emerald, 4 FireRed, 5 LeafGreen
     * (the low byte of GEN3_VERSION_*); 0 for any other cart */
    uint8_t met_game;
    uint16_t flash_id; /* SAVE_PROBE's */
    /* Header ok, one of the five games, and a 128 KB flash that isn't SRAM: the helper reads its
     * save */
    bool can_read_save;
    bool can_browse; /* can_read_save, and not region J (whose layout isn't known) */
    /* can_browse, one of the two chips, FLASH1M_V and no other library, region E, not Emerald */
    bool can_write;
    Gen3CartRefuse why;
} Gen3CartId;

void gen3_cart_identify(const Gen3HCartInfo* info, const Gen3HProbe* probe, Gen3CartId* out);
/* One line of at most 21 characters; "" for a value outside the enum */
const char* gen3_cart_refuse_text(Gen3CartRefuse r);

/* The save's trainer as an editor OT: name_raw[0..6], tid, sid, gender (non-zero -> 1). */
void gen3_cart_trainer_ot(const Gen3SaveTrainer* t, Gen3Trainer* out);
/* A new Pokemon for this save: gen3_edit_init(ed, seed) (false: the bulk file can't be read, and
 * *ed is not usable), then gen3_edit_set_trainer(ed, the save's trainer), which a pristine editor
 * takes as OT at once, keeping shininess, then MetGame = met_game, checksum fixed. */
bool gen3_cart_new_mon(Gen3Editor* ed, const Gen3SaveTrainer* t, uint8_t met_game, uint32_t seed);
