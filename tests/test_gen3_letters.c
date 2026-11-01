#include "test.h"
#include "bulk_file.h"
#include <src/gen3/gen3_letters.h>
#include <src/gen3/gen3_data.h>

/* Ids 10..15, already sorted by name */
static const char* const names[16] =
    {"", "", "", "", "", "", "", "", "", "", "ACID", "AIR", "BIND", "BITE", "CUT", "ZAP"};
static uint16_t at(size_t i) {
    return (uint16_t)(10 + i);
}
static bool name_of(uint16_t id, char* out) {
    strcpy(out, names[id]);
    return true;
}
static const Gen3NameList list = {6, at, name_of};

/* The same list with one read failing: the name of the third id, or the third id itself */
static bool name_fails_third(uint16_t id, char* out) {
    out[0] = '\0';
    return id != 12 && name_of(id, out);
}
static uint16_t at_fails_third(size_t i) {
    return i == 2 ? 0 : at(i);
}

TEST(test_letters_distinct_in_order) {
    char out[8];
    ASSERT_EQ(gen3_letters(&list, out, sizeof(out)), 4);
    ASSERT_TRUE(memcmp(out, "ABCZ", 4) == 0);
}

TEST(test_letters_truncates) {
    char out[2];
    ASSERT_EQ(gen3_letters(&list, out, sizeof(out)), 2);
    ASSERT_TRUE(memcmp(out, "AB", 2) == 0);
}

TEST(test_letter_ids) {
    uint16_t out[8];
    ASSERT_EQ(gen3_letter_ids(&list, 'B', out, 8), 2);
    ASSERT_EQ(out[0], 12);
    ASSERT_EQ(out[1], 13);
    ASSERT_EQ(gen3_letter_ids(&list, 'Q', out, 8), 0);
    ASSERT_EQ(gen3_letter_ids(&list, 'A', out, 1), 1);
}

/* An id or a name that can't be read gives 0: the screen shows Data missing, not a short list */
TEST(test_letters_fail_closed) {
    const Gen3NameList bad_name = {6, at, name_fails_third};
    const Gen3NameList bad_id = {6, at_fails_third, name_of};
    char out[8];
    uint16_t ids[8];
    ASSERT_EQ(gen3_letters(&bad_name, out, sizeof(out)), 0);
    ASSERT_EQ(gen3_letter_ids(&bad_name, 'A', ids, 8), 0);
    ASSERT_EQ(gen3_letter_ids(&bad_name, 'Z', ids, 8), 0);
    ASSERT_EQ(gen3_letters(&bad_id, out, sizeof(out)), 0);
    ASSERT_EQ(gen3_letter_ids(&bad_id, 'A', ids, 8), 0);
    ASSERT_EQ(gen3_letter_ids(&bad_id, 'Z', ids, 8), 0);
}

TEST(test_real_moves_and_items) {
    char out[64];
    uint16_t ids[400];
    size_t n;
    size_t total = 0;
    const Gen3NameList moves = {GEN3_MOVE_COUNT - 1, gen3_move_by_name_get, gen3_move_name_get};
    const Gen3NameList items = {
        GEN3_HOLDABLE_COUNT, gen3_holdable_by_name_get, gen3_item_name_get};

    n = gen3_letters(&moves, out, sizeof(out));
    ASSERT_TRUE(n > 10 && n < sizeof(out));
    ASSERT_TRUE(n <= GEN3_LETTERS_MAX);
    for(size_t i = 0; i < n; i++) {
        size_t k = gen3_letter_ids(&moves, out[i], ids, 400);
        ASSERT_TRUE(k <= GEN3_LETTER_IDS_MAX);
        total += k;
    }
    ASSERT_EQ(total, GEN3_MOVE_COUNT - 1);

    n = gen3_letters(&items, out, sizeof(out));
    ASSERT_TRUE(n > 10 && n < sizeof(out));
    total = 0;
    ASSERT_TRUE(n <= GEN3_LETTERS_MAX);
    for(size_t i = 0; i < n; i++) {
        size_t k = gen3_letter_ids(&items, out[i], ids, 400);
        ASSERT_TRUE(k <= GEN3_LETTER_IDS_MAX);
        total += k;
    }
    ASSERT_EQ(total, GEN3_HOLDABLE_COUNT);
    ASSERT_TRUE(gen3_bulk_ready());

    /* Without the bulk file, both lists fail closed */
    gen3_bulk_close();
    ASSERT_EQ(gen3_letters(&moves, out, sizeof(out)), 0);
    ASSERT_EQ(gen3_letter_ids(&items, 'A', ids, 400), 0);
    ASSERT_TRUE(bulk_file_open());
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_letters_distinct_in_order);
    RUN(test_letters_truncates);
    RUN(test_letter_ids);
    RUN(test_letters_fail_closed);
    RUN(test_real_moves_and_items);
    TEST_EXIT();
}
