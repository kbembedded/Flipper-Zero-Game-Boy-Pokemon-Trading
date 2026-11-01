#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_edit.h>

/* .pk3 files: the PKHeX layout, 80 bytes (box) or 100 bytes (party), decrypted. */

typedef enum {
    Gen3CheckOk,
    Gen3CheckBadSize,
    Gen3CheckBadChecksum,
    Gen3CheckBadEgg,
    Gen3CheckEgg,
    Gen3CheckBadSpecies,
    Gen3CheckBadMove,
    Gen3CheckBadItem,
    Gen3CheckFileError,
    Gen3CheckNoData, /* the Pokemon needs the bulk data file (gen3_data.h), which can't be read */
} Gen3Check;

/* "OK", "Not a .pk3 (size)", "Checksum mismatch", "Bad Egg", "Eggs not supported",
 * "Unknown species", "Unknown move", "Unknown item", "Can't read file",
 * "Gen III data unreadable" */
const char* gen3_check_text(Gen3Check c);

/* In order: BadEgg flag; IsEgg or EggFlag; species maps to no national; any move >= 355;
 * item >= 377. */
Gen3Check gen3_pkm_check(const Gen3Pkm* p);

/* What an import changed, for the UI. */
typedef struct {
    Gen3NormaliseInfo normalise; /* what gen3_normalise changed */
    bool mail_removed; /* a held Mail item was removed: item 0, mail index 0xFF */
} Gen3ImportInfo;

/* Order: size, checksum, gen3_pkm_check. Then gen3_normalise runs, which also removes a held
 * Mail item (ids 121..132; the file is not refused) and sets the mail index to 0xFF. len 80:
 * party fields derived (Status 0, Mail 0xFF, Hp = MaxHp, stats from gen3_recalc_party). len 100:
 * the status and current HP are kept, stats are recomputed, a mail index other than 0xFF is
 * cleared (normalise.mail_cleared). On Ok, HasSpecies is set to 1. *out is unchanged unless Ok.
 * NoData: the Pokemon has no move and the default moveset can't be read from the bulk file.
 * info may be NULL; it is always reset. */
Gen3Check gen3_pk3_import(const uint8_t* data, size_t len, Gen3Pkm* out, Gen3ImportInfo* info);

/* raw[0..79], checksum recomputed. */
void gen3_pk3_export(const Gen3Pkm* p, uint8_t out[GEN3_BOX_SIZE]);

#define GEN3_PK3_NAME_MAX 16
/* The nickname with only [A-Za-z0-9-] kept, max 16; "" -> "POKEMON". */
void gen3_pk3_default_name(const Gen3Pkm* p, char* out, size_t out_size);
/* 1..16 chars of [A-Za-z0-9_-]. */
bool gen3_pk3_name_valid(const char* name);
