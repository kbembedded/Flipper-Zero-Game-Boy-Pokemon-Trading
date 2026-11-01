#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_pkm.h>

/* Test Pokemon for the trade tests, made with the plan-2 editor. The editor reads the bulk file,
 * so main() calls bulk_file_open (bulk_file.h) before any of these; without it they abort. */

/* Decrypted: a fresh editor Pokemon of that species and level. */
static inline void tm_make_pkm(uint16_t national, uint8_t level, uint32_t seed, Gen3Pkm* out) {
    Gen3Editor ed;
    if(!gen3_edit_init(&ed, seed) || !gen3_edit_set_species(&ed, national)) {
        printf("tm_make: Gen III bulk data not open\n");
        abort();
    }
    gen3_edit_set_level(&ed, level);
    *out = ed.pkm;
}

/* Game form (encrypted), as a GBA party slot holds it. */
static inline void
    tm_make(uint16_t national, uint8_t level, uint32_t seed, uint8_t out[GEN3_PARTY_SIZE]) {
    Gen3Pkm p;
    tm_make_pkm(national, level, seed, &p);
    gen3_pkm_encrypt(&p, out);
}

/* tm_make(1, 5, 1) with IsEgg 1 and EggFlag 1, checksum fixed. */
static inline void tm_make_egg(uint8_t out[GEN3_PARTY_SIZE]) {
    Gen3Pkm p;
    tm_make_pkm(1, 5, 1, &p);
    gen3_pkm_set(&p, Gen3FieldIsEgg, 1);
    gen3_pkm_set(&p, Gen3FieldEggFlag, 1);
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, out);
}

/* tm_make(1, 5, 2) holding ORANGE MAIL (121) with mail index 0, checksum fixed. Poked raw:
 * gen3_edit_set_item refuses Mail. */
static inline void tm_make_mail(uint8_t out[GEN3_PARTY_SIZE]) {
    Gen3Pkm p;
    tm_make_pkm(1, 5, 2, &p);
    gen3_pkm_set(&p, Gen3FieldHeldItem, 121);
    gen3_pkm_set(&p, Gen3FieldMail, 0);
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, out);
}

/* tm_make(national, 5, seed) with the fateful-encounter bit 0, checksum fixed: a Mew or Deoxys
 * from a source that doesn't set it (a .pk3, or a trade from Ruby). */
static inline void
    tm_make_unfateful(uint16_t national, uint32_t seed, uint8_t out[GEN3_PARTY_SIZE]) {
    Gen3Pkm p;
    tm_make_pkm(national, 5, seed, &p);
    gen3_pkm_set(&p, Gen3FieldFateful, 0);
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, out);
}

/* tm_make(1, 5, 3) with one secure-area bit flipped: the checksum no longer matches. */
static inline void tm_make_bad_egg(uint8_t out[GEN3_PARTY_SIZE]) {
    tm_make(1, 5, 3, out);
    out[0x30] ^= 1;
}
