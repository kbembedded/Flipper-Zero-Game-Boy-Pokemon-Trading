#include "test.h"
#include "bulk_file.h"
#include <src/gen3/gen3_data.h>

#define LU(level, move) (((level) << 9) | (move))

/* files/gen3_bulk.bin v2: header, species names, learnset starts, learnset entries (all of v1),
 * then move names (at 12943), move ids sorted by name (17558), item names (18266) and holdable
 * item ids sorted by name (23921) */
#define BULK_V1_SIZE (16 + 11 * 387 + 2 * 388 + 2 * 3947)
#define BULK_SIZE (16 + 11 * 387 + 2 * 388 + 2 * 3947 + 13 * 355 + 2 * 354 + 15 * 377 + 2 * 231)
_Static_assert(BULK_SIZE == 24383, "gen3_bulk.bin v2 size");
_Static_assert(
    BULK_V1_SIZE == 12943 && 12943 + 13 * 355 == 17558 && 17558 + 2 * 354 == 18266 &&
        18266 + 15 * 377 == 23921 && 23921 + 2 * 231 == BULK_SIZE,
    "gen3_bulk.bin v2 section offsets");

/* An in-memory copy of the file, to corrupt one thing at a time */
static uint8_t mem_data[BULK_SIZE + 1];
static size_t mem_size;

static bool mem_read(void* ctx, uint32_t offset, void* buf, uint32_t len) {
    (void)ctx;
    if(offset > mem_size || len > mem_size - offset) return false;
    memcpy(buf, mem_data + offset, len);
    return true;
}

/* Loads the real file; true if it has exactly the pinned size. */
static bool mem_load(void) {
    FILE* f = fopen(BULK_FILE_PATH, "rb");
    if(!f) return false;
    mem_size = fread(mem_data, 1, sizeof(mem_data), f);
    fclose(f);
    return mem_size == BULK_SIZE;
}

/* The real file is installed, then the copy is offered: false, and nothing is left installed. */
static bool mem_open_fails(void) {
    if(!bulk_file_open() || !gen3_bulk_ready()) return false;
    return !gen3_bulk_open(mem_read, NULL) && !gen3_bulk_ready();
}

TEST(test_bulk_open_ok) {
    ASSERT_TRUE(bulk_file_open());
    ASSERT_TRUE(gen3_bulk_ready());
    /* The unmodified copy opens too, so the failures below are down to the corruption */
    ASSERT_TRUE(mem_load());
    ASSERT_TRUE(gen3_bulk_open(mem_read, NULL));
    ASSERT_TRUE(gen3_bulk_ready());
    ASSERT_TRUE(!gen3_bulk_open(NULL, NULL));
    ASSERT_TRUE(!gen3_bulk_ready());
}

TEST(test_bulk_open_bad_magic) {
    ASSERT_TRUE(mem_load());
    mem_data[0] = 'g';
    ASSERT_TRUE(mem_open_fails());
    ASSERT_TRUE(mem_load());
    mem_data[3] ^= 0x01;
    ASSERT_TRUE(mem_open_fails());
}

TEST(test_bulk_open_bad_header) {
    /* version 2, species_count 387, learnset_entries 3947: each is checked */
    static const uint32_t fields[] = {4, 5, 6, 7, 8, 11};
    for(size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        ASSERT_TRUE(mem_load());
        mem_data[fields[i]] ^= 0x01;
        ASSERT_TRUE(mem_open_fails());
    }
}

TEST(test_bulk_open_bad_crc) {
    /* The first and last payload bytes, a learnset start, the first byte of each v2 section
     * and the stored CRC itself */
    static const uint32_t bytes[] = {
        16, BULK_SIZE - 1, 16 + 11 * 387 + 2, 12943, 17558, 18266, 23921, 12, 15};
    for(size_t i = 0; i < sizeof(bytes) / sizeof(bytes[0]); i++) {
        ASSERT_TRUE(mem_load());
        mem_data[bytes[i]] ^= 0x40;
        ASSERT_TRUE(mem_open_fails());
    }
}

TEST(test_bulk_open_short) {
    static const size_t sizes[] = {
        0, 15, 16, BULK_V1_SIZE - 2 * 3947, 12943, BULK_SIZE - 462, BULK_SIZE - 1};
    for(size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        ASSERT_TRUE(mem_load());
        mem_size = sizes[i];
        ASSERT_TRUE(mem_open_fails());
    }
    /* Anything after the pinned layout is refused too: the CRC covers to the end of the file */
    ASSERT_TRUE(mem_load());
    mem_data[BULK_SIZE] = 0;
    mem_size = BULK_SIZE + 1;
    ASSERT_TRUE(mem_open_fails());
}

/* CRC-32 (IEEE, reflected), bit by bit, as binascii.crc32 */
static uint32_t crc32_of(const uint8_t* p, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for(size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for(int k = 0; k < 8; k++) crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return ~crc;
}

/* A well-formed v1 file (an asset unpacked by an older build) has no move or item names: it is
 * refused, so the editor says Data missing rather than show empty names. */
TEST(test_bulk_open_v1_refused) {
    ASSERT_TRUE(mem_load());
    mem_size = BULK_V1_SIZE;
    mem_data[4] = 1;
    mem_data[5] = 0;
    uint32_t crc = crc32_of(mem_data + 16, BULK_V1_SIZE - 16);
    for(int k = 0; k < 4; k++) mem_data[12 + k] = (uint8_t)(crc >> (8 * k));
    ASSERT_TRUE(mem_open_fails());
}

TEST(test_species_name_get) {
    char name[GEN3_SPECIES_NAME_LEN + 1];
    ASSERT_TRUE(bulk_file_open());
    ASSERT_TRUE(gen3_species_name_get(1, name));
    ASSERT_STR_EQ(name, "BULBASAUR");
    ASSERT_TRUE(gen3_species_name_get(29, name));
    ASSERT_STR_EQ(name, "NIDORAN\200");
    ASSERT_TRUE(gen3_species_name_get(252, name));
    ASSERT_STR_EQ(name, "TREECKO");
    ASSERT_TRUE(gen3_species_name_get(358, name));
    ASSERT_STR_EQ(name, "CHIMECHO");
    ASSERT_TRUE(gen3_species_name_get(GEN3_NATIONAL_DEOXYS, name));
    ASSERT_STR_EQ(name, "DEOXYS");
    memset(name, 'x', sizeof(name));
    ASSERT_TRUE(!gen3_species_name_get(0, name));
    ASSERT_STR_EQ(name, "");
    memset(name, 'x', sizeof(name));
    ASSERT_TRUE(!gen3_species_name_get(GEN3_NATIONAL_COUNT + 1, name));
    ASSERT_STR_EQ(name, "");
    for(uint16_t n = 1; n <= GEN3_NATIONAL_COUNT; n++) {
        ASSERT_TRUE(gen3_species_name_get(n, name));
        ASSERT_TRUE(name[0] != '\0');
        ASSERT_TRUE(strlen(name) <= GEN3_SPECIES_NAME_LEN);
    }
}

TEST(test_learnset_get) {
    uint16_t e[GEN3_LEARNSET_MAX + 1];
    ASSERT_TRUE(bulk_file_open());

    ASSERT_EQ(gen3_learnset_get(1, e, GEN3_LEARNSET_MAX), 11); /* Bulbasaur */
    ASSERT_EQ(e[0], LU(1, 33));
    ASSERT_EQ(e[10], LU(46, 76));
    ASSERT_EQ(gen3_learnset_get(358, e, GEN3_LEARNSET_MAX), 12); /* Chimecho: internal id 411 */
    ASSERT_EQ(e[0], LU(1, 35));
    ASSERT_EQ(e[2], LU(9, 310));
    ASSERT_EQ(e[11], LU(46, 94));
    ASSERT_EQ(gen3_learnset_get(252, e, GEN3_LEARNSET_MAX), 11); /* Treecko */
    ASSERT_EQ(e[2], LU(6, 71));
    ASSERT_TRUE(gen3_learnset_get(2, e, GEN3_LEARNSET_MAX) > 3); /* Ivysaur */
    ASSERT_EQ(e[3], LU(4, 45));
    ASSERT_EQ(gen3_learnset_get(GEN3_NATIONAL_UNOWN, e, GEN3_LEARNSET_MAX), 1);
    ASSERT_EQ(e[0], LU(1, 237));

    ASSERT_EQ(gen3_learnset_get(0, e, GEN3_LEARNSET_MAX), 0);
    ASSERT_EQ(gen3_learnset_get(GEN3_NATIONAL_COUNT + 1, e, GEN3_LEARNSET_MAX), 0);

    /* max truncates, in level order, and nothing past max is written */
    e[3] = 0xBEEF;
    ASSERT_EQ(gen3_learnset_get(1, e, 3), 3);
    ASSERT_EQ(e[0], LU(1, 33));
    ASSERT_EQ(e[3], 0xBEEF);
    ASSERT_EQ(gen3_learnset_get(1, e, 0), 0);

    size_t total = 0;
    size_t largest = 0;
    for(uint16_t n = 1; n <= GEN3_NATIONAL_COUNT; n++) {
        e[GEN3_LEARNSET_MAX] = 0xBEEF;
        size_t count = gen3_learnset_get(n, e, GEN3_LEARNSET_MAX);
        ASSERT_TRUE(count > 0);
        ASSERT_EQ(e[GEN3_LEARNSET_MAX], 0xBEEF);
        ASSERT_EQ(e[0] >> 9, 1); /* a default moveset is never empty */
        for(size_t i = 1; i < count; i++) ASSERT_TRUE((e[i] >> 9) >= (e[i - 1] >> 9));
        if(count > largest) largest = count;
        total += count;
    }
    ASSERT_EQ(total, GEN3_LEARNSET_COUNT);
    ASSERT_EQ(largest, GEN3_LEARNSET_MAX);
}

/* A reader over the in-memory copy that fails one read, the fail_at-th from now (1 = the next).
 * flaky_reads counts the reads it is asked for. */
static int fail_at;
static int flaky_reads;

static bool flaky_read(void* ctx, uint32_t offset, void* buf, uint32_t len) {
    flaky_reads++;
    if(fail_at > 0 && --fail_at == 0) return false;
    return mem_read(ctx, offset, buf, len);
}

/* A read that fails after a good open (the SD card pulled, say) closes the source, so the next
 * Gen III menu visit opens the file again. */
TEST(test_read_failure_closes) {
    char name[GEN3_SPECIES_NAME_LEN + 1];
    char move[GEN3_MOVE_NAME_LEN + 1];
    char item[GEN3_ITEM_NAME_LEN + 1];
    uint16_t e[GEN3_LEARNSET_MAX];
    ASSERT_TRUE(mem_load());
    fail_at = 0;
    ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));

    fail_at = 1; /* the name read */
    ASSERT_TRUE(!gen3_species_name_get(1, name));
    ASSERT_TRUE(!gen3_bulk_ready());
    ASSERT_TRUE(!gen3_species_name_get(1, name)); /* reads would work again, but it is closed */
    ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));
    ASSERT_TRUE(gen3_species_name_get(1, name));
    ASSERT_STR_EQ(name, "BULBASAUR");

    /* A learnset is two reads: the starts, then the entries. Either failing closes it. */
    for(int k = 1; k <= 2; k++) {
        fail_at = k;
        ASSERT_EQ(gen3_learnset_get(1, e, GEN3_LEARNSET_MAX), 0);
        ASSERT_TRUE(!gen3_bulk_ready());
        ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));
        ASSERT_EQ(gen3_learnset_get(1, e, GEN3_LEARNSET_MAX), 11);
    }

    /* The file no longer matches what was checked at open (a learnset ending past the entries):
     * closed too, so the next open checks it again */
    uint32_t end_hi = 16 + 11 * 387 + 2 * 2 + 1; /* species 1's end, high byte */
    uint8_t saved = mem_data[end_hi];
    mem_data[end_hi] = 0xFF;
    ASSERT_EQ(gen3_learnset_get(1, e, GEN3_LEARNSET_MAX), 0);
    ASSERT_TRUE(!gen3_bulk_ready());
    ASSERT_TRUE(!gen3_bulk_open(flaky_read, NULL)); /* the CRC no longer matches */
    mem_data[end_hi] = saved;
    ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));
    ASSERT_EQ(gen3_learnset_get(1, e, GEN3_LEARNSET_MAX), 11);

    /* A species out of range reads nothing, so it leaves the source open */
    ASSERT_TRUE(!gen3_species_name_get(0, name));
    ASSERT_EQ(gen3_learnset_get(GEN3_NATIONAL_COUNT + 1, e, GEN3_LEARNSET_MAX), 0);
    ASSERT_TRUE(gen3_bulk_ready());

    /* The move and item accessors make one read each. A failed one gives false and "" (or 0)
     * and closes the source; reopened, the same call works. */
    flaky_reads = 0;
    ASSERT_TRUE(gen3_move_name_get(1, move));
    ASSERT_STR_EQ(move, "POUND");
    ASSERT_EQ(flaky_reads, 1);
    ASSERT_TRUE(gen3_item_name_get(200, item));
    ASSERT_STR_EQ(item, "LEFTOVERS");
    ASSERT_EQ(flaky_reads, 2);
    ASSERT_EQ(gen3_move_by_name_get(0), 71);
    ASSERT_EQ(flaky_reads, 3);
    ASSERT_EQ(gen3_holdable_by_name_get(0), 146);
    ASSERT_EQ(flaky_reads, 4);

    fail_at = 1;
    memset(move, 'x', sizeof(move));
    ASSERT_TRUE(!gen3_move_name_get(1, move));
    ASSERT_STR_EQ(move, "");
    ASSERT_TRUE(!gen3_bulk_ready());
    ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));
    ASSERT_TRUE(gen3_move_name_get(1, move));
    ASSERT_STR_EQ(move, "POUND");

    fail_at = 1;
    memset(item, 'x', sizeof(item));
    ASSERT_TRUE(!gen3_item_name_get(200, item));
    ASSERT_STR_EQ(item, "");
    ASSERT_TRUE(!gen3_bulk_ready());
    ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));
    ASSERT_TRUE(gen3_item_name_get(200, item));
    ASSERT_STR_EQ(item, "LEFTOVERS");

    fail_at = 1;
    ASSERT_EQ(gen3_move_by_name_get(0), 0);
    ASSERT_TRUE(!gen3_bulk_ready());
    ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));
    ASSERT_EQ(gen3_move_by_name_get(0), 71);

    fail_at = 1;
    ASSERT_EQ(gen3_holdable_by_name_get(0), 0);
    ASSERT_TRUE(!gen3_bulk_ready());
    ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));
    ASSERT_EQ(gen3_holdable_by_name_get(0), 146);

    /* The file no longer reads as it did at open: a sorted move id outside 1..354 */
    uint8_t saved_id[2] = {mem_data[17558], mem_data[17559]};
    mem_data[17558] = 0;
    mem_data[17559] = 0;
    ASSERT_EQ(gen3_move_by_name_get(0), 0);
    ASSERT_TRUE(!gen3_bulk_ready());
    mem_data[17558] = saved_id[0];
    mem_data[17559] = saved_id[1];
    ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));
    ASSERT_EQ(gen3_move_by_name_get(0), 71);

    /* Nor a holdable id that gen3_item_holdable rejects: 121, ORANGE MAIL */
    saved_id[0] = mem_data[23921];
    saved_id[1] = mem_data[23922];
    mem_data[23921] = 121;
    mem_data[23922] = 0;
    ASSERT_EQ(gen3_holdable_by_name_get(0), 0);
    ASSERT_TRUE(!gen3_bulk_ready());
    mem_data[23921] = saved_id[0];
    mem_data[23922] = saved_id[1];
    ASSERT_TRUE(gen3_bulk_open(flaky_read, NULL));
    ASSERT_EQ(gen3_holdable_by_name_get(0), 146);

    /* Out of range reads nothing, so it leaves the source open */
    flaky_reads = 0;
    memset(move, 'x', sizeof(move));
    ASSERT_TRUE(!gen3_move_name_get(GEN3_MOVE_COUNT, move));
    ASSERT_STR_EQ(move, "");
    memset(item, 'x', sizeof(item));
    ASSERT_TRUE(!gen3_item_name_get(GEN3_ITEM_COUNT, item));
    ASSERT_STR_EQ(item, "");
    ASSERT_EQ(gen3_move_by_name_get(GEN3_MOVE_COUNT - 1), 0);
    ASSERT_EQ(gen3_holdable_by_name_get(GEN3_HOLDABLE_COUNT), 0);
    ASSERT_EQ(flaky_reads, 0);
    ASSERT_TRUE(gen3_bulk_ready());
}

TEST(test_not_ready) {
    char name[GEN3_SPECIES_NAME_LEN + 1];
    char move[GEN3_MOVE_NAME_LEN + 1];
    char item[GEN3_ITEM_NAME_LEN + 1];
    uint16_t e[GEN3_LEARNSET_MAX];
    ASSERT_TRUE(bulk_file_open());
    gen3_bulk_close();
    ASSERT_TRUE(!gen3_bulk_ready());
    memset(name, 'x', sizeof(name));
    ASSERT_TRUE(!gen3_species_name_get(1, name));
    ASSERT_STR_EQ(name, "");
    ASSERT_EQ(gen3_learnset_get(1, e, GEN3_LEARNSET_MAX), 0);
    memset(move, 'x', sizeof(move));
    ASSERT_TRUE(!gen3_move_name_get(1, move));
    ASSERT_STR_EQ(move, "");
    memset(item, 'x', sizeof(item));
    ASSERT_TRUE(!gen3_item_name_get(200, item));
    ASSERT_STR_EQ(item, "");
    ASSERT_EQ(gen3_move_by_name_get(0), 0);
    ASSERT_EQ(gen3_holdable_by_name_get(0), 0);
    gen3_bulk_close(); /* twice is fine */
    ASSERT_TRUE(!gen3_bulk_ready());
}

int main(void) {
    RUN(test_bulk_open_ok);
    RUN(test_bulk_open_bad_magic);
    RUN(test_bulk_open_bad_header);
    RUN(test_bulk_open_bad_crc);
    RUN(test_bulk_open_short);
    RUN(test_bulk_open_v1_refused);
    RUN(test_species_name_get);
    RUN(test_learnset_get);
    RUN(test_read_failure_closes);
    RUN(test_not_ready);
    TEST_EXIT();
}
