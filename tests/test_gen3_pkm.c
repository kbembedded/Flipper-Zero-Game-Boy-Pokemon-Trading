#include "test.h"
#include "gen3_golden.h"
#include <src/gen3/gen3_pkm.h>

TEST(test_field_layout) {
    Gen3Pkm p;
    memset(&p, 0, sizeof(p));
    gen3_pkm_set(&p, Gen3FieldPid, 0x11223344);
    const uint8_t pid[4] = {0x44, 0x33, 0x22, 0x11};
    ASSERT_MEM_EQ(p.raw, pid, 4);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldPid), 0x11223344);

    gen3_pkm_set(&p, Gen3FieldMetLevel, 5);
    gen3_pkm_set(&p, Gen3FieldMetGame, 2);
    gen3_pkm_set(&p, Gen3FieldBall, 4);
    gen3_pkm_set(&p, Gen3FieldOtGender, 1);
    ASSERT_EQ(p.raw[0x46], 0x05);
    ASSERT_EQ(p.raw[0x47], 0xA1);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldBall), 4);

    for(int s = 0; s < Gen3StatCount; s++) gen3_pkm_set(&p, Gen3FieldIvHp + s, 31 - s);
    gen3_pkm_set(&p, Gen3FieldAbilityBit, 1);
    const uint8_t iv[4] = {0xDF, 0x77, 0xBE, 0xB5};
    ASSERT_MEM_EQ(p.raw + 0x48, iv, 4);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldIvSpd), 26);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldAbilityBit), 1);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldIsEgg), 0);

    gen3_pkm_set(&p, Gen3FieldHasSpecies, 1);
    ASSERT_EQ(p.raw[0x13], 0x02);
    gen3_pkm_set(&p, Gen3FieldFateful, 1);
    ASSERT_EQ(p.raw[0x4F], 0x80);
    gen3_pkm_set(&p, Gen3FieldSpd, 0x0102);
    ASSERT_EQ(p.raw[0x62], 0x02);
    ASSERT_EQ(p.raw[0x63], 0x01);
}

TEST(test_field_extra_offsets) {
    Gen3Pkm p;
    memset(&p, 0, sizeof(p));
    gen3_pkm_set(&p, Gen3FieldFiller, 0x0201);
    ASSERT_EQ(p.raw[0x1E], 0x01);
    ASSERT_EQ(p.raw[0x1F], 0x02);
    gen3_pkm_set(&p, Gen3FieldGrowthUnknown, 0x0403);
    ASSERT_EQ(p.raw[0x2A], 0x03);
    ASSERT_EQ(p.raw[0x2B], 0x04);
    for(int i = 0; i < 6; i++) gen3_pkm_set(&p, Gen3FieldCoolness + i, 0x10 + i);
    for(int i = 0; i < 6; i++) ASSERT_EQ(p.raw[0x3E + i], 0x10 + i);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldSheen), 0x15);
    gen3_pkm_set(&p, Gen3FieldRibbons, 0x80000001);
    ASSERT_EQ(p.raw[0x4C], 0x01);
    ASSERT_EQ(p.raw[0x4F], 0x80);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldFateful), 1);
}

TEST(test_field_masking) {
    Gen3Pkm p;
    memset(&p, 0, sizeof(p));
    gen3_pkm_set(&p, Gen3FieldMetGame, 2);
    gen3_pkm_set(&p, Gen3FieldMetLevel, 0xFF);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldMetLevel), 0x7F);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldMetGame), 2);

    gen3_pkm_set(&p, Gen3FieldIvHp, 7);
    gen3_pkm_set(&p, Gen3FieldIvDef, 9);
    gen3_pkm_set(&p, Gen3FieldIvAtk, 40);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldIvAtk), 8);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldIvHp), 7);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldIvDef), 9);
}

TEST(test_names) {
    Gen3Pkm p;
    memset(&p, 0, sizeof(p));
    gen3_pkm_set_nickname(&p, "TREECKO");
    const uint8_t nick[10] = {0xCE, 0xCC, 0xBF, 0xBF, 0xBD, 0xC5, 0xC9, 0xFF, 0xFF, 0xFF};
    ASSERT_MEM_EQ(p.raw + 0x08, nick, 10);
    char out[16];
    ASSERT_EQ(gen3_pkm_nickname(&p, out, sizeof(out)), 7);
    ASSERT_STR_EQ(out, "TREECKO");

    gen3_pkm_set_ot_name(&p, "FLIPPERZERO");
    const uint8_t ot[7] = {0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC};
    ASSERT_MEM_EQ(p.raw + 0x14, ot, 7);
    ASSERT_EQ(p.raw[0x1B], 0);
    ASSERT_EQ(gen3_pkm_ot_name(&p, out, sizeof(out)), 7);
    ASSERT_STR_EQ(out, "FLIPPER");
}

/* The OT name bytes as stored, raw[0x14..0x1A], with nothing decoded or lost */
TEST(test_ot_name_raw) {
    static const uint8_t ruby[GEN3_OT_NAME_LEN] = {0xCC, 0xCF, 0xBC, 0xD3, 0xFF, 0xFF, 0xFF};
    static const uint8_t odd[GEN3_OT_NAME_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    Gen3Pkm p;
    memset(&p, 0xEE, sizeof(p));
    gen3_pkm_set_ot_name_raw(&p, ruby);
    ASSERT_MEM_EQ(p.raw + 0x14, ruby, GEN3_OT_NAME_LEN);
    ASSERT_EQ(p.raw[0x13], 0xEE);
    ASSERT_EQ(p.raw[0x1B], 0xEE);
    uint8_t got[GEN3_OT_NAME_LEN];
    gen3_pkm_ot_name_raw(&p, got);
    ASSERT_MEM_EQ(got, ruby, GEN3_OT_NAME_LEN);
    char out[16];
    ASSERT_EQ(gen3_pkm_ot_name(&p, out, sizeof(out)), 4);
    ASSERT_STR_EQ(out, "RUBY");

    /* Bytes the app can't show are kept byte for byte */
    gen3_pkm_set_ot_name_raw(&p, odd);
    ASSERT_MEM_EQ(p.raw + 0x14, odd, GEN3_OT_NAME_LEN);
    gen3_pkm_ot_name_raw(&p, got);
    ASSERT_MEM_EQ(got, odd, GEN3_OT_NAME_LEN);
    ASSERT_EQ(p.raw[0x13], 0xEE);
    ASSERT_EQ(p.raw[0x1B], 0xEE);
}

TEST(test_golden_build) {
    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_fix_checksum(&p);
    ASSERT_EQ(gen3_pkm_checksum(&p), 0x338D);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldChecksum), 0x338D);
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, 100);
}

TEST(test_golden_encrypt) {
    Gen3Pkm p;
    memcpy(p.raw, GOLDEN_PK3, 100);
    uint8_t out[GEN3_PARTY_SIZE];
    gen3_pkm_encrypt(&p, out);
    uint8_t want[100];
    golden_game(want);
    ASSERT_MEM_EQ(out, want, 100);
}

TEST(test_golden_decrypt) {
    uint8_t game[100];
    golden_game(game);
    Gen3Pkm p;
    memset(&p, 0x5A, sizeof(p));
    ASSERT_TRUE(gen3_pkm_decrypt(game, 100, &p));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, 100);

    ASSERT_TRUE(gen3_pkm_decrypt(game, 80, &p));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, 80);
    for(int i = 80; i < 100; i++) ASSERT_EQ(p.raw[i], 0);
}

/* Orders are listed by position, substructs G A E M (Bulbapedia; pokeemerald SUBSTRUCT_CASE). */
static const char* const ORDER[24] = {
    "GAEM", "GAME", "GEAM", "GEMA", "GMAE", "GMEA", "AGEM", "AGME", "AEGM", "AEMG", "AMGE", "AMEG",
    "EGAM", "EGMA", "EAGM", "EAMG", "EMGA", "EMAG", "MGAE", "MGEA", "MAGE", "MAEG", "MEGA", "MEAG"};

TEST(test_substruct_orders) {
    for(uint32_t n = 0; n < 24; n++) {
        Gen3Pkm p;
        memset(&p, 0, sizeof(p));
        p.raw[0x20] = 0xA0;
        p.raw[0x2C] = 0xA1;
        p.raw[0x38] = 0xA2;
        p.raw[0x44] = 0xA3;
        gen3_pkm_set(&p, Gen3FieldPid, n + 24 * 1000);
        gen3_pkm_set(&p, Gen3FieldOtId, n + 24 * 1000); /* XOR key 0 */
        uint8_t out[GEN3_PARTY_SIZE];
        gen3_pkm_encrypt(&p, out);
        for(int pos = 0; pos < 4; pos++) {
            const char* idx = strchr("GAEM", ORDER[n][pos]);
            ASSERT_EQ(out[0x20 + 12 * pos], 0xA0 + (idx - "GAEM"));
        }
    }
}

TEST(test_roundtrip_all_orders) {
    uint32_t lcg = 12345;
    for(uint32_t n = 0; n < 24; n++) {
        Gen3Pkm p;
        memset(&p, 0, sizeof(p));
        for(int i = 0; i < 0x50; i++) {
            lcg = lcg * 1664525u + 1013904223u;
            p.raw[i] = lcg >> 24;
        }
        for(int i = 0x50; i < 100; i++) p.raw[i] = i;
        gen3_pkm_set(&p, Gen3FieldPid, 0x9E377A3F + n);
        gen3_pkm_set(&p, Gen3FieldOtId, 0xDEADBEEF);
        gen3_pkm_fix_checksum(&p);
        uint8_t game[GEN3_PARTY_SIZE];
        gen3_pkm_encrypt(&p, game);
        Gen3Pkm q;
        ASSERT_TRUE(gen3_pkm_decrypt(game, 100, &q));
        ASSERT_MEM_EQ(q.raw, p.raw, 100);
    }
}

TEST(test_decrypt_bad_checksum) {
    uint8_t game[100];
    golden_game(game);
    game[0x30] ^= 1;
    Gen3Pkm p;
    memset(&p, 0x5A, sizeof(p));
    ASSERT_TRUE(!gen3_pkm_decrypt(game, 100, &p));
    for(int i = 0; i < 100; i++) ASSERT_EQ(p.raw[i], 0x5A);
}

TEST(test_decrypt_bad_len) {
    uint8_t game[100];
    golden_game(game);
    Gen3Pkm p;
    ASSERT_TRUE(!gen3_pkm_decrypt(game, 0, &p));
    ASSERT_TRUE(!gen3_pkm_decrypt(game, 79, &p));
    ASSERT_TRUE(!gen3_pkm_decrypt(game, 99, &p));
}

int main(void) {
    RUN(test_field_layout);
    RUN(test_field_extra_offsets);
    RUN(test_field_masking);
    RUN(test_names);
    RUN(test_ot_name_raw);
    RUN(test_golden_build);
    RUN(test_golden_encrypt);
    RUN(test_golden_decrypt);
    RUN(test_substruct_orders);
    RUN(test_roundtrip_all_orders);
    RUN(test_decrypt_bad_checksum);
    RUN(test_decrypt_bad_len);
    TEST_EXIT();
}
