#include "test.h"
#include "bulk_file.h"
#include "gen3_golden.h"
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_pk3.h>

static void golden(Gen3Pkm* p) {
    memcpy(p->raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
}

static Gen3Check import_pkm(Gen3Pkm* in, size_t len, Gen3Pkm* out, Gen3ImportInfo* info) {
    gen3_pkm_fix_checksum(in);
    return gen3_pk3_import(in->raw, len, out, info);
}

TEST(test_import_80_golden) {
    Gen3Pkm out;
    memset(&out, 0x5A, sizeof(out));
    ASSERT_EQ(gen3_pk3_import(GOLDEN_PK3, 80, &out, NULL), Gen3CheckOk);
    ASSERT_MEM_EQ(out.raw, GOLDEN_PK3, 100);
}

TEST(test_import_100_keeps_bytes) {
    Gen3Pkm in, out;
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldAtk, 999);
    ASSERT_EQ(gen3_pk3_import(in.raw, 100, &out, NULL), Gen3CheckOk);
    /* party stats are recomputed, so the stored Atk 999 becomes the computed 12 */
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldAtk), 12);
    ASSERT_MEM_EQ(out.raw, GOLDEN_PK3, 100);

    /* The party status and the current HP are kept. The mail index isn't: no mail record comes
     * with a file, so a stale index (0x55 from another save, say) is cleared to 0xFF. */
    static const uint8_t stale[] = {3, 0x55};
    for(size_t i = 0; i < sizeof(stale); i++) {
        Gen3ImportInfo info;
        golden(&in);
        gen3_pkm_set(&in, Gen3FieldStatus, 0x07);
        gen3_pkm_set(&in, Gen3FieldMail, stale[i]);
        gen3_pkm_set(&in, Gen3FieldHp, 7);
        ASSERT_EQ(gen3_pk3_import(in.raw, 100, &out, &info), Gen3CheckOk);
        ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldStatus), 7);
        ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldMail), 0xFF);
        ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldHp), 7);
        ASSERT_TRUE(info.normalise.mail_cleared);
        ASSERT_TRUE(!info.mail_removed);
    }
    /* An 80-byte file has no mail index: 0xFF, and nothing to report */
    Gen3ImportInfo info;
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldMail, 0x55);
    ASSERT_EQ(gen3_pk3_import(in.raw, 80, &out, &info), Gen3CheckOk);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldMail), 0xFF);
    ASSERT_TRUE(!info.normalise.mail_cleared);
}

TEST(test_export_roundtrip_every_byte) {
    Gen3Pkm in, out;
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldMarkings, 0x0F);
    gen3_pkm_set(&in, Gen3FieldGrowthUnknown, 0xBEEF);
    gen3_pkm_set(&in, Gen3FieldFiller, 0x1234);
    for(int i = 0; i < 6; i++) gen3_pkm_set(&in, Gen3FieldCoolness + i, 1 + i);
    gen3_pkm_set(&in, Gen3FieldRibbons, 0x7FFFFFFF);
    gen3_pkm_set(&in, Gen3FieldPpBonuses, 0xE4);
    gen3_pkm_set(&in, Gen3FieldMetLocation, 0x10);
    ASSERT_EQ(import_pkm(&in, 100, &out, NULL), Gen3CheckOk);
    uint8_t exp[GEN3_BOX_SIZE];
    gen3_pk3_export(&out, exp);
    ASSERT_MEM_EQ(exp, in.raw, GEN3_BOX_SIZE);
}

TEST(test_import_sizes) {
    static const size_t lens[] = {0, 79, 81, 99, 101};
    uint8_t buf[101];
    memcpy(buf, GOLDEN_PK3, 100);
    buf[100] = 0;
    for(size_t i = 0; i < sizeof(lens) / sizeof(lens[0]); i++) {
        Gen3Pkm out;
        memset(&out, 0x5A, sizeof(out));
        ASSERT_EQ(gen3_pk3_import(buf, lens[i], &out, NULL), Gen3CheckBadSize);
        for(int k = 0; k < 100; k++) ASSERT_EQ(out.raw[k], 0x5A);
    }
}

TEST(test_import_bad_checksum) {
    Gen3Pkm in, out;
    golden(&in);
    in.raw[0x30] ^= 1;
    ASSERT_EQ(gen3_pk3_import(in.raw, 100, &out, NULL), Gen3CheckBadChecksum);
}

TEST(test_import_bad_species) {
    static const uint16_t species[] = {0, 252, 276, 412};
    for(size_t i = 0; i < sizeof(species) / sizeof(species[0]); i++) {
        Gen3Pkm in, out;
        golden(&in);
        gen3_pkm_set(&in, Gen3FieldSpecies, species[i]);
        ASSERT_EQ(import_pkm(&in, 100, &out, NULL), Gen3CheckBadSpecies);
    }
}

TEST(test_import_egg) {
    Gen3Pkm in, out;
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldIsEgg, 1);
    ASSERT_EQ(import_pkm(&in, 100, &out, NULL), Gen3CheckEgg);
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldEggFlag, 1);
    ASSERT_EQ(import_pkm(&in, 100, &out, NULL), Gen3CheckEgg);
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldBadEgg, 1);
    ASSERT_EQ(import_pkm(&in, 100, &out, NULL), Gen3CheckBadEgg);
}

TEST(test_import_bad_move_item) {
    Gen3Pkm in, out;
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldMove2, 355);
    ASSERT_EQ(import_pkm(&in, 100, &out, NULL), Gen3CheckBadMove);
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldHeldItem, 377);
    ASSERT_EQ(import_pkm(&in, 100, &out, NULL), Gen3CheckBadItem);
}

TEST(test_import_sets_has_species) {
    Gen3Pkm in, out;
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldHasSpecies, 0);
    ASSERT_EQ(import_pkm(&in, 100, &out, NULL), Gen3CheckOk);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldHasSpecies), 1);
}

TEST(test_import_normalises) {
    Gen3Pkm in, out;
    Gen3ImportInfo info;
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldMove1, 1);
    gen3_pkm_set(&in, Gen3FieldMove2, 0);
    gen3_pkm_set(&in, Gen3FieldMove3, 1); /* duplicate of Move1, after a gap */
    gen3_pkm_set(&in, Gen3FieldMove4, 43);
    gen3_pkm_set(&in, Gen3FieldPp3, 35);
    gen3_pkm_set(&in, Gen3FieldPp4, 30);
    gen3_pkm_set(&in, Gen3FieldPp2, 0);
    gen3_pkm_set(&in, Gen3FieldAbilityBit, 1); /* Treecko has one ability */
    ASSERT_EQ(import_pkm(&in, 100, &out, &info), Gen3CheckOk);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldMove1), 1);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldMove2), 43);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldMove3), 0);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldMove4), 0);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldPp2), 30);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldAbilityBit), 0);
    ASSERT_TRUE(info.normalise.duplicates_removed);
    ASSERT_TRUE(info.normalise.gaps_packed);
    ASSERT_TRUE(info.normalise.ability_fixed);
    ASSERT_TRUE(!info.normalise.exp_clamped);
    ASSERT_TRUE(!info.mail_removed);
    ASSERT_EQ(gen3_pkm_checksum(&out), gen3_pkm_get(&out, Gen3FieldChecksum));

    /* exp 0 is raised to level 1 */
    golden(&in);
    gen3_pkm_set(&in, Gen3FieldExp, 0);
    ASSERT_EQ(import_pkm(&in, 100, &out, &info), Gen3CheckOk);
    ASSERT_TRUE(info.normalise.exp_clamped);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldExp), gen3_exp_for_level(gen3_species(252 /* Treecko */)->growth, 1));
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldLevel), 1);
    ASSERT_TRUE(gen3_pkm_get(&out, Gen3FieldMaxHp) > 0);
    ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldHp), gen3_pkm_get(&out, Gen3FieldMaxHp));
}

TEST(test_import_strips_mail) {
    for(uint16_t item = 121; item <= 132; item += 11) {
        for(int k = 0; k < 2; k++) {
            Gen3Pkm in, out;
            Gen3ImportInfo info;
            golden(&in);
            gen3_pkm_set(&in, Gen3FieldHeldItem, item);
            gen3_pkm_set(&in, Gen3FieldMail, 2);
            ASSERT_EQ(import_pkm(&in, k ? 80 : 100, &out, &info), Gen3CheckOk);
            ASSERT_TRUE(info.mail_removed);
            ASSERT_TRUE(info.normalise.mail_item_removed);
            ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldHeldItem), 0);
            ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldMail), 0xFF);
            ASSERT_EQ(gen3_pkm_checksum(&out), gen3_pkm_get(&out, Gen3FieldChecksum));
        }
    }
    /* a holdable item and the neighbours of the mail range stay */
    static const uint16_t keep[] = {120, 133, 1};
    for(size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        Gen3Pkm in, out;
        Gen3ImportInfo info;
        golden(&in);
        gen3_pkm_set(&in, Gen3FieldHeldItem, keep[i]);
        ASSERT_EQ(import_pkm(&in, 100, &out, &info), Gen3CheckOk);
        ASSERT_TRUE(!info.mail_removed);
        ASSERT_EQ(gen3_pkm_get(&out, Gen3FieldHeldItem), keep[i]);
    }
}

TEST(test_pk3_default_name) {
    Gen3Pkm p;
    char out[GEN3_PK3_NAME_MAX + 1];
    memset(&p, 0, sizeof(p));
    gen3_pkm_set_nickname(&p, "MR. MIME");
    gen3_pk3_default_name(&p, out, sizeof(out));
    ASSERT_STR_EQ(out, "MRMIME");
    gen3_pkm_set_nickname(&p, "HO-OH");
    gen3_pk3_default_name(&p, out, sizeof(out));
    ASSERT_STR_EQ(out, "HO-OH");
    gen3_pkm_set_nickname(&p, "NIDORAN\200");
    gen3_pk3_default_name(&p, out, sizeof(out));
    ASSERT_STR_EQ(out, "NIDORAN");
    gen3_pkm_set_nickname(&p, "?!");
    gen3_pk3_default_name(&p, out, sizeof(out));
    ASSERT_STR_EQ(out, "POKEMON");
}

TEST(test_pk3_name_valid) {
    ASSERT_TRUE(gen3_pk3_name_valid("TREECKO"));
    ASSERT_TRUE(gen3_pk3_name_valid("my_mon-1"));
    ASSERT_TRUE(gen3_pk3_name_valid("ABCDEFGHIJKLMNOP"));
    ASSERT_TRUE(!gen3_pk3_name_valid(""));
    ASSERT_TRUE(!gen3_pk3_name_valid("a b"));
    ASSERT_TRUE(!gen3_pk3_name_valid("a.pk3"));
    ASSERT_TRUE(!gen3_pk3_name_valid("ABCDEFGHIJKLMNOPQ"));
}

TEST(test_check_text) {
    for(int c = Gen3CheckOk; c <= Gen3CheckNoData; c++) {
        ASSERT_TRUE(gen3_check_text((Gen3Check)c)[0] != 0);
    }
    ASSERT_STR_EQ(gen3_check_text(Gen3CheckEgg), "Eggs not supported");
    ASSERT_STR_EQ(gen3_check_text(Gen3CheckOk), "OK");
    ASSERT_STR_EQ(gen3_check_text(Gen3CheckNoData), "Gen III data unreadable");
}

/* A moveless Pokemon needs the default moveset from the bulk file. Without it (mode 0: reads fail
 * after a good open; mode 1: no source) the file is refused and *out is untouched. */
TEST(test_import_no_data) {
    Gen3Pkm in, out;
    Gen3ImportInfo info;
    for(int mode = 0; mode < 2; mode++) {
        ASSERT_TRUE(bulk_file_open());
        if(mode == 0)
            bulk_file_failing = true;
        else
            gen3_bulk_close();
        for(size_t len = GEN3_BOX_SIZE; len <= GEN3_PARTY_SIZE; len += 20) {
            golden(&in);
            for(int s = 0; s < 4; s++) {
                gen3_pkm_set(&in, Gen3FieldMove1 + s, 0);
                gen3_pkm_set(&in, Gen3FieldPp1 + s, 0);
            }
            memset(&out, 0x5A, sizeof(out));
            memset(&info, 0xFF, sizeof(info));
            ASSERT_EQ(import_pkm(&in, len, &out, &info), Gen3CheckNoData);
            for(size_t i = 0; i < sizeof(out.raw); i++) ASSERT_EQ(out.raw[i], 0x5A);
            ASSERT_TRUE(!info.mail_removed && !info.normalise.moves_defaulted);
        }
        /* With a move, no bulk data is needed */
        golden(&in);
        ASSERT_EQ(import_pkm(&in, GEN3_BOX_SIZE, &out, &info), Gen3CheckOk);
        ASSERT_MEM_EQ(out.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    }
    /* With the data, the moveless Pokemon gets Treecko L5's default moveset */
    ASSERT_TRUE(bulk_file_open());
    golden(&in);
    for(int s = 0; s < 4; s++) gen3_pkm_set(&in, Gen3FieldMove1 + s, 0);
    ASSERT_EQ(import_pkm(&in, GEN3_BOX_SIZE, &out, &info), Gen3CheckOk);
    ASSERT_TRUE(info.normalise.moves_defaulted);
    ASSERT_MEM_EQ(out.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_import_80_golden);
    RUN(test_import_100_keeps_bytes);
    RUN(test_export_roundtrip_every_byte);
    RUN(test_import_sizes);
    RUN(test_import_bad_checksum);
    RUN(test_import_bad_species);
    RUN(test_import_egg);
    RUN(test_import_bad_move_item);
    RUN(test_import_sets_has_species);
    RUN(test_import_normalises);
    RUN(test_import_strips_mail);
    RUN(test_pk3_default_name);
    RUN(test_pk3_name_valid);
    RUN(test_check_text);
    RUN(test_import_no_data);
    TEST_EXIT();
}
