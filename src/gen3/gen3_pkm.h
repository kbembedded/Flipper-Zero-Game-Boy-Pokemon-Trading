#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GEN3_BOX_SIZE               80
#define GEN3_PARTY_SIZE             100
#define GEN3_NICKNAME_LEN           10
#define GEN3_OT_NAME_LEN            7
#define GEN3_MON_LANGUAGE_ENGLISH   2 /* same value as GEN3_LANGUAGE_ENGLISH in gen3_link_player.h */
#define GEN3_MAIL_NONE              0xFF
#define GEN3_METLOC_FATEFUL         0xFF

typedef enum {
    Gen3StatHp,
    Gen3StatAtk,
    Gen3StatDef,
    Gen3StatSpe,
    Gen3StatSpa,
    Gen3StatSpd,
    Gen3StatCount
} Gen3Stat;

/* Decrypted Pokemon, party form, in the PKHeX .pk3 layout: substructs unshuffled at
 * 0x20 Growth, 0x2C Attacks, 0x38 EVs/Condition, 0x44 Misc; party fields at 0x50. */
typedef struct {
    uint8_t raw[GEN3_PARTY_SIZE];
} Gen3Pkm;

typedef enum {
    Gen3FieldPid,
    Gen3FieldOtId,
    Gen3FieldLanguage,
    Gen3FieldBadEgg,
    Gen3FieldHasSpecies,
    Gen3FieldEggFlag,
    Gen3FieldMarkings,
    Gen3FieldChecksum,
    Gen3FieldSpecies,
    Gen3FieldHeldItem,
    Gen3FieldExp,
    Gen3FieldPpBonuses,
    Gen3FieldFriendship,
    Gen3FieldMove1,
    Gen3FieldMove2,
    Gen3FieldMove3,
    Gen3FieldMove4,
    Gen3FieldPp1,
    Gen3FieldPp2,
    Gen3FieldPp3,
    Gen3FieldPp4,
    Gen3FieldEvHp,
    Gen3FieldEvAtk,
    Gen3FieldEvDef,
    Gen3FieldEvSpe,
    Gen3FieldEvSpa,
    Gen3FieldEvSpd,
    Gen3FieldPokerus,
    Gen3FieldMetLocation,
    Gen3FieldMetLevel,
    Gen3FieldMetGame,
    Gen3FieldBall,
    Gen3FieldOtGender,
    Gen3FieldIvHp,
    Gen3FieldIvAtk,
    Gen3FieldIvDef,
    Gen3FieldIvSpe,
    Gen3FieldIvSpa,
    Gen3FieldIvSpd,
    Gen3FieldIsEgg,
    Gen3FieldAbilityBit,
    Gen3FieldFateful,
    Gen3FieldStatus,
    Gen3FieldLevel,
    Gen3FieldMail,
    Gen3FieldHp,
    Gen3FieldMaxHp,
    Gen3FieldAtk,
    Gen3FieldDef,
    Gen3FieldSpe,
    Gen3FieldSpa,
    Gen3FieldSpd,
    /* Bytes the editor does not model, exposed so they can be poked without raw offsets. */
    Gen3FieldFiller, /* 0x1E u16 */
    Gen3FieldGrowthUnknown, /* 0x2A u16 */
    Gen3FieldCoolness, /* 0x3E..0x43: cool, beauty, cute, smart, tough, sheen */
    Gen3FieldBeauty,
    Gen3FieldCuteness,
    Gen3FieldSmartness,
    Gen3FieldToughness,
    Gen3FieldSheen,
    Gen3FieldRibbons, /* 0x4C u32 (ribbons/obedience, Fateful is bit 31) */
    Gen3FieldCount,
} Gen3Field; /* Gen3FieldEvHp + s, Gen3FieldIvHp + s, Gen3FieldMaxHp + s index by Gen3Stat s */

uint32_t gen3_pkm_get(const Gen3Pkm* p, Gen3Field f);
/* value is masked to the field width. */
void gen3_pkm_set(Gen3Pkm* p, Gen3Field f, uint32_t value);

size_t gen3_pkm_nickname(const Gen3Pkm* p, char* out, size_t out_size);
/* Encodes at most 10 chars, EOS-filled. */
void gen3_pkm_set_nickname(Gen3Pkm* p, const char* ascii);
size_t gen3_pkm_ot_name(const Gen3Pkm* p, char* out, size_t out_size);
/* Encodes at most 7 chars, EOS-filled. */
void gen3_pkm_set_ot_name(Gen3Pkm* p, const char* ascii);
/* The OT name's 7 bytes as stored, raw[0x14..0x1A] (game encoding, EOS 0xFF): unlike the ASCII
 * pair above, no byte is decoded, replaced or lost. */
void gen3_pkm_ot_name_raw(const Gen3Pkm* p, uint8_t out[GEN3_OT_NAME_LEN]);
void gen3_pkm_set_ot_name_raw(Gen3Pkm* p, const uint8_t name[GEN3_OT_NAME_LEN]);

/* u16 sum of the 24 little-endian words at 0x20..0x4F. */
uint16_t gen3_pkm_checksum(const Gen3Pkm* p);
void gen3_pkm_fix_checksum(Gen3Pkm* p);

/* Game form: header copied, checksum recomputed into out, the 4 substructs placed by PID % 24,
 * each LE u32 of 0x20..0x4F XORed with PID ^ OTID, party bytes copied. */
void gen3_pkm_encrypt(const Gen3Pkm* p, uint8_t out[GEN3_PARTY_SIZE]);
/* Inverse. len 80 (party bytes zeroed) or 100. False on any other len or a checksum mismatch,
 * and *out is unchanged then. */
bool gen3_pkm_decrypt(const uint8_t* in, size_t len, Gen3Pkm* out);
