#include "test.h"
#include <src/gen3/gen3_data.h>

/* files/all_sprites.fxbm, as src/views/gen3_species_select.c reads it: one 404-byte record per
 * species, index = National Dex - 1. Each is a u32 400 (the bytes after it), a u32 width 56,
 * a u32 height 56 and 56 rows of 7 bytes. tools/gen3_sprites.py writes it. The tests run from
 * tests/. */
#define SPRITES_PATH   "../files/all_sprites.fxbm"
#define RECORD_SIZE    404
#define SPRITE_BYTES   (7 * 56)
#define SPRITES_SIZE   (GEN3_SPRITE_MAX_NATIONAL * RECORD_SIZE)

static uint8_t sprites[SPRITES_SIZE + 1];
static size_t sprites_size;

static uint32_t u32_at(const uint8_t* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

TEST(test_every_species_has_a_sprite) {
    ASSERT_EQ(GEN3_SPRITE_MAX_NATIONAL, GEN3_NATIONAL_DEOXYS);
    ASSERT_EQ(sprites_size, SPRITES_SIZE);
}

TEST(test_records) {
    for(size_t n = 1; n <= GEN3_SPRITE_MAX_NATIONAL; n++) {
        const uint8_t* rec = sprites + (n - 1) * RECORD_SIZE;
        ASSERT_EQ(u32_at(rec), RECORD_SIZE - 4);
        ASSERT_EQ(u32_at(rec + 4), 56);
        ASSERT_EQ(u32_at(rec + 8), 56);
        /* Not blank */
        size_t black = 0;
        for(size_t i = 0; i < SPRITE_BYTES; i++) {
            black += rec[12 + i] != 0;
        }
        ASSERT_TRUE(black > 0);
    }
}

int main(void) {
    FILE* f = fopen(SPRITES_PATH, "rb");
    if(f) {
        sprites_size = fread(sprites, 1, sizeof(sprites), f);
        fclose(f);
    }
    RUN(test_every_species_has_a_sprite);
    RUN(test_records);
    TEST_EXIT();
}
