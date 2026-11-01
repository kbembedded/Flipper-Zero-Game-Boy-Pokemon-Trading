#include "test.h"
#include "bulk_file.h"
#include "gen3_golden.h"
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_rules.h>

#define G(f) gen3_pkm_get(&ed.pkm, Gen3Field##f)

#define RULE(cond, what) \
    do { \
        if(!(cond)) { \
            printf("  edit_consistent: %s\n", what); \
            return false; \
        } \
    } while(0)

/* The game-valid invariant. full_hp: Hp == MaxHp (every mutator); else Hp <= MaxHp (a loaded
 * Pokemon keeps its current HP). */
static bool edit_consistent_hp(const Gen3Editor* ed, bool full_hp) {
    const Gen3Pkm* p = &ed->pkm;
    RULE(gen3_pkm_checksum(p) == gen3_pkm_get(p, Gen3FieldChecksum), "checksum mismatch");
    RULE(gen3_pkm_get(p, Gen3FieldHasSpecies) == 1, "HasSpecies is 0");
    RULE(gen3_pkm_get(p, Gen3FieldIsEgg) == 0, "IsEgg set");
    RULE(gen3_pkm_get(p, Gen3FieldEggFlag) == 0, "EggFlag set");
    RULE(gen3_pkm_get(p, Gen3FieldBadEgg) == 0, "BadEgg set");
    const Gen3SpeciesInfo* info =
        gen3_species(gen3_national_from_internal(gen3_pkm_get(p, Gen3FieldSpecies)));
    RULE(info != NULL, "species maps to no national");
    uint8_t level = gen3_level_for_exp(info->growth, gen3_pkm_get(p, Gen3FieldExp));
    RULE(level >= 1, "exp below level 1");
    RULE(gen3_pkm_get(p, Gen3FieldLevel) == level, "Level != level_for_exp");
    uint16_t st[Gen3StatCount];
    gen3_calc_stats(p, st);
    for(int s = 0; s < Gen3StatCount; s++)
        RULE(gen3_pkm_get(p, Gen3FieldMaxHp + s) == st[s], "stats != calc_stats");
    uint32_t hp = gen3_pkm_get(p, Gen3FieldHp);
    RULE(full_hp ? hp == st[Gen3StatHp] : hp <= st[Gen3StatHp], "Hp vs MaxHp");
    RULE(
        info->ability[1] != 0 || gen3_pkm_get(p, Gen3FieldAbilityBit) == 0,
        "ability bit set on a one-ability species");
    int n = 0;
    bool gap = false;
    for(int s = 0; s < 4; s++) {
        uint32_t m = gen3_pkm_get(p, Gen3FieldMove1 + s);
        if(m == 0) {
            gap = true;
            continue;
        }
        RULE(!gap, "moves not packed");
        for(int t = 0; t < s; t++)
            RULE(gen3_pkm_get(p, Gen3FieldMove1 + t) != m, "duplicate move");
        n++;
    }
    RULE(n >= 1, "no moves");
    uint32_t item = gen3_pkm_get(p, Gen3FieldHeldItem);
    RULE(item < GEN3_ITEM_MAIL_FIRST || item > GEN3_ITEM_MAIL_LAST, "holds a Mail item");
    RULE(gen3_pkm_get(p, Gen3FieldMail) == GEN3_MAIL_NONE, "mail index != 0xFF");
    return true;
}

static bool edit_consistent(const Gen3Editor* ed) {
    return edit_consistent_hp(ed, true);
}

static void golden_pkm(Gen3Pkm* p) {
    memcpy(p->raw, GOLDEN_PK3, sizeof(p->raw));
}

static bool moves_are(const Gen3Editor* ed, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    const uint16_t want[4] = {a, b, c, d};
    for(int s = 0; s < 4; s++)
        if(gen3_pkm_get(&ed->pkm, Gen3FieldMove1 + s) != want[s]) return false;
    return true;
}

static bool pp_are(const Gen3Editor* ed, uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t want[4] = {a, b, c, d};
    for(int s = 0; s < 4; s++)
        if(gen3_pkm_get(&ed->pkm, Gen3FieldPp1 + s) != want[s]) return false;
    return true;
}

/* The whole editor, the RNG included, field by field (no padding compared) */
static bool editor_eq(const Gen3Editor* a, const Gen3Editor* b) {
    return memcmp(a->pkm.raw, b->pkm.raw, GEN3_PARTY_SIZE) == 0 && a->preset == b->preset &&
           a->preset_applied == b->preset_applied && a->rng.state == b->rng.state &&
           a->pristine == b->pristine && a->has_trainer == b->has_trainer &&
           memcmp(a->trainer.name, b->trainer.name, GEN3_OT_NAME_LEN) == 0 &&
           a->trainer.tid == b->trainer.tid && a->trainer.sid == b->trainer.sid &&
           a->trainer.gender == b->trainer.gender;
}

/* The GBA player of the D7 tests: RUBY, TID 12345, SID 54321 (OT ID 0xD4313039), female */
static const uint8_t RUBY_NAME[GEN3_OT_NAME_LEN] = {0xCC, 0xCF, 0xBC, 0xD3, 0xFF, 0xFF, 0xFF};
static const uint8_t FLIPPER_NAME[GEN3_OT_NAME_LEN] = {0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC};
#define RUBY_OT_ID 0xD4313039u

static void ruby_trainer(Gen3Trainer* t) {
    memset(t, 0, sizeof(*t));
    memcpy(t->name, RUBY_NAME, GEN3_OT_NAME_LEN);
    t->tid = 12345;
    t->sid = 54321;
    t->gender = 1;
}

static bool ot_bytes_are(const Gen3Editor* ed, const uint8_t name[GEN3_OT_NAME_LEN]) {
    return memcmp(ed->pkm.raw + 0x14, name, GEN3_OT_NAME_LEN) == 0;
}

TEST(test_edit_init_defaults) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    char buf[16];
    ASSERT_EQ(gen3_edit_national(&ed), 1);
    ASSERT_EQ(G(Species), 1);
    ASSERT_EQ(G(Level), 5);
    ASSERT_EQ(G(Exp), 135);
    gen3_pkm_nickname(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "BULBASAUR");
    gen3_pkm_ot_name(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "FLIPPER");
    ASSERT_EQ(G(OtId), 22222);
    ASSERT_EQ(G(OtGender), 0);
    ASSERT_TRUE(moves_are(&ed, 33, 45, 0, 0));
    ASSERT_TRUE(pp_are(&ed, 35, 40, 0, 0));
    ASSERT_EQ(G(PpBonuses), 0);
    ASSERT_EQ(G(HeldItem), 0);
    ASSERT_EQ(G(Ball), 4);
    ASSERT_EQ(G(MetLocation), 0xFF);
    ASSERT_EQ(G(MetLevel), 5);
    ASSERT_EQ(G(MetGame), 2);
    ASSERT_EQ(G(Friendship), 70);
    ASSERT_EQ(G(Language), 2);
    ASSERT_EQ(G(Mail), 0xFF);
    ASSERT_EQ(G(Status), 0);
    ASSERT_EQ(G(Pokerus), 0);
    ASSERT_EQ(G(Fateful), 0);
    for(int s = 0; s < Gen3StatCount; s++) ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 0);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    ASSERT_EQ(ed.preset, Gen3PresetRandIvZeroEv);
    ASSERT_TRUE(ed.preset_applied);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_resets_keeps_ot) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_ot_name(&ed, "ASH");
    gen3_edit_set_ot_id(&ed, 1234, 4321);
    gen3_pkm_set(&ed.pkm, Gen3FieldOtGender, 1); /* no editor setter; the next edit commits it */
    gen3_pkm_set(&ed.pkm, Gen3FieldPpBonuses, 0xFF);
    gen3_pkm_set(&ed.pkm, Gen3FieldRibbons, 0x00ABCDEF);
    gen3_pkm_set(&ed.pkm, Gen3FieldCoolness, 9);
    gen3_pkm_set(&ed.pkm, Gen3FieldMarkings, 0x0F);
    gen3_pkm_set(&ed.pkm, Gen3FieldStatus, 0x08);
    gen3_edit_set_level(&ed, 30);
    gen3_edit_set_item(&ed, 200);
    gen3_edit_set_ball(&ed, 1);
    gen3_edit_set_pokerus(&ed, 0x41);
    gen3_edit_set_nickname(&ed, "LEAFY");
    gen3_edit_set_preset(&ed, Gen3PresetMaxIvLevelEv);

    gen3_edit_set_species(&ed, 252);
    char buf[16];
    ASSERT_EQ(gen3_edit_national(&ed), 252);
    gen3_pkm_nickname(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "TREECKO");
    ASSERT_EQ(G(Level), 30);
    ASSERT_EQ(G(Exp), 21760);
    gen3_pkm_ot_name(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "ASH");
    ASSERT_EQ(G(OtId), (4321u << 16) | 1234u);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_EQ(G(HeldItem), 0);
    ASSERT_EQ(G(Ball), 4);
    ASSERT_EQ(G(Pokerus), 0);
    ASSERT_TRUE(moves_are(&ed, 98, 228, 103, 72));
    ASSERT_TRUE(pp_are(&ed, 30, 20, 40, 10)); /* full base PP */
    ASSERT_EQ(G(PpBonuses), 0);
    ASSERT_EQ(G(MetLevel), 30);
    ASSERT_EQ(G(MetLocation), 0xFF);
    ASSERT_EQ(G(MetGame), 2);
    ASSERT_EQ(G(Friendship), 70);
    ASSERT_EQ(G(Ribbons), 0);
    ASSERT_EQ(G(Coolness), 0);
    ASSERT_EQ(G(Markings), 0);
    ASSERT_EQ(G(Status), 0);
    ASSERT_EQ(G(Mail), 0xFF);
    ASSERT_EQ(G(Language), 2);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    /* The preset is kept and applied to the new Pokemon. */
    ASSERT_EQ(ed.preset, Gen3PresetMaxIvLevelEv);
    ASSERT_TRUE(ed.preset_applied);
    for(int s = 0; s < Gen3StatCount; s++) {
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldIvHp + s), 31);
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 85 * 30 / 100);
    }
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_stores_internal) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 252);
    ASSERT_EQ(G(Species), 277);
    ASSERT_EQ(ed.pkm.raw[0x20], 0x15);
    ASSERT_EQ(ed.pkm.raw[0x21], 0x01);
    ASSERT_EQ(gen3_edit_national(&ed), 252);
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_species(&ed, 386);
    ASSERT_EQ(G(Species), 410);
    ASSERT_EQ(gen3_edit_national(&ed), 386);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_same_is_noop) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_nickname(&ed, "LEAFY");
    Gen3Pkm before = ed.pkm;
    uint32_t rng = ed.rng.state;
    gen3_edit_set_species(&ed, 1);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    gen3_edit_set_species(&ed, 0);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    gen3_edit_set_species(&ed, 387);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_EQ(ed.rng.state, rng);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_skips_shiny_pid) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    /* Make the next random PID shiny for this OT: the new Pokemon must not take it. */
    Gen3Rng peek = ed.rng;
    uint32_t r = gen3_rng_next(&peek);
    uint16_t tid = (r >> 16) ^ (r & 0xFFFF);
    gen3_pkm_set(&ed.pkm, Gen3FieldOtId, tid);
    ASSERT_TRUE(gen3_is_shiny(tid, r));
    gen3_edit_set_species(&ed, 4);
    ASSERT_TRUE(G(Pid) != r);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_species_fateful) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 151);
    ASSERT_EQ(G(Fateful), 1);
    ASSERT_EQ(G(Friendship), 100); /* species base friendship, not a fixed 70 */
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_species(&ed, 386);
    ASSERT_EQ(G(Fateful), 1);
    ASSERT_EQ(G(Friendship), 0);
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_species(&ed, 1);
    ASSERT_EQ(G(Fateful), 0);
    ASSERT_EQ(G(Friendship), 70);
    ASSERT_TRUE(edit_consistent(&ed));
}

/* The fateful-encounter bit by itself (decrypted raw 0x4F bit 7), for a Mew or Deoxys that came
 * without it; pristine follows gen3_edit.h */
TEST(test_edit_set_fateful) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_EQ(G(Fateful), 0); /* BULBASAUR */
    Gen3Editor before = ed;
    gen3_edit_set_fateful(&ed, false);
    ASSERT_TRUE(editor_eq(&ed, &before));
    ASSERT_TRUE(ed.pristine);
    gen3_edit_set_fateful(&ed, true);
    ASSERT_EQ(G(Fateful), 1);
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(edit_consistent(&ed));

    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(gen3_edit_set_species(&ed, 151));
    ASSERT_EQ(G(Fateful), 1);
    const Gen3Pkm mew = ed.pkm;
    gen3_edit_set_fateful(&ed, false);
    ASSERT_EQ(G(Fateful), 0);
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(edit_consistent(&ed));
    /* Only that bit and the checksum changed */
    ASSERT_EQ(ed.pkm.raw[0x4F], mew.raw[0x4F] & 0x7F);
    for(int i = 0; i < GEN3_PARTY_SIZE; i++) {
        if(i == 0x1C || i == 0x1D || i == 0x4F) continue;
        ASSERT_EQ(ed.pkm.raw[i], mew.raw[i]);
    }
    gen3_edit_set_fateful(&ed, true);
    ASSERT_EQ(G(Fateful), 1);
    ASSERT_MEM_EQ(ed.pkm.raw, mew.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_all_species) {
    static const uint8_t levels[2] = {1, 100};
    for(int li = 0; li < 2; li++) {
        for(uint16_t n = 1; n <= GEN3_NATIONAL_COUNT; n++) {
            const Gen3SpeciesInfo* info = gen3_species(n);
            Gen3Editor ed;
            gen3_edit_init(&ed, n);
            gen3_edit_set_level(&ed, levels[li]);
            gen3_edit_set_species(&ed, n);
            bool ok = gen3_edit_national(&ed) == n && G(Species) == info->internal &&
                      G(Level) == levels[li] && G(Friendship) == info->friendship &&
                      edit_consistent(&ed);
            /* D6: a new Pokemon's ability slot is pid & 1 on a two-ability species. */
            if(info->ability[1] != 0) ok = ok && G(AbilityBit) == (G(Pid) & 1);
            for(int s = 0; ok && s < 4; s++) {
                uint16_t m = gen3_pkm_get(&ed.pkm, Gen3FieldMove1 + s);
                ok = gen3_pkm_get(&ed.pkm, Gen3FieldPp1 + s) == (m ? gen3_move_pp(m) : 0);
            }
            if(!ok) printf("  national %u level %u\n", n, levels[li]);
            ASSERT_TRUE(ok);
        }
    }
}

TEST(test_edit_level) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_level(&ed, 50);
    ASSERT_EQ(G(Level), 50);
    ASSERT_EQ(G(Exp), 117360);
    ASSERT_TRUE(moves_are(&ed, 33, 45, 0, 0));
    ASSERT_EQ(G(MetLevel), 5);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_level_clamped) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_level(&ed, 0);
    ASSERT_EQ(G(Level), 1);
    ASSERT_EQ(G(Exp), 1);
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_level(&ed, 101);
    ASSERT_EQ(G(Level), 100);
    ASSERT_EQ(G(Exp), 1059860);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_presets) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_level(&ed, 50);
    /* EV per preset at level 50: zero, 85 * 50 / 100, max. */
    static const uint8_t ev[6] = {0, 42, 85, 0, 42, 85};
    ASSERT_EQ(GEN3_PRESET_COUNT, 6);
    ASSERT_EQ(Gen3PresetMaxIvMaxEv + 1, GEN3_PRESET_COUNT);
    for(int pr = Gen3PresetRandIvZeroEv; pr < GEN3_PRESET_COUNT; pr++) {
        gen3_edit_set_preset(&ed, (Gen3Preset)pr);
        ASSERT_EQ(ed.preset, pr);
        ASSERT_TRUE(ed.preset_applied);
        int sum = 0;
        for(int s = 0; s < Gen3StatCount; s++) {
            uint32_t iv = gen3_pkm_get(&ed.pkm, Gen3FieldIvHp + s);
            ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), ev[pr]);
            sum += gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s);
            if(pr >= Gen3PresetMaxIvZeroEv) ASSERT_EQ(iv, 31);
        }
        if(pr == Gen3PresetMaxIvMaxEv) ASSERT_EQ(sum, 510);
        ASSERT_TRUE(edit_consistent(&ed));
    }
    /* Random IVs: 5 bits each of the next RNG word, so each application re-rolls them. */
    for(int round = 0; round < 2; round++) {
        Gen3Rng peek = ed.rng;
        uint32_t r = gen3_rng_next(&peek);
        gen3_edit_set_preset(&ed, Gen3PresetRandIvLevelEv);
        for(int s = 0; s < Gen3StatCount; s++)
            ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldIvHp + s), (r >> (5 * s)) & 31);
        ASSERT_EQ(ed.rng.state, peek.state);
    }
    Gen3Pkm before = ed.pkm;
    gen3_edit_set_preset(&ed, (Gen3Preset)GEN3_PRESET_COUNT);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_EQ(ed.preset, Gen3PresetRandIvLevelEv);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_level_ev_follows_level) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_preset(&ed, Gen3PresetMaxIvLevelEv);
    gen3_edit_set_level(&ed, 100);
    for(int s = 0; s < Gen3StatCount; s++) ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 85);
    ASSERT_TRUE(edit_consistent(&ed));
    gen3_edit_set_level(&ed, 20);
    for(int s = 0; s < Gen3StatCount; s++) ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 17);
    ASSERT_TRUE(edit_consistent(&ed));
    /* A MaxEv preset doesn't follow the level. */
    gen3_edit_set_preset(&ed, Gen3PresetRandIvMaxEv);
    gen3_edit_set_level(&ed, 40);
    for(int s = 0; s < Gen3StatCount; s++) ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), 85);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_loaded_evs_kept) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_preset(&ed, Gen3PresetMaxIvLevelEv);
    Gen3Pkm in;
    golden_pkm(&in);
    static const uint8_t evs[6] = {252, 252, 6, 0, 0, 0};
    for(int s = 0; s < Gen3StatCount; s++) gen3_pkm_set(&in, Gen3FieldEvHp + s, evs[s]);
    gen3_pkm_fix_checksum(&in);
    gen3_edit_load(&ed, &in);
    ASSERT_TRUE(!ed.preset_applied);
    ASSERT_EQ(ed.preset, Gen3PresetMaxIvLevelEv);
    gen3_edit_set_level(&ed, 40);
    ASSERT_EQ(G(Level), 40);
    for(int s = 0; s < Gen3StatCount; s++) {
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldEvHp + s), evs[s]);
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldIvHp + s), 31 - s); /* golden IVs */
    }
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_nickname_default) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 29);
    gen3_edit_set_nickname(&ed, "LEAFY");
    gen3_edit_set_nickname(&ed, "");
    static const uint8_t want[10] = {0xC8, 0xC3, 0xBE, 0xC9, 0xCC, 0xBB, 0xC8, 0xB6, 0xFF, 0xFF};
    ASSERT_MEM_EQ(ed.pkm.raw + 0x08, want, 10);
    ASSERT_TRUE(edit_consistent(&ed));
}

/* Confirming the value already shown changes nothing: the level keeps exp inside the level and
 * the current HP; a name keeps the bytes it shows as '?' or '.'. */
TEST(test_edit_same_value_is_noop) {
    const Gen3SpeciesInfo* info = gen3_species(252); /* golden: Treecko L5 */
    uint32_t exp = gen3_exp_for_level(info->growth, 5) + 20;
    ASSERT_EQ(gen3_level_for_exp(info->growth, exp), 5);
    Gen3Pkm in;
    golden_pkm(&in);
    gen3_pkm_set(&in, Gen3FieldExp, exp);
    gen3_pkm_set(&in, Gen3FieldHp, 7);
    gen3_pkm_set_nickname(&in, "LEAFX");
    in.raw[0x08 + 4] = 0x01; /* a character the app can't show: '?' */
    gen3_pkm_set_ot_name(&in, "ASHX");
    in.raw[0x14 + 3] = 0xB0; /* the ellipsis, shown as '.' (which encodes as 0xAD) */
    gen3_pkm_fix_checksum(&in);
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(gen3_edit_load(&ed, &in));
    char buf[16];
    gen3_pkm_nickname(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "LEAF?");
    gen3_pkm_ot_name(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "ASH.");
    Gen3Editor before = ed;

    gen3_edit_set_level(&ed, 5);
    ASSERT_TRUE(editor_eq(&ed, &before));
    ASSERT_TRUE(gen3_edit_set_nickname(&ed, "LEAF?"));
    ASSERT_TRUE(editor_eq(&ed, &before));
    gen3_edit_set_ot_name(&ed, "ASH.");
    ASSERT_TRUE(editor_eq(&ed, &before));
    ASSERT_EQ(G(Hp), 7);
    ASSERT_TRUE(edit_consistent_hp(&ed, false));

    /* A different value is a real edit */
    gen3_edit_set_ot_name(&ed, "ASH");
    ASSERT_EQ(ed.pkm.raw[0x14 + 3], 0xFF);
    ASSERT_TRUE(gen3_edit_set_nickname(&ed, "LEAF!"));
    ASSERT_EQ(ed.pkm.raw[0x08 + 4], 0xAB);
    gen3_edit_set_level(&ed, 6);
    ASSERT_EQ(G(Exp), gen3_exp_for_level(info->growth, 6));
    ASSERT_TRUE(edit_consistent(&ed));

    /* The empty nickname is the species name: nothing to change while that is shown */
    ASSERT_TRUE(gen3_edit_set_nickname(&ed, ""));
    gen3_pkm_nickname(&ed.pkm, buf, sizeof(buf));
    ASSERT_STR_EQ(buf, "TREECKO");
    before = ed;
    ASSERT_TRUE(gen3_edit_set_nickname(&ed, ""));
    ASSERT_TRUE(gen3_edit_set_nickname(&ed, NULL));
    ASSERT_TRUE(gen3_edit_set_nickname(&ed, "TREECKO"));
    ASSERT_TRUE(editor_eq(&ed, &before));

    /* The level is compared once clamped */
    gen3_edit_set_level(&ed, 100);
    before = ed;
    gen3_edit_set_level(&ed, 101);
    gen3_edit_set_level(&ed, 255);
    ASSERT_TRUE(editor_eq(&ed, &before));
    gen3_edit_set_level(&ed, 1);
    before = ed;
    gen3_edit_set_level(&ed, 0);
    ASSERT_TRUE(editor_eq(&ed, &before));
}

TEST(test_edit_set_move_pp) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_pkm_set(&ed.pkm, Gen3FieldPpBonuses, 0xFF);
    ASSERT_TRUE(gen3_edit_set_move(&ed, 2, 76));
    ASSERT_EQ(G(Move3), 76);
    ASSERT_EQ(G(Pp3), 10);
    ASSERT_EQ(G(PpBonuses), 0xCF);
    ASSERT_TRUE(moves_are(&ed, 33, 45, 76, 0));
    ASSERT_TRUE(pp_are(&ed, 35, 40, 10, 0));
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_set_move_packs) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_pkm_set(&ed.pkm, Gen3FieldPpBonuses, 0x09); /* slot 0: 1 PP Up, slot 1: 2 */
    ASSERT_TRUE(gen3_edit_set_move(&ed, 0, 0));
    ASSERT_TRUE(moves_are(&ed, 45, 0, 0, 0));
    ASSERT_EQ(G(Pp1), 40);
    ASSERT_EQ(G(PpBonuses), 0x02); /* Growl's PP Ups moved with it */
    ASSERT_TRUE(edit_consistent(&ed));
    ASSERT_TRUE(gen3_edit_set_move(&ed, 3, 22));
    ASSERT_TRUE(moves_are(&ed, 45, 22, 0, 0));
    ASSERT_TRUE(pp_are(&ed, 40, 10, 0, 0));
    ASSERT_EQ(G(PpBonuses), 0x02);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_set_move_rejects) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    Gen3Pkm before = ed.pkm;
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 1, 33));
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 3, 45));
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 4, 22));
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 2, GEN3_MOVE_COUNT));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(gen3_edit_set_move(&ed, 1, 0));
    ASSERT_TRUE(moves_are(&ed, 33, 0, 0, 0));
    before = ed.pkm;
    ASSERT_TRUE(!gen3_edit_set_move(&ed, 0, 0));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_default_moves) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_level(&ed, 30);
    ASSERT_TRUE(gen3_edit_set_move(&ed, 0, 76));
    ASSERT_TRUE(gen3_edit_set_move(&ed, 1, 0));
    gen3_pkm_set(&ed.pkm, Gen3FieldPpBonuses, 0xFF);
    gen3_edit_default_moves(&ed);
    uint16_t want[4];
    gen3_default_moves(1, 30, want);
    ASSERT_TRUE(want[3] != 0); /* four moves at level 30 */
    for(int s = 0; s < 4; s++) {
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldMove1 + s), want[s]);
        ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldPp1 + s), gen3_move_pp(want[s]));
    }
    ASSERT_EQ(G(PpBonuses), 0);
    ASSERT_TRUE(edit_consistent(&ed));
}

static bool traits_match(const Gen3Traits* got, const Gen3Traits* want, bool unown) {
    return got->nature == want->nature && got->gender == want->gender &&
           got->shiny == want->shiny && got->ability_slot == want->ability_slot &&
           (!unown || got->unown_letter == want->unown_letter);
}

TEST(test_edit_traits_matrix) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 280); /* Ralts: gender ratio 127, two abilities */
    for(uint8_t nature = 0; nature < 25; nature++)
        for(int g = 0; g < 2; g++)
            for(int shiny = 0; shiny < 2; shiny++)
                for(uint8_t slot = 0; slot < 2; slot++) {
                    Gen3Traits want = {
                        nature, g ? Gen3GenderFemale : Gen3GenderMale, shiny != 0, slot, 0};
                    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
                    Gen3Traits got;
                    gen3_traits_of(&ed.pkm, &got);
                    ASSERT_TRUE(traits_match(&got, &want, false));
                    ASSERT_EQ(G(AbilityBit), G(Pid) & 1);
                    ASSERT_TRUE(edit_consistent(&ed));
                }
}

TEST(test_edit_traits_static_gender) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 29); /* female only */
    Gen3Traits want;
    gen3_traits_of(&ed.pkm, &want);
    want.gender = Gen3GenderMale;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    Gen3Traits got;
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_EQ(got.gender, Gen3GenderFemale);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_ability_bit_single_ability) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7); /* Bulbasaur: one ability */
    bool odd = false;
    for(int i = 0; i < 100; i++) {
        Gen3Gender gender = (i / 25) & 1 ? Gen3GenderFemale : Gen3GenderMale;
        Gen3Traits want = {i % 25, gender, false, i & 1, 0}; /* slot 1 asked half the time */
        ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
        ASSERT_EQ(G(AbilityBit), 0);
        ASSERT_EQ(G(Pid) % 25, i % 25);
        if(G(Pid) & 1) odd = true;
        ASSERT_TRUE(edit_consistent(&ed));
    }
    ASSERT_TRUE(odd);
}

TEST(test_edit_unown_letters) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 201);
    for(uint8_t letter = 0; letter < 28; letter++) {
        Gen3Traits want;
        gen3_traits_of(&ed.pkm, &want);
        want.unown_letter = letter;
        ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
        ASSERT_EQ(gen3_unown_letter(G(Pid)), letter);
        Gen3Traits got;
        gen3_traits_of(&ed.pkm, &got);
        ASSERT_TRUE(traits_match(&got, &want, true));
        ASSERT_TRUE(edit_consistent(&ed));
    }
}

/* Asking for the traits it already has (after normalising the want as gen3_pid_find does)
 * keeps all 100 bytes: no new PID, no recalculation. */
TEST(test_edit_traits_same_keeps_pid) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7); /* Bulbasaur: gendered, one ability, not Unown */
    Gen3Pkm before = ed.pkm;
    Gen3Traits want;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    /* Ignored: the slot of a one-ability species, the letter of a non-Unown */
    want.ability_slot = !want.ability_slot;
    want.unown_letter = (want.unown_letter + 1) % GEN3_UNOWN_LETTER_COUNT;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    /* An out-of-range want is still refused, untouched */
    want.nature = GEN3_NATURE_COUNT;
    ASSERT_TRUE(!gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    /* The gender of a gendered species is not ignored: the other one is a real change */
    gen3_traits_of(&ed.pkm, &want);
    want.gender = want.gender == Gen3GenderMale ? Gen3GenderFemale : Gen3GenderMale;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_TRUE(G(Pid) != gen3_pkm_get(&before, Gen3FieldPid));
    Gen3Traits got;
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_EQ(got.gender, want.gender);
    ASSERT_EQ(got.nature, want.nature);
    ASSERT_TRUE(edit_consistent(&ed));

    /* Ignored: the gender of a female-only species */
    gen3_edit_set_species(&ed, 29);
    before = ed.pkm;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_EQ(want.gender, Gen3GenderFemale);
    want.gender = Gen3GenderMale;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);

    /* Unown (genderless): the gender is ignored, the letter is not */
    gen3_edit_set_species(&ed, GEN3_NATIONAL_UNOWN);
    before = ed.pkm;
    gen3_traits_of(&ed.pkm, &want);
    want.gender = Gen3GenderMale;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    want.unown_letter = (want.unown_letter + 1) % GEN3_UNOWN_LETTER_COUNT;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_TRUE(G(Pid) != gen3_pkm_get(&before, Gen3FieldPid));
    ASSERT_EQ(gen3_unown_letter(G(Pid)), want.unown_letter);
    ASSERT_TRUE(edit_consistent(&ed));

    /* A loaded two-ability Pokemon whose stored slot isn't pid & 1, with HP below MaxHp: the
     * same traits keep the PID, the slot and the HP. */
    gen3_edit_set_species(&ed, 280); /* Ralts */
    Gen3Pkm in = ed.pkm;
    uint32_t bit = !(gen3_pkm_get(&in, Gen3FieldPid) & 1);
    gen3_pkm_set(&in, Gen3FieldAbilityBit, bit);
    gen3_pkm_set(&in, Gen3FieldHp, 1);
    gen3_pkm_fix_checksum(&in);
    gen3_edit_load(&ed, &in);
    ASSERT_EQ(G(AbilityBit), bit);
    ASSERT_EQ(G(Hp), 1);
    before = ed.pkm;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_EQ(want.ability_slot, bit);
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent_hp(&ed, false));
    /* The other slot is a real change */
    want.ability_slot = !bit;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_EQ(G(AbilityBit), !bit);
    ASSERT_EQ(G(AbilityBit), G(Pid) & 1);
    ASSERT_TRUE(edit_consistent(&ed));

    /* An unknown species is still refused, untouched. gen3_edit_load refuses it, so it is put in
     * the editor directly. */
    in = ed.pkm;
    gen3_pkm_set(&in, Gen3FieldSpecies, 260); /* a "?" placeholder */
    ASSERT_TRUE(!gen3_edit_load(&ed, &in));
    ed.pkm = in;
    before = ed.pkm;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_TRUE(!gen3_edit_set_traits(&ed, &want));
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
}

TEST(test_edit_traits_preserve_other_bytes) {
    Gen3Pkm in;
    golden_pkm(&in);
    gen3_pkm_set(&in, Gen3FieldRibbons, 0x80ABCDEF);
    gen3_pkm_set(&in, Gen3FieldMarkings, 0x0F);
    for(int c = 0; c < 6; c++) gen3_pkm_set(&in, Gen3FieldCoolness + c, c + 1);
    gen3_pkm_set(&in, Gen3FieldPpBonuses, 0xE4);
    gen3_pkm_set(&in, Gen3FieldPokerus, 0x41);
    gen3_pkm_set(&in, Gen3FieldMetLocation, 0x10);
    gen3_pkm_set(&in, Gen3FieldMetLevel, 3);
    gen3_pkm_set(&in, Gen3FieldMetGame, 1);
    gen3_pkm_set(&in, Gen3FieldBall, 2);
    gen3_pkm_set(&in, Gen3FieldOtGender, 1);
    for(int s = 0; s < Gen3StatCount; s++) gen3_pkm_set(&in, Gen3FieldEvHp + s, 10 * (s + 1));
    gen3_pkm_set(&in, Gen3FieldHeldItem, 200);
    gen3_pkm_set(&in, Gen3FieldGrowthUnknown, 0xBEEF);
    gen3_pkm_set(&in, Gen3FieldFiller, 0x1234);
    gen3_pkm_fix_checksum(&in);

    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_load(&ed, &in);
    Gen3Pkm before = ed.pkm;
    Gen3Traits want;
    gen3_traits_of(&ed.pkm, &want);
    ASSERT_TRUE(want.nature != 4);
    want.nature = 4;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_EQ(G(Pid) % 25, 4);
    /* The new PID re-orders the substructures (seed 7 guarantees it), so encrypt must use it. */
    ASSERT_TRUE(G(Pid) % 24 != gen3_pkm_get(&before, Gen3FieldPid) % 24);
    for(int i = 0; i < GEN3_PARTY_SIZE; i++) {
        if(i <= 0x03 || i == 0x1C || i == 0x1D || (i >= 0x56 && i <= 0x63)) continue;
        if(i == 0x4B) {
            ASSERT_EQ(ed.pkm.raw[i] & 0x7F, before.raw[i] & 0x7F);
            continue;
        }
        if(ed.pkm.raw[i] != before.raw[i]) printf("  byte 0x%02X changed\n", i);
        ASSERT_EQ(ed.pkm.raw[i], before.raw[i]);
    }
    uint8_t game[GEN3_PARTY_SIZE];
    gen3_pkm_encrypt(&ed.pkm, game);
    Gen3Pkm back;
    ASSERT_TRUE(gen3_pkm_decrypt(game, sizeof(game), &back));
    ASSERT_MEM_EQ(back.raw, ed.pkm.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_ot_id_keeps_shiny) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 280); /* Ralts: gendered, two abilities */
    Gen3Traits want = {10, Gen3GenderFemale, true, 1, 0};
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 12345, 54321), Gen3OtIdOk);
    ASSERT_EQ(G(OtId), (54321u << 16) | 12345u);
    Gen3Traits got;
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_TRUE(traits_match(&got, &want, false));
    ASSERT_TRUE(edit_consistent(&ed));

    want.shiny = false;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 22222, 0), Gen3OtIdOk);
    ASSERT_EQ(G(OtId), 22222);
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_TRUE(traits_match(&got, &want, false));
    ASSERT_TRUE(edit_consistent(&ed));

    /* No change in shininess: the PID stays. */
    uint32_t pid = G(Pid);
    ASSERT_TRUE(!gen3_is_shiny((7u << 16) | 7u, pid));
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 7, 7), Gen3OtIdOk);
    ASSERT_EQ(G(Pid), pid);
    ASSERT_TRUE(edit_consistent(&ed));
}

/* Unown HARDY/P has shiny PIDs for OT 22222/0 but none for 64893/0 (test_unown_shiny_possible).
 * Asking for it there is refused at once and changes nothing. */
TEST(test_edit_unown_shiny_impossible) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(gen3_edit_set_species(&ed, GEN3_NATIONAL_UNOWN));
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 64893, 0), Gen3OtIdOk);
    Gen3Traits want = {0 /* HARDY */, Gen3GenderNone, false, 0, 15 /* P */};
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    Gen3Editor before = ed;
    want.shiny = true;
    ASSERT_TRUE(!gen3_edit_set_traits(&ed, &want));
    ASSERT_TRUE(editor_eq(&ed, &before)); /* the RNG too: no PID was tried */
    ASSERT_TRUE(edit_consistent(&ed));
    /* The same from a shiny Unown of another letter: the letter can't change */
    want.unown_letter = 0;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    before = ed;
    want.unown_letter = 15;
    ASSERT_TRUE(!gen3_edit_set_traits(&ed, &want));
    ASSERT_TRUE(editor_eq(&ed, &before));
}

/* A shiny HARDY/P Unown under 22222/0 gets OT TID 64893, where it can't be shiny: the OT ID is
 * applied anyway, and the PID is kept: it isn't shiny for the new OT, and it keeps the nature
 * and letter. */
TEST(test_edit_ot_id_shiny_lost) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(gen3_edit_set_species(&ed, GEN3_NATIONAL_UNOWN));
    Gen3Traits want = {0, Gen3GenderNone, true, 0, 15};
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    Gen3Pkm before = ed.pkm;
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 64893, 0), Gen3OtIdShinyLost);
    ASSERT_EQ(G(OtId), 64893);
    Gen3Traits got;
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_TRUE(!got.shiny);
    ASSERT_EQ(got.nature, 0);
    ASSERT_EQ(got.unown_letter, 15);
    /* Only the OT ID changed (it is outside the checksummed bytes) */
    for(int i = 0; i < GEN3_PARTY_SIZE; i++)
        if(i < 4 || i > 7) ASSERT_EQ(ed.pkm.raw[i], before.raw[i]);
    ASSERT_TRUE(edit_consistent(&ed));

    /* Back to 22222/0, where that PID is shiny: not shiny now, so a new non-shiny PID */
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 22222, 0), Gen3OtIdOk);
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_TRUE(!got.shiny);
    ASSERT_EQ(got.nature, 0);
    ASSERT_EQ(got.unown_letter, 15);
    ASSERT_TRUE(G(Pid) != gen3_pkm_get(&before, Gen3FieldPid));
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_ball_item_pokerus) {
    Gen3Editor ed;
    char name[GEN3_ITEM_NAME_LEN + 1];
    gen3_edit_init(&ed, 7);
    ASSERT_EQ(GEN3_EDIT_BALL_MAX, 12);
    ASSERT_TRUE(gen3_item_name_get(GEN3_EDIT_BALL_MAX, name));
    ASSERT_STR_EQ(name, "PREMIER BALL");
    gen3_edit_set_ball(&ed, GEN3_EDIT_BALL_MAX);
    ASSERT_EQ(G(Ball), GEN3_EDIT_BALL_MAX);
    gen3_edit_set_ball(&ed, 0);
    gen3_edit_set_ball(&ed, GEN3_EDIT_BALL_MAX + 1);
    ASSERT_EQ(G(Ball), GEN3_EDIT_BALL_MAX);
    gen3_edit_set_item(&ed, 200);
    ASSERT_EQ(G(HeldItem), 200);
    /* Refused: mail (121..132), a key item, an unused id, out of range. */
    static const uint16_t refused[] = {121, 132, 259, 120, GEN3_ITEM_COUNT};
    for(unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        ASSERT_TRUE(!gen3_item_holdable(refused[i]));
        gen3_edit_set_item(&ed, refused[i]);
        ASSERT_EQ(G(HeldItem), 200);
    }
    gen3_edit_set_item(&ed, 0);
    ASSERT_EQ(G(HeldItem), 0);
    gen3_edit_set_pokerus(&ed, 0x41);
    ASSERT_EQ(G(Pokerus), 0x41);
    ASSERT_STR_EQ(gen3_pokerus_status(0x41), "Infected");
    ASSERT_STR_EQ(gen3_pokerus_status(0x40), "Cured");
    ASSERT_STR_EQ(gen3_pokerus_status(0), "Clean");
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_load_keeps_bytes) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    Gen3Pkm in;
    golden_pkm(&in);
    gen3_edit_load(&ed, &in);
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!ed.preset_applied);
    ASSERT_TRUE(edit_consistent(&ed));

    /* Unmodelled bytes are kept verbatim; the party stats are recomputed (Atk 999 -> 12). */
    in.raw[0x13] |= 0xF0; /* flags byte, unused bits */
    gen3_pkm_set(&in, Gen3FieldLanguage, 3);
    gen3_pkm_set(&in, Gen3FieldMarkings, 0x0F);
    gen3_pkm_set(&in, Gen3FieldFiller, 0x1234);
    gen3_pkm_set(&in, Gen3FieldGrowthUnknown, 0xBEEF);
    for(int c = 0; c < 6; c++) gen3_pkm_set(&in, Gen3FieldCoolness + c, c + 1);
    gen3_pkm_set(&in, Gen3FieldRibbons, 0xFFFFFFFF);
    gen3_pkm_set(&in, Gen3FieldMetLocation, 0x10);
    gen3_pkm_set(&in, Gen3FieldMetLevel, 3);
    gen3_pkm_set(&in, Gen3FieldMetGame, 1);
    gen3_pkm_set(&in, Gen3FieldBall, 2);
    gen3_pkm_set(&in, Gen3FieldOtGender, 1);
    gen3_pkm_set(&in, Gen3FieldPokerus, 0x41);
    gen3_pkm_set(&in, Gen3FieldPpBonuses, 0xE4);
    gen3_pkm_set(&in, Gen3FieldFriendship, 123);
    gen3_pkm_set(&in, Gen3FieldHeldItem, 200);
    gen3_pkm_set(&in, Gen3FieldStatus, 0x08);
    gen3_pkm_set(&in, Gen3FieldAtk, 999);
    gen3_pkm_fix_checksum(&in);
    gen3_edit_load(&ed, &in);
    Gen3Pkm want = in;
    uint16_t st[Gen3StatCount];
    gen3_calc_stats(&in, st);
    ASSERT_EQ(st[Gen3StatAtk], 12);
    gen3_pkm_set(&want, Gen3FieldAtk, st[Gen3StatAtk]);
    ASSERT_MEM_EQ(ed.pkm.raw, want.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_load_normalises) {
    Gen3Pkm in;
    golden_pkm(&in);
    /* Golden moves are {1, 43, 0, 0}: add a gap, a duplicate and PP Ups (1, 0, 2, 3). */
    static const uint16_t mv[4] = {1, 0, 1, 43};
    static const uint8_t pp[4] = {35, 0, 20, 30};
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(&in, Gen3FieldMove1 + s, mv[s]);
        gen3_pkm_set(&in, Gen3FieldPp1 + s, pp[s]);
    }
    gen3_pkm_set(&in, Gen3FieldPpBonuses, 0xE1);
    gen3_pkm_set(&in, Gen3FieldAbilityBit, 1); /* Treecko has one ability */
    gen3_pkm_set(&in, Gen3FieldHp, 999);
    gen3_pkm_set(&in, Gen3FieldLevel, 77);
    gen3_pkm_fix_checksum(&in);

    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_load(&ed, &in);
    /* The duplicate is dropped with its PP and PP Ups; Leer carries its PP and 3 PP Ups. */
    Gen3Pkm want;
    golden_pkm(&want);
    gen3_pkm_set(&want, Gen3FieldPpBonuses, 0x0D);
    gen3_pkm_fix_checksum(&want);
    ASSERT_MEM_EQ(ed.pkm.raw, want.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_load_clamps_exp) {
    static const uint32_t exp_in[3] = {0, 0xFFFFFFFF, 2000000};
    static const uint32_t exp_out[3] = {1, 1059860, 1059860};
    static const uint8_t level_out[3] = {1, 100, 100};
    for(int i = 0; i < 3; i++) {
        Gen3Pkm in;
        golden_pkm(&in);
        gen3_pkm_set(&in, Gen3FieldExp, exp_in[i]);
        gen3_pkm_fix_checksum(&in);
        Gen3Editor ed;
        gen3_edit_init(&ed, 7);
        gen3_edit_load(&ed, &in);
        ASSERT_EQ(G(Exp), exp_out[i]);
        ASSERT_EQ(G(Level), level_out[i]);
        /* The current HP (20) is kept, clamped to the new MaxHp (12 at level 1). */
        ASSERT_EQ(G(Hp), G(MaxHp) < 20 ? G(MaxHp) : 20);
        ASSERT_TRUE(edit_consistent_hp(&ed, false));
    }
}

TEST(test_edit_load_keeps_current_hp) {
    Gen3Pkm in;
    golden_pkm(&in);
    gen3_pkm_set(&in, Gen3FieldHp, 7);
    gen3_pkm_set(&in, Gen3FieldStatus, 0x08);
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_load(&ed, &in);
    ASSERT_EQ(G(Hp), 7);
    ASSERT_EQ(G(MaxHp), 20);
    ASSERT_EQ(G(Status), 0x08);
    ASSERT_TRUE(edit_consistent_hp(&ed, false));
    gen3_edit_set_nickname(&ed, "LEAFY"); /* any edit recalculates: Hp = MaxHp */
    ASSERT_EQ(G(Hp), 20);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_two_ability_bit_kept) {
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 280); /* Ralts: Synchronize / Trace */
    Gen3Pkm in = ed.pkm;
    uint32_t bit = !(gen3_pkm_get(&in, Gen3FieldPid) & 1); /* stored slot != pid & 1 */
    gen3_pkm_set(&in, Gen3FieldAbilityBit, bit);
    gen3_pkm_fix_checksum(&in);
    gen3_edit_load(&ed, &in);
    ASSERT_EQ(G(AbilityBit), bit);
    gen3_edit_set_nickname(&ed, "LEAFY");
    ASSERT_EQ(G(AbilityBit), bit);
    gen3_edit_set_item(&ed, 200);
    ASSERT_EQ(G(AbilityBit), bit);
    gen3_edit_set_level(&ed, 30);
    ASSERT_EQ(G(AbilityBit), bit);
    gen3_edit_set_ball(&ed, 2);
    ASSERT_EQ(G(AbilityBit), bit);
    ASSERT_TRUE(gen3_edit_set_move(&ed, 3, 0));
    ASSERT_EQ(G(AbilityBit), bit);
    /* set_ot_id without a shininess flip keeps the PID and the slot. */
    uint32_t pid = G(Pid);
    ASSERT_TRUE(!gen3_is_shiny((7u << 16) | 7u, pid));
    gen3_edit_set_ot_id(&ed, 7, 7);
    ASSERT_EQ(G(Pid), pid);
    ASSERT_EQ(G(AbilityBit), bit);
    ASSERT_TRUE(edit_consistent(&ed));
    /* With a flip (the old PID is shiny for this OT) it finds a new PID for the same slot. */
    uint16_t tid = (pid >> 16) ^ (pid & 0xFFFF);
    gen3_edit_set_ot_id(&ed, tid, 0);
    ASSERT_TRUE(G(Pid) != pid);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    ASSERT_EQ(G(AbilityBit), bit);
    ASSERT_TRUE(edit_consistent(&ed));
}

static bool info_is(const Gen3NormaliseInfo* i, bool md, bool dr, bool gp, bool ec, bool af) {
    return i->moves_defaulted == md && i->duplicates_removed == dr && i->gaps_packed == gp &&
           i->exp_clamped == ec && i->ability_fixed == af && !i->mail_item_removed &&
           !i->mail_cleared;
}

/* Only the mail flags given are set */
static bool info_mail_is(const Gen3NormaliseInfo* i, bool item_removed, bool cleared) {
    return !i->moves_defaulted && !i->duplicates_removed && !i->gaps_packed && !i->exp_clamped &&
           !i->ability_fixed && i->mail_item_removed == item_removed && i->mail_cleared == cleared;
}

static void set_moves(Gen3Pkm* p, uint16_t a, uint16_t b, uint16_t c, uint16_t d) {
    const uint16_t mv[4] = {a, b, c, d};
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(p, Gen3FieldMove1 + s, mv[s]);
        gen3_pkm_set(p, Gen3FieldPp1 + s, mv[s] ? gen3_move_pp(mv[s]) : 0);
    }
}

TEST(test_normalise_flags) {
    Gen3Pkm p;
    Gen3NormaliseInfo ni;
    golden_pkm(&p);
    gen3_pkm_set(&p, Gen3FieldHp, 999); /* party stats are fixed silently */
    gen3_pkm_set(&p, Gen3FieldLevel, 77);
    memset(&ni, 0xFF, sizeof(ni));
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, false, false, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    set_moves(&p, 1, 0, 43, 0);
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, false, true, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    set_moves(&p, 1, 43, 1, 0); /* the dropped duplicate leaves no gap before a move */
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, true, false, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    set_moves(&p, 1, 1, 43, 0); /* gaps_packed reports the input, which had no gap */
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, true, false, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    set_moves(&p, 0, 0, 0, 0);
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, true, false, false, false, false));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_pkm(&p);
    gen3_pkm_set(&p, Gen3FieldExp, 0);
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, false, false, true, false));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldExp), 1);

    golden_pkm(&p);
    gen3_pkm_set(&p, Gen3FieldAbilityBit, 1); /* Treecko has one ability */
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, false, false, false, true));
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    /* A two-ability species keeps its stored slot, even when it isn't pid & 1. */
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_set_species(&ed, 280);
    p = ed.pkm;
    uint32_t bit = !(gen3_pkm_get(&p, Gen3FieldPid) & 1);
    gen3_pkm_set(&p, Gen3FieldAbilityBit, bit);
    ASSERT_TRUE(gen3_normalise(&p, NULL));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldAbilityBit), bit);
    ASSERT_EQ(gen3_pkm_checksum(&p), gen3_pkm_get(&p, Gen3FieldChecksum));

    /* Unknown species (0, a "?" placeholder 260, past the table 412): false, untouched. */
    static const uint16_t bad[3] = {0, 260, 412};
    for(int i = 0; i < 3; i++) {
        golden_pkm(&p);
        gen3_pkm_set(&p, Gen3FieldSpecies, bad[i]);
        set_moves(&p, 1, 0, 1, 43);
        Gen3Pkm before = p;
        memset(&ni, 0xFF, sizeof(ni));
        ASSERT_TRUE(!gen3_normalise(&p, &ni));
        ASSERT_TRUE(info_is(&ni, false, false, false, false, false));
        ASSERT_MEM_EQ(p.raw, before.raw, GEN3_PARTY_SIZE);
    }
}

/* No mail record exists behind the editor's Pokemon, so the mail index always comes out 0xFF.
 * Ruby's trade code uses the partner's index without looking at the held item: 0..5 gives the
 * received Pokemon that mail (and its item), 6..254 reads past the mail array. */
TEST(test_normalise_mail) {
    static const uint8_t stale[] = {0, 3, 5, 6, 0x55, 0xFE};
    for(size_t i = 0; i < sizeof(stale); i++) {
        Gen3Pkm p;
        Gen3NormaliseInfo ni;
        golden_pkm(&p);
        gen3_pkm_set(&p, Gen3FieldMail, stale[i]);
        memset(&ni, 0xFF, sizeof(ni));
        ASSERT_TRUE(gen3_normalise(&p, &ni));
        ASSERT_TRUE(info_mail_is(&ni, false, true));
        ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    }
    /* Already 0xFF: nothing to report */
    Gen3Pkm p;
    Gen3NormaliseInfo ni;
    golden_pkm(&p);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldMail), GEN3_MAIL_NONE);
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_mail_is(&ni, false, false));

    /* A held Mail item has no record behind it either: the item goes. Other items stay. */
    for(uint16_t item = GEN3_ITEM_MAIL_FIRST; item <= GEN3_ITEM_MAIL_LAST; item++) {
        golden_pkm(&p);
        gen3_pkm_set(&p, Gen3FieldHeldItem, item);
        ASSERT_TRUE(gen3_normalise(&p, &ni));
        ASSERT_TRUE(info_mail_is(&ni, true, false));
        ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
        golden_pkm(&p);
        gen3_pkm_set(&p, Gen3FieldHeldItem, item);
        gen3_pkm_set(&p, Gen3FieldMail, 2);
        ASSERT_TRUE(gen3_normalise(&p, &ni));
        ASSERT_TRUE(info_mail_is(&ni, true, true));
        ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    }
    static const uint16_t keep[] = {GEN3_ITEM_MAIL_FIRST - 1, GEN3_ITEM_MAIL_LAST + 1, 200};
    for(size_t i = 0; i < sizeof(keep) / sizeof(keep[0]); i++) {
        golden_pkm(&p);
        gen3_pkm_set(&p, Gen3FieldHeldItem, keep[i]);
        ASSERT_TRUE(gen3_normalise(&p, &ni));
        ASSERT_TRUE(info_mail_is(&ni, false, false));
        ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldHeldItem), keep[i]);
    }

    /* The same through gen3_edit_load: the stale index of a loaded Pokemon is cleared */
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    golden_pkm(&p);
    gen3_pkm_set(&p, Gen3FieldMail, 0x55);
    gen3_pkm_set(&p, Gen3FieldHeldItem, GEN3_ITEM_MAIL_FIRST);
    gen3_pkm_fix_checksum(&p);
    ASSERT_TRUE(gen3_edit_load(&ed, &p));
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

/* HasSpecies is set without being reported, as party stats are recalculated. */
TEST(test_normalise_has_species) {
    Gen3Pkm p;
    Gen3NormaliseInfo ni;
    golden_pkm(&p);
    gen3_pkm_set(&p, Gen3FieldHasSpecies, 0);
    ASSERT_TRUE(gen3_normalise(&p, &ni));
    ASSERT_TRUE(info_is(&ni, false, false, false, false, false));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldHasSpecies), 1);
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
}

/* gen3_edit_load takes only what gen3_pkm_check accepts. Anything else leaves the editor as it
 * was. */
TEST(test_edit_load_validates) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(gen3_edit_set_nickname(&ed, "LEAFY"));
    Gen3Editor before = ed;
    static const struct {
        Gen3Field field;
        uint32_t value;
    } bad[] = {
        {Gen3FieldIsEgg, 1},
        {Gen3FieldEggFlag, 1},
        {Gen3FieldBadEgg, 1},
        {Gen3FieldMove1, GEN3_MOVE_COUNT},
        {Gen3FieldMove2, GEN3_MOVE_COUNT},
        {Gen3FieldMove4, 0xFFFF},
        {Gen3FieldHeldItem, GEN3_ITEM_COUNT},
        {Gen3FieldHeldItem, 0xFFFF},
        {Gen3FieldSpecies, 0},
        {Gen3FieldSpecies, 260},
    };
    for(size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        Gen3Pkm in;
        golden_pkm(&in);
        gen3_pkm_set(&in, bad[i].field, bad[i].value);
        gen3_pkm_fix_checksum(&in);
        ASSERT_TRUE(!gen3_edit_load(&ed, &in));
        ASSERT_TRUE(editor_eq(&ed, &before));
    }

    /* HasSpecies 0 is accepted and comes out set */
    Gen3Pkm in;
    golden_pkm(&in);
    gen3_pkm_set(&in, Gen3FieldHasSpecies, 0);
    gen3_pkm_fix_checksum(&in);
    ASSERT_TRUE(gen3_edit_load(&ed, &in));
    ASSERT_EQ(G(HasSpecies), 1);
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!ed.preset_applied);
    ASSERT_TRUE(edit_consistent(&ed));
}

TEST(test_edit_load_no_moves) {
    Gen3Pkm in;
    golden_pkm(&in);
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(&in, Gen3FieldMove1 + s, 0);
        gen3_pkm_set(&in, Gen3FieldPp1 + s, 0);
    }
    gen3_pkm_set(&in, Gen3FieldPpBonuses, 0xFF);
    gen3_pkm_fix_checksum(&in);
    Gen3Editor ed;
    gen3_edit_init(&ed, 7);
    gen3_edit_load(&ed, &in);
    /* Treecko L5's default moveset with full PP is the golden {1, 43} / {35, 30}. */
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_TRUE(edit_consistent(&ed));
}

/* No bulk data: mode 0, every read fails after a good open (an SD error); mode 1, no source. */
static void no_data(int mode) {
    if(mode == 0)
        bulk_file_failing = true;
    else
        gen3_bulk_close();
}

/* Each edit that needs the bulk file refuses, and the Pokemon stays as it was, game-valid. */
TEST(test_edit_no_data_keeps_pokemon) {
    for(int mode = 0; mode < 2; mode++) {
        Gen3Editor ed;
        ASSERT_TRUE(bulk_file_open());
        ASSERT_TRUE(gen3_edit_init(&ed, 7));
        ASSERT_TRUE(gen3_edit_set_move(&ed, 0, 85)); /* not a default move */
        ASSERT_TRUE(gen3_edit_set_nickname(&ed, "LEAFY"));
        Gen3Pkm before = ed.pkm;
        no_data(mode);

        ASSERT_TRUE(!gen3_edit_set_species(&ed, 252));
        ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
        ASSERT_TRUE(edit_consistent(&ed));
        ASSERT_TRUE(gen3_edit_set_species(&ed, 1)); /* already Bulbasaur: nothing to read */
        ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);

        ASSERT_TRUE(!gen3_edit_default_moves(&ed));
        ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
        ASSERT_TRUE(edit_consistent(&ed));

        ASSERT_TRUE(!gen3_edit_set_nickname(&ed, ""));
        ASSERT_TRUE(!gen3_edit_set_nickname(&ed, NULL));
        ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
        ASSERT_TRUE(edit_consistent(&ed));

        /* An edit that needs no bulk data still works */
        ASSERT_TRUE(gen3_edit_set_nickname(&ed, "BOB"));
        ASSERT_TRUE(edit_consistent(&ed));

        Gen3Editor fresh;
        ASSERT_TRUE(!gen3_edit_init(&fresh, 7));
    }
    ASSERT_TRUE(bulk_file_open());
}

/* A moveless Pokemon needs the default moveset: without the bulk data, normalise and load refuse
 * it and change nothing. */
TEST(test_edit_no_data_load) {
    Gen3Pkm moveless;
    golden_pkm(&moveless);
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(&moveless, Gen3FieldMove1 + s, 0);
        gen3_pkm_set(&moveless, Gen3FieldPp1 + s, 0);
    }
    gen3_pkm_fix_checksum(&moveless);
    for(int mode = 0; mode < 2; mode++) {
        Gen3Editor ed;
        Gen3NormaliseInfo ni;
        ASSERT_TRUE(bulk_file_open());
        ASSERT_TRUE(gen3_edit_init(&ed, 7));
        Gen3Pkm before = ed.pkm;
        no_data(mode);

        Gen3Pkm p = moveless;
        memset(&ni, 0xFF, sizeof(ni));
        ASSERT_TRUE(!gen3_normalise(&p, &ni));
        ASSERT_MEM_EQ(p.raw, moveless.raw, GEN3_PARTY_SIZE);
        ASSERT_TRUE(info_is(&ni, false, false, false, false, false));

        ASSERT_TRUE(!gen3_edit_load(&ed, &moveless));
        ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
        ASSERT_TRUE(edit_consistent(&ed));

        /* An unknown species is refused the same way */
        p = moveless;
        set_moves(&p, 1, 43, 0, 0);
        gen3_pkm_set(&p, Gen3FieldSpecies, 260);
        ASSERT_TRUE(!gen3_edit_load(&ed, &p));
        ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);

        /* A Pokemon with a move needs no bulk data */
        golden_pkm(&p);
        ASSERT_TRUE(gen3_edit_load(&ed, &p));
        ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    }
    ASSERT_TRUE(bulk_file_open());
}

/* D7: the untouched initial default takes the GBA player's OT at once, and stays the default */
TEST(test_edit_trainer_pristine) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(ed.pristine);
    ASSERT_TRUE(!ed.has_trainer);
    ASSERT_TRUE(gen3_edit_trainer(&ed) == NULL);
    ASSERT_TRUE(!gen3_edit_ot_is_trainer(&ed));
    Gen3Pkm before = ed.pkm;
    Gen3Trainer t;
    ruby_trainer(&t);
    gen3_edit_set_trainer(&ed, &t);
    memset(&t, 0, sizeof(t)); /* the editor keeps its own copy */
    ASSERT_TRUE(ot_bytes_are(&ed, RUBY_NAME));
    ASSERT_EQ(G(OtId), RUBY_OT_ID);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    ASSERT_EQ(gen3_edit_national(&ed), 1);
    ASSERT_EQ(G(Level), 5);
    ASSERT_TRUE(ed.pristine);
    ASSERT_TRUE(ed.has_trainer);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    const Gen3Trainer* got = gen3_edit_trainer(&ed);
    ASSERT_TRUE(got != NULL);
    ASSERT_MEM_EQ(got->name, RUBY_NAME, GEN3_OT_NAME_LEN);
    ASSERT_EQ(got->tid, 12345);
    ASSERT_EQ(got->sid, 54321);
    ASSERT_EQ(got->gender, 1);
    /* Only the OT changed: its ID, name and gender bit (0x47), and the checksum. The PID isn't
     * shiny for either OT, so it stays. */
    ASSERT_EQ(G(Pid), gen3_pkm_get(&before, Gen3FieldPid));
    for(int i = 0; i < GEN3_PARTY_SIZE; i++) {
        bool ot = (i >= 0x04 && i <= 0x07) || (i >= 0x14 && i <= 0x1A) || i == 0x1C || i == 0x1D ||
                  i == 0x47;
        if(!ot) ASSERT_EQ(ed.pkm.raw[i], before.raw[i]);
    }
    ASSERT_EQ(G(Ball), gen3_pkm_get(&before, Gen3FieldBall));
    ASSERT_EQ(G(MetGame), gen3_pkm_get(&before, Gen3FieldMetGame));
    ASSERT_TRUE(edit_consistent(&ed));
}

/* The default's PID is shiny under the player's OT (sid = pid_hi ^ pid_lo ^ tid). set_trainer
 * finds a new PID with the same nature, gender and ability, and the editor stays pristine. */
TEST(test_edit_trainer_pristine_refinds_pid) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    uint32_t pid = G(Pid);
    Gen3Traits old;
    gen3_traits_of(&ed.pkm, &old);
    ASSERT_TRUE(!old.shiny);
    Gen3Trainer t;
    ruby_trainer(&t);
    t.sid = (uint16_t)((pid >> 16) ^ (pid & 0xFFFF) ^ t.tid);
    uint32_t ot_id = ((uint32_t)t.sid << 16) | t.tid;
    ASSERT_TRUE(gen3_is_shiny(ot_id, pid));
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_TRUE(ed.pristine);
    ASSERT_EQ(G(OtId), ot_id);
    ASSERT_TRUE(G(Pid) != pid);
    ASSERT_TRUE(!gen3_is_shiny(ot_id, G(Pid)));
    Gen3Traits got;
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_TRUE(!got.shiny);
    ASSERT_EQ(got.nature, old.nature);
    ASSERT_EQ(got.gender, old.gender);
    ASSERT_EQ(got.ability_slot, old.ability_slot);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    ASSERT_TRUE(edit_consistent(&ed));
}

/* Any non-zero gender is stored as 1, the OT gender bit's value, so the OT still reads as the
 * trainer's */
TEST(test_edit_trainer_gender_normalised) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    Gen3Trainer t;
    ruby_trainer(&t);
    t.gender = 5;
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_EQ(gen3_edit_trainer(&ed)->gender, 1);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    Gen3Editor before = ed;
    ASSERT_EQ(gen3_edit_use_trainer_ot(&ed), Gen3UseTrainerOk);
    ASSERT_TRUE(editor_eq(&ed, &before));

    /* An edited Pokemon: only remembered, normalised the same way */
    gen3_edit_set_level(&ed, 10);
    t.gender = 0x80;
    t.tid = 1;
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_EQ(gen3_edit_trainer(&ed)->gender, 1);
    ASSERT_EQ(gen3_edit_use_trainer_ot(&ed), Gen3UseTrainerOk);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_EQ(G(OtId), 0xD4310001u);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    t.gender = 0;
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_EQ(gen3_edit_trainer(&ed)->gender, 0);
    ASSERT_TRUE(edit_consistent(&ed));
}

/* An edited Pokemon keeps its OT: the player is only remembered */
TEST(test_edit_trainer_after_edit) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_edit_set_level(&ed, 10);
    ASSERT_TRUE(!ed.pristine);
    Gen3Pkm before = ed.pkm;
    uint32_t rng = ed.rng.state;
    Gen3Trainer t;
    ruby_trainer(&t);
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_EQ(ed.rng.state, rng);
    ASSERT_TRUE(ot_bytes_are(&ed, FLIPPER_NAME));
    ASSERT_EQ(G(OtId), 22222);
    ASSERT_EQ(G(OtGender), 0);
    ASSERT_TRUE(!gen3_edit_ot_is_trainer(&ed));
    ASSERT_TRUE(ed.has_trainer);
    ASSERT_TRUE(gen3_edit_trainer(&ed) != NULL);
    ASSERT_EQ(gen3_edit_trainer(&ed)->tid, 12345);
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(edit_consistent(&ed));
}

/* D7 amends D5: once the player is known, a new species is theirs, and its first non-shiny PID is
 * searched against their OT ID. Without a player the OT is kept
 * (test_edit_species_resets_keeps_ot). */
TEST(test_edit_species_takes_trainer) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_edit_set_level(&ed, 10);
    Gen3Trainer t;
    ruby_trainer(&t);
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_TRUE(gen3_edit_set_species(&ed, 252));
    ASSERT_EQ(gen3_edit_national(&ed), 252);
    ASSERT_EQ(G(Level), 10);
    ASSERT_TRUE(ot_bytes_are(&ed, RUBY_NAME));
    ASSERT_EQ(G(OtId), RUBY_OT_ID);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), G(Pid)));
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(edit_consistent(&ed));

    /* Another OT on the current Pokemon */
    gen3_edit_set_ot_name(&ed, "ASH");
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 1, 2), Gen3OtIdOk);
    ASSERT_TRUE(!gen3_edit_ot_is_trainer(&ed));

    /* Fails closed: without the bulk data nothing changes, the OT included */
    Gen3Editor before = ed;
    bulk_file_failing = true;
    ASSERT_TRUE(!gen3_edit_set_species(&ed, 4));
    ASSERT_TRUE(editor_eq(&ed, &before));
    ASSERT_TRUE(bulk_file_open());

    /* Make the next random PID shiny for the player's OT but not for the current one: the new
     * Pokemon must not take it. */
    Gen3Rng peek = ed.rng;
    uint32_t r = gen3_rng_next(&peek);
    t.sid = (uint16_t)((r >> 16) ^ (r & 0xFFFF) ^ t.tid);
    gen3_edit_set_trainer(&ed, &t);
    uint32_t ot_id = ((uint32_t)t.sid << 16) | t.tid;
    ASSERT_TRUE(gen3_is_shiny(ot_id, r));
    ASSERT_TRUE(!gen3_is_shiny(G(OtId), r));
    ASSERT_TRUE(gen3_edit_set_species(&ed, 4));
    ASSERT_EQ(gen3_edit_national(&ed), 4);
    ASSERT_TRUE(ot_bytes_are(&ed, RUBY_NAME));
    ASSERT_EQ(G(OtId), ot_id);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_TRUE(G(Pid) != r);
    ASSERT_TRUE(!gen3_is_shiny(ot_id, G(Pid)));
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    ASSERT_TRUE(edit_consistent(&ed));
}

/* "OT: use <NAME>": the player's OT on any Pokemon, shininess kept where it can be (plan 2 D6) */
TEST(test_edit_use_trainer_ot) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    /* No player yet: nothing changes */
    Gen3Editor before = ed;
    ASSERT_EQ(gen3_edit_use_trainer_ot(&ed), Gen3UseTrainerNoTrainer);
    ASSERT_TRUE(editor_eq(&ed, &before));
    ASSERT_TRUE(ed.pristine);

    Gen3Pkm in;
    golden_pkm(&in);
    ASSERT_TRUE(gen3_edit_load(&ed, &in));
    Gen3Traits old;
    gen3_traits_of(&ed.pkm, &old);
    old.shiny = true;
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &old));
    gen3_traits_of(&ed.pkm, &old);
    ASSERT_TRUE(old.shiny);
    Gen3Trainer t;
    ruby_trainer(&t);
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_TRUE(ot_bytes_are(&ed, FLIPPER_NAME)); /* an edited Pokemon: not applied yet */
    ASSERT_TRUE(!gen3_edit_ot_is_trainer(&ed));
    ASSERT_EQ(gen3_edit_use_trainer_ot(&ed), Gen3UseTrainerOk);
    ASSERT_TRUE(ot_bytes_are(&ed, RUBY_NAME));
    ASSERT_EQ(G(OtId), RUBY_OT_ID);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    Gen3Traits got;
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_TRUE(got.shiny);
    ASSERT_EQ(got.nature, old.nature);
    ASSERT_EQ(got.gender, old.gender);
    ASSERT_EQ(got.ability_slot, old.ability_slot);
    ASSERT_EQ(gen3_edit_national(&ed), 252);
    ASSERT_TRUE(edit_consistent(&ed));
    /* Already the player's: nothing changes */
    before = ed;
    ASSERT_EQ(gen3_edit_use_trainer_ot(&ed), Gen3UseTrainerOk);
    ASSERT_TRUE(editor_eq(&ed, &before));

    /* Unown HARDY/P is shiny for 22222/0 but can't be for 64893/0 (test_edit_ot_id_shiny_lost):
     * the OT is applied and the Pokemon is no longer shiny */
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(gen3_edit_set_species(&ed, GEN3_NATIONAL_UNOWN));
    Gen3Traits want = {0, Gen3GenderNone, true, 0, 15};
    ASSERT_TRUE(gen3_edit_set_traits(&ed, &want));
    t.tid = 64893;
    t.sid = 0;
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_EQ(gen3_edit_use_trainer_ot(&ed), Gen3UseTrainerShinyLost);
    ASSERT_EQ(G(OtId), 64893);
    ASSERT_TRUE(ot_bytes_are(&ed, RUBY_NAME));
    ASSERT_EQ(G(OtGender), 1);
    gen3_traits_of(&ed.pkm, &got);
    ASSERT_TRUE(!got.shiny);
    ASSERT_EQ(got.nature, 0);
    ASSERT_EQ(got.unown_letter, 15);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(edit_consistent(&ed));
}

/* The no-op guard. A loaded, hurt Pokemon that already carries the trainer's OT is left exactly as
 * it is (ed_commit would heal it). */
TEST(test_edit_use_trainer_ot_noop_keeps_hp) {
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    Gen3Pkm in;
    golden_pkm(&in);
    gen3_pkm_set_ot_name_raw(&in, RUBY_NAME);
    gen3_pkm_set(&in, Gen3FieldOtId, RUBY_OT_ID);
    gen3_pkm_set(&in, Gen3FieldOtGender, 1);
    gen3_pkm_set(&in, Gen3FieldHp, 10);
    gen3_pkm_fix_checksum(&in);
    ASSERT_TRUE(gen3_edit_load(&ed, &in));
    ASSERT_EQ(G(Hp), 10);
    ASSERT_EQ(G(MaxHp), 20);
    Gen3Trainer t;
    ruby_trainer(&t);
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    Gen3Editor before = ed;
    ASSERT_EQ(gen3_edit_use_trainer_ot(&ed), Gen3UseTrainerOk);
    ASSERT_TRUE(editor_eq(&ed, &before));
    ASSERT_EQ(G(Hp), 10);
    ASSERT_TRUE(edit_consistent_hp(&ed, false));
}

/* The player's name bytes are taken as they are, even ones the app can't show */
TEST(test_edit_trainer_raw_bytes) {
    static const uint8_t odd[GEN3_OT_NAME_LEN] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    Gen3Editor ed;
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    Gen3Trainer t;
    ruby_trainer(&t);
    memcpy(t.name, odd, GEN3_OT_NAME_LEN);
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_TRUE(ot_bytes_are(&ed, odd));
    ASSERT_MEM_EQ(gen3_edit_trainer(&ed)->name, odd, GEN3_OT_NAME_LEN);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    ASSERT_TRUE(ed.pristine);
    ASSERT_TRUE(edit_consistent(&ed));

    /* Another player while the editor is still the default: theirs replaces it */
    ruby_trainer(&t);
    t.tid = 1;
    t.sid = 2;
    t.gender = 0;
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_TRUE(ot_bytes_are(&ed, RUBY_NAME));
    ASSERT_EQ(G(OtId), 0x00020001);
    ASSERT_EQ(G(OtGender), 0);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    ASSERT_TRUE(ed.pristine);
    ASSERT_TRUE(edit_consistent(&ed));
}

/* Pristine (D7, gen3_edit.h) until a call changes the Pokemon's bytes. A call that refuses, fails
 * or changes nothing keeps it; set_trainer keeps it although it rewrites the OT. */
TEST(test_edit_pristine_flags) {
    Gen3Editor ed;
    Gen3Pkm golden, egg;
    golden_pkm(&golden);
    egg = golden;
    gen3_pkm_set(&egg, Gen3FieldIsEgg, 1);
    gen3_pkm_fix_checksum(&egg);

    /* Changes: cleared */
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(ed.pristine);
    gen3_edit_set_level(&ed, 10);
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(gen3_edit_set_nickname(&ed, "BOB"));
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_edit_set_item(&ed, 200);
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(gen3_edit_load(&ed, &golden));
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    ASSERT_TRUE(gen3_edit_set_species(&ed, 4));
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_edit_set_fateful(&ed, true);
    ASSERT_TRUE(!ed.pristine);

    /* No change: kept */
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    Gen3Editor before = ed;
    gen3_edit_set_level(&ed, 5);
    ASSERT_TRUE(gen3_edit_set_nickname(&ed, "")); /* the species name, as shown */
    gen3_edit_set_item(&ed, 175); /* refused */
    ASSERT_TRUE(gen3_edit_set_species(&ed, 1));
    ASSERT_TRUE(!gen3_edit_load(&ed, &egg)); /* refused */
    gen3_edit_set_item(&ed, 0);
    gen3_edit_set_ball(&ed, GEN3_EDIT_BALL);
    gen3_edit_set_pokerus(&ed, 0);
    gen3_edit_set_fateful(&ed, false);
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, GEN3_EDIT_DEFAULT_TID, GEN3_EDIT_DEFAULT_SID), Gen3OtIdOk);
    gen3_edit_set_ot_name(&ed, GEN3_EDIT_DEFAULT_OT_NAME);
    ASSERT_TRUE(editor_eq(&ed, &before));
    ASSERT_TRUE(ed.pristine);

    /* The player's OT: kept, and applying it again changes nothing */
    Gen3Trainer t;
    ruby_trainer(&t);
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_TRUE(ot_bytes_are(&ed, RUBY_NAME));
    ASSERT_TRUE(ed.pristine);
    before = ed;
    ASSERT_EQ(gen3_edit_use_trainer_ot(&ed), Gen3UseTrainerOk);
    gen3_edit_set_trainer(&ed, &t);
    ASSERT_TRUE(editor_eq(&ed, &before));
    ASSERT_TRUE(ed.pristine);

    /* Cleared for good: an edit back to the same bytes doesn't set it again */
    gen3_edit_set_level(&ed, 6);
    ASSERT_TRUE(!ed.pristine);
    gen3_edit_set_level(&ed, 5);
    ASSERT_TRUE(!ed.pristine);
    ASSERT_TRUE(edit_consistent(&ed));
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_edit_init_defaults);
    RUN(test_edit_species_resets_keeps_ot);
    RUN(test_edit_species_stores_internal);
    RUN(test_edit_species_same_is_noop);
    RUN(test_edit_species_skips_shiny_pid);
    RUN(test_edit_species_fateful);
    RUN(test_edit_set_fateful);
    RUN(test_edit_all_species);
    RUN(test_edit_level);
    RUN(test_edit_level_clamped);
    RUN(test_edit_presets);
    RUN(test_edit_level_ev_follows_level);
    RUN(test_edit_loaded_evs_kept);
    RUN(test_edit_nickname_default);
    RUN(test_edit_same_value_is_noop);
    RUN(test_edit_set_move_pp);
    RUN(test_edit_set_move_packs);
    RUN(test_edit_set_move_rejects);
    RUN(test_edit_default_moves);
    RUN(test_edit_traits_matrix);
    RUN(test_edit_traits_static_gender);
    RUN(test_edit_ability_bit_single_ability);
    RUN(test_edit_unown_letters);
    RUN(test_edit_traits_preserve_other_bytes);
    RUN(test_edit_traits_same_keeps_pid);
    RUN(test_edit_ot_id_keeps_shiny);
    RUN(test_edit_unown_shiny_impossible);
    RUN(test_edit_ot_id_shiny_lost);
    RUN(test_edit_ball_item_pokerus);
    RUN(test_edit_load_keeps_bytes);
    RUN(test_edit_load_normalises);
    RUN(test_edit_load_clamps_exp);
    RUN(test_edit_load_keeps_current_hp);
    RUN(test_edit_load_no_moves);
    RUN(test_edit_two_ability_bit_kept);
    RUN(test_normalise_flags);
    RUN(test_normalise_mail);
    RUN(test_normalise_has_species);
    RUN(test_edit_load_validates);
    RUN(test_edit_no_data_keeps_pokemon);
    RUN(test_edit_no_data_load);
    RUN(test_edit_trainer_pristine);
    RUN(test_edit_trainer_pristine_refinds_pid);
    RUN(test_edit_trainer_gender_normalised);
    RUN(test_edit_trainer_after_edit);
    RUN(test_edit_species_takes_trainer);
    RUN(test_edit_use_trainer_ot);
    RUN(test_edit_use_trainer_ot_noop_keeps_hp);
    RUN(test_edit_trainer_raw_bytes);
    RUN(test_edit_pristine_flags);
    TEST_EXIT();
}
