#pragma once

#include <stdint.h>
#include <string.h>
#include <src/gen3/gen3_pkm.h>

/* Golden Pokemon derived once during planning from pokeemerald's struct layout and its
 * GetSubstruct/CalculateBoxMonChecksum/EncryptBoxMon algorithms (not built here).
 * PID 0x9E377A3F: Adamant, PID % 24 = 23 (game order M E A G), male Treecko, not shiny. */
static const uint8_t GOLDEN_PK3[100] = {/* decrypted, G A E M, party form */
    0x3F, 0x7A, 0x37, 0x9E, 0xCE, 0x56, 0x00, 0x00, 0xCE, 0xCC, 0xBF, 0xBF, 0xBD, 0xC5, 0xC9, 0xFF,
    0xFF, 0xFF, 0x02, 0x02, 0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0x00, 0x8D, 0x33, 0x00, 0x00,
    0x15, 0x01, 0x00, 0x00, 0x87, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x01, 0x00, 0x2B, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x23, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x05, 0x21, 0xDF, 0x77, 0xBE, 0x35, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x05, 0xFF, 0x14, 0x00, 0x14, 0x00, 0x0C, 0x00, 0x09, 0x00, 0x0D, 0x00,
    0x0A, 0x00, 0x0B, 0x00};

/* GOLDEN_GAME[100]: GOLDEN_PK3 with bytes 0x20..0x4F replaced by: */
static const uint8_t GOLDEN_GAME_SECURE[48] = {
    0xF1, 0xD3, 0x32, 0xBF, 0x2E, 0x5B, 0x89, 0xAB, 0xF1, 0x2C, 0x37, 0x9E, 0xF1, 0x2C, 0x37, 0x9E,
    0xF1, 0x2C, 0x37, 0x9E, 0xF1, 0x2C, 0x37, 0x9E, 0xF0, 0x2C, 0x1C, 0x9E, 0xF1, 0x2C, 0x37, 0x9E,
    0xD2, 0x32, 0x37, 0x9E, 0xE4, 0x2D, 0x37, 0x9E, 0x76, 0x2C, 0x37, 0x9E, 0xF1, 0x6A, 0x37, 0x9E};

/* Zeroed Pokemon with the golden fields set through gen3_pkm_set (checksum not fixed). */
static inline void golden_build(Gen3Pkm* p) {
    memset(p, 0, sizeof(*p));
    gen3_pkm_set(p, Gen3FieldPid, 0x9E377A3F);
    gen3_pkm_set(p, Gen3FieldOtId, 0x000056CE);
    gen3_pkm_set_nickname(p, "TREECKO");
    gen3_pkm_set_ot_name(p, "FLIPPER");
    gen3_pkm_set(p, Gen3FieldLanguage, GEN3_MON_LANGUAGE_ENGLISH);
    gen3_pkm_set(p, Gen3FieldHasSpecies, 1);
    gen3_pkm_set(p, Gen3FieldSpecies, 277);
    gen3_pkm_set(p, Gen3FieldExp, 135);
    gen3_pkm_set(p, Gen3FieldFriendship, 70);
    gen3_pkm_set(p, Gen3FieldMove1, 1);
    gen3_pkm_set(p, Gen3FieldMove2, 43);
    gen3_pkm_set(p, Gen3FieldPp1, 35);
    gen3_pkm_set(p, Gen3FieldPp2, 30);
    gen3_pkm_set(p, Gen3FieldMetLocation, GEN3_METLOC_FATEFUL);
    gen3_pkm_set(p, Gen3FieldMetLevel, 5);
    gen3_pkm_set(p, Gen3FieldMetGame, 2);
    gen3_pkm_set(p, Gen3FieldBall, 4);
    gen3_pkm_set(p, Gen3FieldOtGender, 0);
    gen3_pkm_set(p, Gen3FieldIvHp, 31);
    gen3_pkm_set(p, Gen3FieldIvAtk, 30);
    gen3_pkm_set(p, Gen3FieldIvDef, 29);
    gen3_pkm_set(p, Gen3FieldIvSpe, 28);
    gen3_pkm_set(p, Gen3FieldIvSpa, 27);
    gen3_pkm_set(p, Gen3FieldIvSpd, 26);
    gen3_pkm_set(p, Gen3FieldStatus, 0);
    gen3_pkm_set(p, Gen3FieldLevel, 5);
    gen3_pkm_set(p, Gen3FieldMail, GEN3_MAIL_NONE);
    gen3_pkm_set(p, Gen3FieldHp, 20);
    gen3_pkm_set(p, Gen3FieldMaxHp, 20);
    gen3_pkm_set(p, Gen3FieldAtk, 12);
    gen3_pkm_set(p, Gen3FieldDef, 9);
    gen3_pkm_set(p, Gen3FieldSpe, 13);
    gen3_pkm_set(p, Gen3FieldSpa, 10);
    gen3_pkm_set(p, Gen3FieldSpd, 11);
}

static inline void golden_game(uint8_t out[100]) {
    memcpy(out, GOLDEN_PK3, 100);
    memcpy(out + 0x20, GOLDEN_GAME_SECURE, 48);
}
