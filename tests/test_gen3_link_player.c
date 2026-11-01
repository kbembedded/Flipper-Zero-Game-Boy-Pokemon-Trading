#include "test.h"
#include <src/gen3/gen3_text.h>
#include <src/gen3/gen3_link_player.h>

static const uint8_t magic[16] =
    {0x47, 0x61, 0x6D, 0x65, 0x46, 0x72, 0x65, 0x61, 0x6B, 0x20, 0x69, 0x6E, 0x63, 0x2E, 0x00, 0x00};

static void make_lp(Gen3LinkPlayer* lp) {
    memset(lp, 0, sizeof(*lp));
    lp->version = GEN3_VERSION_RUBY;
    lp->lp_field_2 = 0;
    lp->trainer_id = 0x12345678;
    gen3_text_encode("FLIPPER", lp->name, 11, 7);
    lp->gender = 1;
    lp->link_type = GEN3_LINKTYPE_TRADE_SETUP;
    lp->id = 0;
    lp->language = GEN3_LANGUAGE_ENGLISH;
}

TEST(test_lp_build_layout) {
    Gen3LinkPlayer lp;
    make_lp(&lp);
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    ASSERT_MEM_EQ(out, magic, 16);
    ASSERT_EQ(out[0x10], 0x02);
    ASSERT_EQ(out[0x11], 0x40);
    const uint8_t tid[4] = {0x78, 0x56, 0x34, 0x12};
    ASSERT_MEM_EQ(out + 0x14, tid, 4);
    ASSERT_MEM_EQ(out + 0x18, lp.name, 11);
    ASSERT_EQ(out[0x23], 1);
    const uint8_t lt[4] = {0x33, 0x11, 0, 0};
    ASSERT_MEM_EQ(out + 0x24, lt, 4);
    ASSERT_EQ(out[0x2A], 0x02);
    ASSERT_EQ(out[0x2B], 0);
    ASSERT_MEM_EQ(out + 0x2C, magic, 16);
}

TEST(test_lp_roundtrip) {
    Gen3LinkPlayer lp, back;
    make_lp(&lp);
    lp.lp_field_2 = 0x8000;
    lp.id = 1;
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    memset(&back, 0xAA, sizeof(back));
    ASSERT_TRUE(gen3_link_player_block_parse(out, &back));
    ASSERT_EQ(back.version, lp.version);
    ASSERT_EQ(back.lp_field_2, lp.lp_field_2);
    ASSERT_EQ(back.trainer_id, lp.trainer_id);
    ASSERT_MEM_EQ(back.name, lp.name, 11);
    ASSERT_EQ(back.gender, lp.gender);
    ASSERT_EQ(back.link_type, lp.link_type);
    ASSERT_EQ(back.id, lp.id);
    ASSERT_EQ(back.language, lp.language);
}

TEST(test_lp_bad_magic1) {
    Gen3LinkPlayer lp, back;
    make_lp(&lp);
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    out[3] ^= 1;
    ASSERT_TRUE(!gen3_link_player_block_parse(out, &back));
}

TEST(test_lp_bad_magic2) {
    Gen3LinkPlayer lp, back;
    make_lp(&lp);
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    out[0x2C + 3] ^= 1;
    ASSERT_TRUE(!gen3_link_player_block_parse(out, &back));
}

TEST(test_lp_magic_byte15_ignored) {
    Gen3LinkPlayer lp, back;
    make_lp(&lp);
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    out[15] = 0x55;
    out[0x3B] = 0x55;
    ASSERT_TRUE(gen3_link_player_block_parse(out, &back));
}

TEST(test_card_layout) {
    Gen3LinkPlayer lp;
    make_lp(&lp);
    uint8_t out[GEN3_TRAINER_CARD_BLOCK_SIZE];
    memset(out, 0x77, sizeof(out));
    gen3_trainer_card_build(&lp, out);
    ASSERT_EQ(out[0], 1);
    ASSERT_EQ(out[1], 0);
    ASSERT_EQ(out[2], 1);
    ASSERT_EQ(out[0x0E], 0x78);
    ASSERT_EQ(out[0x0F], 0x56);
    for(int i = 0x28; i < 0x30; i++) ASSERT_EQ(out[i], 0xFF);
    ASSERT_MEM_EQ(out + 0x30, lp.name, 8);
    for(int i = 0x24; i < 0x28; i++) ASSERT_EQ(out[i], 0);
    for(int i = 0x38; i < 0x64; i++) ASSERT_EQ(out[i], 0);
    for(int i = 3; i < 0x0E; i++) ASSERT_EQ(out[i], 0);
    for(int i = 0x10; i < 0x24; i++) ASSERT_EQ(out[i], 0);
}

TEST(test_lp_progress_bytes) {
    /* name[8..10] are block 0x20..0x22: FRLG's progressFlags, neverRead, progressFlagsCopy */
    ASSERT_EQ(GEN3_LP_PROGRESS_INDEX, 8);
    ASSERT_EQ(GEN3_FRLG_PROGRESS_NATIONAL_DEX, 0x01);
    ASSERT_EQ(GEN3_FRLG_PROGRESS_CAN_LINK_RS, 0x10);
    Gen3LinkPlayer lp, back;
    make_lp(&lp);
    uint8_t name[8];
    memcpy(name, lp.name, sizeof(name));
    gen3_link_player_set_progress(&lp, 0x11);
    ASSERT_MEM_EQ(lp.name, name, 8);
    uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE];
    gen3_link_player_block_build(&lp, out);
    ASSERT_MEM_EQ(out + 0x18, name, 8);
    ASSERT_EQ(out[0x20], 0x11);
    ASSERT_EQ(out[0x21], 0x00);
    ASSERT_EQ(out[0x22], 0x11);
    ASSERT_EQ(out[0x23], lp.gender);
    memset(&back, 0, sizeof(back));
    ASSERT_TRUE(gen3_link_player_block_parse(out, &back));
    ASSERT_EQ(gen3_link_player_progress(&back), 0x11);
    /* The progress is name[8] alone: block 0x22 is a stale copy on a real GBA */
    back.name[10] = 0x00;
    ASSERT_EQ(gen3_link_player_progress(&back), 0x11);
    gen3_link_player_set_progress(&lp, 0x00);
    ASSERT_EQ(lp.name[8], 0x00);
    ASSERT_EQ(lp.name[9], 0x00);
    ASSERT_EQ(lp.name[10], 0x00);
    ASSERT_EQ(gen3_link_player_progress(&lp), 0x00);
}

TEST(test_card_frlg_layout) {
    ASSERT_EQ(GEN3_VERSION_SAPPHIRE, 0x4001);
    ASSERT_EQ(GEN3_VERSION_EMERALD, 0x4003);
    ASSERT_EQ(GEN3_VERSION_FIRERED, 0x4004);
    ASSERT_EQ(GEN3_VERSION_LEAFGREEN, 0x4005);
    ASSERT_EQ(GEN3_FRLG_CARD_VERSION, 0x38);
    Gen3LinkPlayer lp;
    make_lp(&lp);
    lp.version = GEN3_VERSION_FIRERED;
    lp.gender = 1;
    lp.trainer_id = 22222;
    uint8_t ruby[GEN3_TRAINER_CARD_BLOCK_SIZE];
    uint8_t out[GEN3_TRAINER_CARD_BLOCK_SIZE];
    gen3_trainer_card_build(&lp, ruby);
    memset(out, 0x77, sizeof(out));
    gen3_trainer_card_build_frlg(&lp, out);
    /* 0x00..0x37: FRLG's struct TrainerCardRSE, the Ruby card as it is */
    ASSERT_MEM_EQ(out, ruby, 0x38);
    ASSERT_EQ(out[0x38], 0x04);
    for(int i = 0x39; i < GEN3_TRAINER_CARD_BLOCK_SIZE; i++) ASSERT_EQ(out[i], 0);
    lp.version = GEN3_VERSION_LEAFGREEN;
    memset(out, 0x77, sizeof(out));
    gen3_trainer_card_build_frlg(&lp, out);
    ASSERT_MEM_EQ(out, ruby, 0x38);
    ASSERT_EQ(out[0x38], 0x05);
    for(int i = 0x39; i < GEN3_TRAINER_CARD_BLOCK_SIZE; i++) ASSERT_EQ(out[i], 0);
}

int main(void) {
    RUN(test_lp_build_layout);
    RUN(test_lp_roundtrip);
    RUN(test_lp_bad_magic1);
    RUN(test_lp_bad_magic2);
    RUN(test_lp_magic_byte15_ignored);
    RUN(test_card_layout);
    RUN(test_lp_progress_bytes);
    RUN(test_card_frlg_layout);
    TEST_EXIT();
}
