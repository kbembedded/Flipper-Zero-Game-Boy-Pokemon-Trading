#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_text.h>

#include <string.h>

#define GEN3_NICKNAME_OFF 0x08
#define GEN3_OT_NAME_OFF  0x14
#define GEN3_SECURE_OFF   0x20
#define GEN3_SECURE_LEN   48
#define GEN3_SUBSTRUCT_LEN 12
#define GEN3_CHECKSUM_OFF 0x1C

typedef struct {
    uint8_t off;
    uint8_t bytes;
    uint8_t shift;
    uint8_t bits;
} Gen3FieldSpec;

#define F(off, bytes, shift, bits) {off, bytes, shift, bits}

/* One entry per Gen3Field, same order as the enum. Offsets follow the PKHeX .pk3 layout and
 * pokeemerald include/pokemon.h. */
static const Gen3FieldSpec gen3_fields[Gen3FieldCount] = {
    [Gen3FieldPid] = F(0x00, 4, 0, 32),
    [Gen3FieldOtId] = F(0x04, 4, 0, 32),
    [Gen3FieldLanguage] = F(0x12, 1, 0, 8),
    [Gen3FieldBadEgg] = F(0x13, 1, 0, 1),
    [Gen3FieldHasSpecies] = F(0x13, 1, 1, 1),
    [Gen3FieldEggFlag] = F(0x13, 1, 2, 1),
    [Gen3FieldMarkings] = F(0x1B, 1, 0, 8),
    [Gen3FieldChecksum] = F(0x1C, 2, 0, 16),
    [Gen3FieldSpecies] = F(0x20, 2, 0, 16),
    [Gen3FieldHeldItem] = F(0x22, 2, 0, 16),
    [Gen3FieldExp] = F(0x24, 4, 0, 32),
    [Gen3FieldPpBonuses] = F(0x28, 1, 0, 8),
    [Gen3FieldFriendship] = F(0x29, 1, 0, 8),
    [Gen3FieldMove1] = F(0x2C, 2, 0, 16),
    [Gen3FieldMove2] = F(0x2E, 2, 0, 16),
    [Gen3FieldMove3] = F(0x30, 2, 0, 16),
    [Gen3FieldMove4] = F(0x32, 2, 0, 16),
    [Gen3FieldPp1] = F(0x34, 1, 0, 8),
    [Gen3FieldPp2] = F(0x35, 1, 0, 8),
    [Gen3FieldPp3] = F(0x36, 1, 0, 8),
    [Gen3FieldPp4] = F(0x37, 1, 0, 8),
    [Gen3FieldEvHp] = F(0x38, 1, 0, 8),
    [Gen3FieldEvAtk] = F(0x39, 1, 0, 8),
    [Gen3FieldEvDef] = F(0x3A, 1, 0, 8),
    [Gen3FieldEvSpe] = F(0x3B, 1, 0, 8),
    [Gen3FieldEvSpa] = F(0x3C, 1, 0, 8),
    [Gen3FieldEvSpd] = F(0x3D, 1, 0, 8),
    [Gen3FieldPokerus] = F(0x44, 1, 0, 8),
    [Gen3FieldMetLocation] = F(0x45, 1, 0, 8),
    [Gen3FieldMetLevel] = F(0x46, 2, 0, 7),
    [Gen3FieldMetGame] = F(0x46, 2, 7, 4),
    [Gen3FieldBall] = F(0x46, 2, 11, 4),
    [Gen3FieldOtGender] = F(0x46, 2, 15, 1),
    [Gen3FieldIvHp] = F(0x48, 4, 0, 5),
    [Gen3FieldIvAtk] = F(0x48, 4, 5, 5),
    [Gen3FieldIvDef] = F(0x48, 4, 10, 5),
    [Gen3FieldIvSpe] = F(0x48, 4, 15, 5),
    [Gen3FieldIvSpa] = F(0x48, 4, 20, 5),
    [Gen3FieldIvSpd] = F(0x48, 4, 25, 5),
    [Gen3FieldIsEgg] = F(0x48, 4, 30, 1),
    [Gen3FieldAbilityBit] = F(0x48, 4, 31, 1),
    [Gen3FieldFateful] = F(0x4C, 4, 31, 1),
    [Gen3FieldStatus] = F(0x50, 4, 0, 32),
    [Gen3FieldLevel] = F(0x54, 1, 0, 8),
    [Gen3FieldMail] = F(0x55, 1, 0, 8),
    [Gen3FieldHp] = F(0x56, 2, 0, 16),
    [Gen3FieldMaxHp] = F(0x58, 2, 0, 16),
    [Gen3FieldAtk] = F(0x5A, 2, 0, 16),
    [Gen3FieldDef] = F(0x5C, 2, 0, 16),
    [Gen3FieldSpe] = F(0x5E, 2, 0, 16),
    [Gen3FieldSpa] = F(0x60, 2, 0, 16),
    [Gen3FieldSpd] = F(0x62, 2, 0, 16),
    [Gen3FieldFiller] = F(0x1E, 2, 0, 16),
    [Gen3FieldGrowthUnknown] = F(0x2A, 2, 0, 16),
    [Gen3FieldCoolness] = F(0x3E, 1, 0, 8),
    [Gen3FieldBeauty] = F(0x3F, 1, 0, 8),
    [Gen3FieldCuteness] = F(0x40, 1, 0, 8),
    [Gen3FieldSmartness] = F(0x41, 1, 0, 8),
    [Gen3FieldToughness] = F(0x42, 1, 0, 8),
    [Gen3FieldSheen] = F(0x43, 1, 0, 8),
    [Gen3FieldRibbons] = F(0x4C, 4, 0, 32),
};

#undef F

uint32_t gen3_pkm_get(const Gen3Pkm* p, Gen3Field f) {
    if((unsigned)f >= Gen3FieldCount) return 0;
    const Gen3FieldSpec* d = &gen3_fields[f];
    uint32_t v = 0;
    for(unsigned i = 0; i < d->bytes; i++) v |= (uint32_t)p->raw[d->off + i] << (8 * i);
    v >>= d->shift;
    if(d->bits < 32) v &= (1UL << d->bits) - 1;
    return v;
}

void gen3_pkm_set(Gen3Pkm* p, Gen3Field f, uint32_t value) {
    if((unsigned)f >= Gen3FieldCount) return;
    const Gen3FieldSpec* d = &gen3_fields[f];
    uint32_t mask = (d->bits < 32) ? ((1UL << d->bits) - 1) : 0xFFFFFFFFUL;
    uint32_t cur = 0;
    for(unsigned i = 0; i < d->bytes; i++) cur |= (uint32_t)p->raw[d->off + i] << (8 * i);
    cur = (cur & ~(mask << d->shift)) | ((value & mask) << d->shift);
    for(unsigned i = 0; i < d->bytes; i++) p->raw[d->off + i] = (uint8_t)(cur >> (8 * i));
}

size_t gen3_pkm_nickname(const Gen3Pkm* p, char* out, size_t out_size) {
    return gen3_text_decode(p->raw + GEN3_NICKNAME_OFF, GEN3_NICKNAME_LEN, out, out_size);
}

void gen3_pkm_set_nickname(Gen3Pkm* p, const char* ascii) {
    gen3_text_encode(ascii, p->raw + GEN3_NICKNAME_OFF, GEN3_NICKNAME_LEN, GEN3_NICKNAME_LEN);
}

size_t gen3_pkm_ot_name(const Gen3Pkm* p, char* out, size_t out_size) {
    return gen3_text_decode(p->raw + GEN3_OT_NAME_OFF, GEN3_OT_NAME_LEN, out, out_size);
}

void gen3_pkm_set_ot_name(Gen3Pkm* p, const char* ascii) {
    gen3_text_encode(ascii, p->raw + GEN3_OT_NAME_OFF, GEN3_OT_NAME_LEN, GEN3_OT_NAME_LEN);
}

void gen3_pkm_ot_name_raw(const Gen3Pkm* p, uint8_t out[GEN3_OT_NAME_LEN]) {
    memcpy(out, p->raw + GEN3_OT_NAME_OFF, GEN3_OT_NAME_LEN);
}

void gen3_pkm_set_ot_name_raw(Gen3Pkm* p, const uint8_t name[GEN3_OT_NAME_LEN]) {
    memcpy(p->raw + GEN3_OT_NAME_OFF, name, GEN3_OT_NAME_LEN);
}

static uint16_t gen3_sum_words(const uint8_t* secure) {
    uint16_t sum = 0;
    for(unsigned i = 0; i < GEN3_SECURE_LEN; i += 2)
        sum = (uint16_t)(sum + (secure[i] | (secure[i + 1] << 8)));
    return sum;
}

uint16_t gen3_pkm_checksum(const Gen3Pkm* p) {
    return gen3_sum_words(p->raw + GEN3_SECURE_OFF);
}

void gen3_pkm_fix_checksum(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldChecksum, gen3_pkm_checksum(p));
}

/* pos[pid % 24][t] is the game-form position of substruct t (0 Growth, 1 Attacks, 2 EVs,
 * 3 Misc). A fact from pokeemerald src/pokemon.c (GetSubstruct / SUBSTRUCT_CASE), cross-checked
 * against the Bulbapedia order table in tests/test_gen3_pkm.c. */
static const uint8_t gen3_substruct_pos[24][4] = {
    {0, 1, 2, 3}, {0, 1, 3, 2}, {0, 2, 1, 3}, {0, 3, 1, 2}, {0, 2, 3, 1}, {0, 3, 2, 1},
    {1, 0, 2, 3}, {1, 0, 3, 2}, {2, 0, 1, 3}, {3, 0, 1, 2}, {2, 0, 3, 1}, {3, 0, 2, 1},
    {1, 2, 0, 3}, {1, 3, 0, 2}, {2, 1, 0, 3}, {3, 1, 0, 2}, {2, 3, 0, 1}, {3, 2, 0, 1},
    {1, 2, 3, 0}, {1, 3, 2, 0}, {2, 1, 3, 0}, {3, 1, 2, 0}, {2, 3, 1, 0}, {3, 2, 1, 0},
};

static void gen3_xor_secure(uint8_t* secure, uint32_t key) {
    for(unsigned i = 0; i < GEN3_SECURE_LEN; i += 4) {
        for(unsigned b = 0; b < 4; b++) secure[i + b] ^= (uint8_t)(key >> (8 * b));
    }
}

void gen3_pkm_encrypt(const Gen3Pkm* p, uint8_t out[GEN3_PARTY_SIZE]) {
    memcpy(out, p->raw, GEN3_PARTY_SIZE);
    uint16_t sum = gen3_pkm_checksum(p);
    out[GEN3_CHECKSUM_OFF] = (uint8_t)sum;
    out[GEN3_CHECKSUM_OFF + 1] = (uint8_t)(sum >> 8);

    uint32_t pid = gen3_pkm_get(p, Gen3FieldPid);
    const uint8_t* pos = gen3_substruct_pos[pid % 24];
    for(unsigned t = 0; t < 4; t++) {
        memcpy(
            out + GEN3_SECURE_OFF + GEN3_SUBSTRUCT_LEN * pos[t],
            p->raw + GEN3_SECURE_OFF + GEN3_SUBSTRUCT_LEN * t,
            GEN3_SUBSTRUCT_LEN);
    }
    gen3_xor_secure(out + GEN3_SECURE_OFF, pid ^ gen3_pkm_get(p, Gen3FieldOtId));
}

bool gen3_pkm_decrypt(const uint8_t* in, size_t len, Gen3Pkm* out) {
    if(len != GEN3_BOX_SIZE && len != GEN3_PARTY_SIZE) return false;

    Gen3Pkm tmp;
    memset(&tmp, 0, sizeof(tmp));
    memcpy(tmp.raw, in, len);

    uint32_t pid = gen3_pkm_get(&tmp, Gen3FieldPid);
    uint8_t secure[GEN3_SECURE_LEN];
    memcpy(secure, in + GEN3_SECURE_OFF, GEN3_SECURE_LEN);
    gen3_xor_secure(secure, pid ^ gen3_pkm_get(&tmp, Gen3FieldOtId));

    const uint8_t* pos = gen3_substruct_pos[pid % 24];
    for(unsigned t = 0; t < 4; t++) {
        memcpy(
            tmp.raw + GEN3_SECURE_OFF + GEN3_SUBSTRUCT_LEN * t,
            secure + GEN3_SUBSTRUCT_LEN * pos[t],
            GEN3_SUBSTRUCT_LEN);
    }
    if(gen3_pkm_checksum(&tmp) != gen3_pkm_get(&tmp, Gen3FieldChecksum)) return false;

    *out = tmp;
    return true;
}
