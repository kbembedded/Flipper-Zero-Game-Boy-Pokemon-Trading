#include "test.h"
#include "bulk_file.h"
#include <src/gen3/gen3_data.h>

#define LU(level, move) (((level) << 9) | (move))

/* The name from the bulk file; "" if gen3_species_name_get refuses */
static const char* species_name(uint16_t national) {
    static char name[GEN3_SPECIES_NAME_LEN + 1];
    gen3_species_name_get(national, name);
    return name;
}

/* The move or item name from the bulk file, "" if refused. Two buffers each, so a test can
 * compare neighbours. */
static const char* move_name(uint16_t move) {
    static char names[2][GEN3_MOVE_NAME_LEN + 1];
    static int k;
    k ^= 1;
    gen3_move_name_get(move, names[k]);
    return names[k];
}

static const char* item_name(uint16_t item) {
    static char names[2][GEN3_ITEM_NAME_LEN + 1];
    static int k;
    k ^= 1;
    gen3_item_name_get(item, names[k]);
    return names[k];
}

static int species_matches(
    uint16_t national,
    const uint8_t base[6],
    uint8_t type0,
    uint8_t type1,
    uint8_t ratio,
    uint8_t growth,
    uint8_t ability0,
    uint8_t ability1) {
    const Gen3SpeciesInfo* s = gen3_species(national);
    if(!s) return 0;
    return memcmp(s->base, base, 6) == 0 && s->type[0] == type0 && s->type[1] == type1 &&
           s->gender_ratio == ratio && s->growth == growth && s->ability[0] == ability0 &&
           s->ability[1] == ability1;
}

TEST(test_data_internal_national_map) {
    ASSERT_EQ(gen3_internal_from_national(252), 277);
    ASSERT_EQ(gen3_national_from_internal(277), 252);
    ASSERT_EQ(gen3_national_from_internal(1), 1);
    ASSERT_EQ(gen3_national_from_internal(251), 251);
    for(uint16_t i = 252; i <= 276; i++) ASSERT_EQ(gen3_national_from_internal(i), 0);
    ASSERT_EQ(gen3_national_from_internal(0), 0);
    ASSERT_EQ(gen3_national_from_internal(303), 292);
    ASSERT_EQ(gen3_national_from_internal(392), 280);
    ASSERT_EQ(gen3_national_from_internal(410), 386);
    ASSERT_EQ(gen3_national_from_internal(411), 358);
    ASSERT_EQ(gen3_national_from_internal(412), 0);
    ASSERT_EQ(gen3_national_from_internal(0xFFFF), 0);
    ASSERT_EQ(gen3_internal_from_national(0), 0);
    ASSERT_EQ(gen3_internal_from_national(387), 0);
    for(uint16_t n = 1; n <= GEN3_NATIONAL_COUNT; n++) {
        uint16_t internal = gen3_internal_from_national(n);
        ASSERT_TRUE(internal != 0);
        ASSERT_EQ(gen3_national_from_internal(internal), n);
    }
}

TEST(test_data_bulbasaur) {
    const uint8_t base[6] = {45, 49, 49, 45, 65, 65};
    ASSERT_TRUE(species_matches(1, base, 12, 3, 31, 3, 65, 0));
    ASSERT_EQ(gen3_species(1)->friendship, 70);
    ASSERT_EQ(gen3_species(1)->internal, 1);
    ASSERT_STR_EQ(species_name(1), "BULBASAUR");
}

TEST(test_data_treecko) {
    const uint8_t base[6] = {40, 45, 35, 70, 65, 55};
    ASSERT_TRUE(species_matches(252, base, 12, 12, 31, 3, 65, 0));
    ASSERT_EQ(gen3_species(252)->internal, 277);
    ASSERT_STR_EQ(species_name(252), "TREECKO");
}

TEST(test_data_deoxys) {
    const uint8_t base[6] = {50, 150, 50, 150, 150, 50};
    ASSERT_TRUE(species_matches(GEN3_NATIONAL_DEOXYS, base, 14, 14, 255, 5, 46, 0));
    ASSERT_EQ(gen3_species(GEN3_NATIONAL_DEOXYS)->internal, 410);
    ASSERT_EQ(gen3_species(GEN3_NATIONAL_DEOXYS)->friendship, 0);
    ASSERT_EQ(gen3_species(GEN3_NATIONAL_DEOXYS)->gender_ratio, GEN3_GENDERLESS);
}

TEST(test_data_shedinja) {
    const Gen3SpeciesInfo* s = gen3_species(GEN3_NATIONAL_SHEDINJA);
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(s->internal, 303);
    ASSERT_EQ(s->base[0], 1);
    ASSERT_EQ(s->type[0], 6);
    ASSERT_EQ(s->type[1], 7);
    ASSERT_EQ(s->gender_ratio, 255);
    ASSERT_EQ(s->growth, Gen3GrowthErratic);
    ASSERT_EQ(s->ability[0], 25);
    ASSERT_EQ(s->ability[1], 0);
}

TEST(test_data_unown) {
    const uint8_t base[6] = {48, 72, 48, 48, 72, 48};
    ASSERT_TRUE(species_matches(GEN3_NATIONAL_UNOWN, base, 14, 14, 255, 0, 26, 0));
    uint16_t e[GEN3_LEARNSET_MAX];
    ASSERT_EQ(gen3_learnset_get(GEN3_NATIONAL_UNOWN, e, GEN3_LEARNSET_MAX), 1);
    ASSERT_EQ(e[0], LU(1, 237));
}

TEST(test_data_two_abilities) {
    const Gen3SpeciesInfo* s = gen3_species(280); /* Ralts */
    ASSERT_TRUE(s != NULL);
    ASSERT_EQ(s->ability[0], 28);
    ASSERT_EQ(s->ability[1], 36);
    ASSERT_EQ(s->gender_ratio, 127);
    ASSERT_EQ(s->growth, Gen3GrowthSlow);
    ASSERT_EQ(s->friendship, 35);
}

TEST(test_data_names) {
    ASSERT_STR_EQ(species_name(29), "NIDORAN\200");
    ASSERT_EQ(gen3_species(29)->gender_ratio, GEN3_GENDER_FEMALE_ONLY);
    ASSERT_STR_EQ(species_name(32), "NIDORAN\201");
    ASSERT_EQ(gen3_species(32)->gender_ratio, GEN3_GENDER_MALE_ONLY);
    ASSERT_STR_EQ(species_name(83), "FARFETCH'D");
    ASSERT_STR_EQ(species_name(122), "MR. MIME");
    ASSERT_STR_EQ(species_name(250), "HO-OH");
    ASSERT_STR_EQ(species_name(0), "");
    ASSERT_STR_EQ(species_name(387), "");
    ASSERT_TRUE(gen3_species(0) == NULL);
    ASSERT_TRUE(gen3_species(387) == NULL);
}

TEST(test_data_moves) {
    char name[GEN3_MOVE_NAME_LEN + 1];
    ASSERT_TRUE(gen3_move_name_get(1, name));
    ASSERT_STR_EQ(name, "POUND");
    ASSERT_EQ(gen3_move_pp(1), 35);
    ASSERT_TRUE(gen3_move_name_get(9, name));
    ASSERT_STR_EQ(name, "THUNDERPUNCH");
    ASSERT_TRUE(gen3_move_name_get(237, name));
    ASSERT_STR_EQ(name, "HIDDEN POWER");
    ASSERT_EQ(gen3_move_pp(237), 15);
    ASSERT_TRUE(gen3_move_name_get(354, name));
    ASSERT_STR_EQ(name, "PSYCHO BOOST");
    ASSERT_EQ(gen3_move_pp(354), 5);
    ASSERT_TRUE(gen3_move_name_get(0, name));
    ASSERT_STR_EQ(name, "-");
    ASSERT_TRUE(!gen3_move_name_get(GEN3_MOVE_COUNT, name));
    ASSERT_STR_EQ(name, "");
    ASSERT_EQ(gen3_move_pp(GEN3_MOVE_COUNT), 0);

    ASSERT_EQ(gen3_move_by_name_get(0), 71);
    ASSERT_STR_EQ(move_name(71), "ABSORB");
    ASSERT_EQ(gen3_move_by_name_get(1), 51);
    ASSERT_STR_EQ(move_name(51), "ACID");
    ASSERT_EQ(gen3_move_by_name_get(353), 192);
    ASSERT_STR_EQ(move_name(192), "ZAP CANNON");
    ASSERT_EQ(gen3_move_by_name_get(GEN3_MOVE_COUNT - 1), 0);

    /* Sorted by name, and every move 1..354 exactly once */
    static uint8_t seen[GEN3_MOVE_COUNT];
    memset(seen, 0, sizeof(seen));
    for(size_t i = 0; i < GEN3_MOVE_COUNT - 1; i++) {
        uint16_t move = gen3_move_by_name_get(i);
        ASSERT_TRUE(move >= 1 && move < GEN3_MOVE_COUNT);
        ASSERT_EQ(seen[move], 0);
        seen[move] = 1;
        ASSERT_TRUE(move_name(move)[0] != '\0');
        if(i + 1 < GEN3_MOVE_COUNT - 1) {
            const char* a = move_name(move);
            const char* b = move_name(gen3_move_by_name_get(i + 1));
            ASSERT_TRUE(strcmp(a, b) < 0);
        }
    }
    ASSERT_TRUE(gen3_bulk_ready());
}

TEST(test_data_items) {
    char name[GEN3_ITEM_NAME_LEN + 1];
    ASSERT_STR_EQ(item_name(1), "MASTER BALL");
    ASSERT_STR_EQ(item_name(4), "POKE BALL");
    ASSERT_STR_EQ(item_name(12), "PREMIER BALL");
    ASSERT_STR_EQ(item_name(175), "ENIGMA BERRY");
    ASSERT_STR_EQ(item_name(200), "LEFTOVERS");
    ASSERT_STR_EQ(item_name(273), "POKEBLOCK CASE");
    /* An id without a name reads fine, as "???" */
    ASSERT_TRUE(gen3_item_name_get(0, name));
    ASSERT_STR_EQ(name, "???");
    ASSERT_TRUE(gen3_item_name_get(52, name));
    ASSERT_STR_EQ(name, "???");
    ASSERT_TRUE(!gen3_item_name_get(GEN3_ITEM_COUNT, name));
    ASSERT_STR_EQ(name, "");

    ASSERT_TRUE(gen3_item_holdable(4));
    ASSERT_TRUE(gen3_item_holdable(200));
    ASSERT_TRUE(gen3_item_holdable(289)); /* TM01 */
    ASSERT_TRUE(!gen3_item_holdable(0));
    ASSERT_TRUE(!gen3_item_holdable(52));
    ASSERT_TRUE(!gen3_item_holdable(175)); /* Enigma Berry */
    ASSERT_TRUE(!gen3_item_holdable(259)); /* Mach Bike */
    ASSERT_TRUE(!gen3_item_holdable(339)); /* HM01 */
    ASSERT_TRUE(!gen3_item_holdable(376));
    ASSERT_TRUE(!gen3_item_holdable(GEN3_ITEM_COUNT));
    /* Mail has no mail record behind it (mail index 0xFF); READ/TAKE would write out of bounds. */
    ASSERT_STR_EQ(item_name(121), "ORANGE MAIL");
    ASSERT_STR_EQ(item_name(132), "RETRO MAIL");
    for(uint16_t mail = 121; mail <= 132; mail++) ASSERT_TRUE(!gen3_item_holdable(mail));
    ASSERT_TRUE(gen3_item_holdable(133)); /* Cheri Berry, right after the mail */

    ASSERT_EQ(GEN3_HOLDABLE_COUNT, 231);
    ASSERT_EQ(gen3_holdable_by_name_get(0), 146);
    ASSERT_STR_EQ(item_name(146), "AGUAV BERRY");
    ASSERT_EQ(gen3_holdable_by_name_get(230), 70);
    ASSERT_STR_EQ(item_name(70), "ZINC");
    ASSERT_EQ(gen3_holdable_by_name_get(GEN3_HOLDABLE_COUNT), 0);
    size_t holdable = 0;
    for(uint16_t item = 0; item < GEN3_ITEM_COUNT + 8; item++)
        if(gen3_item_holdable(item)) holdable++;
    ASSERT_EQ(holdable, GEN3_HOLDABLE_COUNT);
    for(size_t i = 0; i < GEN3_HOLDABLE_COUNT; i++) {
        ASSERT_TRUE(gen3_item_holdable(gen3_holdable_by_name_get(i)));
        if(i + 1 < GEN3_HOLDABLE_COUNT) {
            const char* a = item_name(gen3_holdable_by_name_get(i));
            const char* b = item_name(gen3_holdable_by_name_get(i + 1));
            ASSERT_TRUE(strcmp(a, b) < 0);
        }
    }
    ASSERT_TRUE(gen3_bulk_ready());
}

TEST(test_data_small_lists) {
    ASSERT_STR_EQ(gen3_ability_name(0), "-------");
    ASSERT_STR_EQ(gen3_ability_name(65), "OVERGROW");
    ASSERT_STR_EQ(gen3_ability_name(77), "AIR LOCK");
    ASSERT_STR_EQ(gen3_ability_name(GEN3_ABILITY_COUNT), "???");
    ASSERT_STR_EQ(gen3_nature_name(0), "HARDY");
    ASSERT_STR_EQ(gen3_nature_name(3), "ADAMANT");
    ASSERT_STR_EQ(gen3_nature_name(24), "QUIRKY");
    ASSERT_STR_EQ(gen3_nature_name(GEN3_NATURE_COUNT), "???");
    ASSERT_STR_EQ(gen3_type_name(9), "???");
    ASSERT_STR_EQ(gen3_type_name(12), "GRASS");
    ASSERT_STR_EQ(gen3_type_name(13), "ELECTR");
    ASSERT_STR_EQ(gen3_type_name(17), "DARK");
    ASSERT_STR_EQ(gen3_type_name(GEN3_TYPE_COUNT), "???");
}

TEST(test_data_species_predicates) {
    ASSERT_TRUE(gen3_species_two_abilities(280)); /* Ralts: Synchronize / Trace */
    ASSERT_TRUE(!gen3_species_two_abilities(1)); /* Bulbasaur: Overgrow */
    ASSERT_TRUE(!gen3_species_two_abilities(0));
    ASSERT_TRUE(!gen3_species_two_abilities(GEN3_NATIONAL_COUNT + 1));
    ASSERT_TRUE(!gen3_species_gender_fixed(1)); /* ratio 31 */
    ASSERT_TRUE(!gen3_species_gender_fixed(280)); /* ratio 127 */
    ASSERT_TRUE(gen3_species_gender_fixed(29)); /* female only */
    ASSERT_TRUE(gen3_species_gender_fixed(32)); /* male only */
    ASSERT_TRUE(gen3_species_gender_fixed(GEN3_NATIONAL_UNOWN)); /* genderless */
    /* Unknown species: nothing to choose */
    ASSERT_TRUE(gen3_species_gender_fixed(0));
    ASSERT_TRUE(gen3_species_gender_fixed(GEN3_NATIONAL_COUNT + 1));
    for(uint16_t n = 1; n <= GEN3_NATIONAL_COUNT; n++) {
        const Gen3SpeciesInfo* s = gen3_species(n);
        uint8_t r = s->gender_ratio;
        ASSERT_EQ(gen3_species_two_abilities(n), s->ability[1] != 0);
        ASSERT_EQ(
            gen3_species_gender_fixed(n),
            r == GEN3_GENDER_MALE_ONLY || r == GEN3_GENDER_FEMALE_ONLY || r == GEN3_GENDERLESS);
    }
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_data_internal_national_map);
    RUN(test_data_bulbasaur);
    RUN(test_data_treecko);
    RUN(test_data_deoxys);
    RUN(test_data_shedinja);
    RUN(test_data_unown);
    RUN(test_data_two_abilities);
    RUN(test_data_names);
    RUN(test_data_moves);
    RUN(test_data_items);
    RUN(test_data_small_lists);
    RUN(test_data_species_predicates);
    TEST_EXIT();
}
