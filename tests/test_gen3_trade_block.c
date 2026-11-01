#include "link_helpers.h"
#include "bulk_file.h"
#include "gen3_golden.h"
#include "trade_mons.h"
#include <src/gen3/gen3_block.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_trade_block.h>

_Static_assert(GEN3_RUBY_ITEMS_COUNT == 349 && GEN3_ITEM_ENIGMA_BERRY == 175, "D8");

TEST(test_link_type_values) {
    ASSERT_EQ(GEN3_LINKTYPE_TRADE, 0x1111);
    ASSERT_EQ(GEN3_LINKTYPE_TRADE_CONNECTING, 0x1122);
    ASSERT_EQ(GEN3_LINKTYPE_TRADE_SETUP, 0x1133);
    ASSERT_EQ(GEN3_LINKTYPE_TRADE_DISCONNECTED, 0x1144);
    ASSERT_EQ(GEN3_LP_NATIONAL_DEX, 0x8000);
}

TEST(test_trade_block_layout) {
    /* The codes, as §B3.2-B3.3 and §B4.3 list them */
    ASSERT_EQ(GEN3_TRADE_BLOCK_SIZE, 20);
    ASSERT_EQ(GEN3_TRADE_READY, 0xAABB);
    ASSERT_EQ(GEN3_TRADE_CANCEL, 0xEEAA);
    ASSERT_EQ(GEN3_TRADE_CONFIRM, 0xBBBB);
    ASSERT_EQ(GEN3_TRADE_CONFIRM_NO, 0xBBCC);
    ASSERT_EQ(GEN3_TRADE_SET_MONS, 0xDDDD);
    ASSERT_EQ(GEN3_TRADE_PARTNER_CANCEL, 0xEECC);
    ASSERT_EQ(GEN3_TRADE_PLAYER_CANCEL, 0xDDEE);
    ASSERT_EQ(GEN3_TRADE_BOTH_CANCEL, 0xEEBB);
    ASSERT_EQ(GEN3_TRADE_START, 0xCCDD);
    ASSERT_EQ(GEN3_TRADE_ANIM_DONE, 0xABCD);
    ASSERT_EQ(GEN3_TRADE_ANIM_ACK, 0xDCBA);

    uint8_t b[GEN3_TRADE_BLOCK_SIZE];
    memset(b, 0xA5, sizeof(b));
    gen3_trade_block_build(GEN3_TRADE_SET_MONS, 3, b);
    const uint8_t exp[GEN3_TRADE_BLOCK_SIZE] = {0xDD, 0xDD, 0x03, 0x00};
    ASSERT_MEM_EQ(b, exp, sizeof(exp));
    uint16_t arg = 0xFFFF;
    ASSERT_EQ(gen3_trade_block_code(b, &arg), 0xDDDD);
    ASSERT_EQ(arg, 3);
    ASSERT_EQ(gen3_trade_block_code(b, NULL), 0xDDDD);

    /* Little-endian, low byte first */
    gen3_trade_block_build(GEN3_TRADE_READY, 0x0102, b);
    const uint8_t exp2[GEN3_TRADE_BLOCK_SIZE] = {0xBB, 0xAA, 0x02, 0x01};
    ASSERT_MEM_EQ(b, exp2, sizeof(exp2));

    /* G doesn't clear linkData between sends: words 2..9 are stale and ignored */
    uint8_t in[GEN3_TRADE_BLOCK_SIZE];
    memset(in, 0x5A, sizeof(in));
    in[0] = 0xBB;
    in[1] = 0xAA;
    in[2] = 0x05;
    in[3] = 0x00;
    ASSERT_EQ(gen3_trade_block_code(in, &arg), GEN3_TRADE_READY);
    ASSERT_EQ(arg, 5);
}

TEST(test_trade_block_on_wire) {
    Gen3Link l;
    uint8_t b[GEN3_TRADE_BLOCK_SIZE];
    gen3_link_init(&l);
    connect(&l);
    gen3_trade_block_build(GEN3_TRADE_SET_MONS, 3, b);
    ASSERT_TRUE(gen3_block_send(&l, b, GEN3_TRADE_BLOCK_SIZE, 0));
    ASSERT_EQ(gen3_link_queued(&l), 3);
    const uint16_t exp[3][GEN3_CMD_LEN] = {
        {0xBBBB, 0x0014, 0x0080, 0, 0, 0, 0, 0},
        {0x8888, 0xDDDD, 0x0003, 0, 0, 0, 0, 0},
        {0x8888, 0, 0, 0, 0, 0, 0, 0},
    };
    for(int i = 0; i < 3; i++) {
        MlinkTx tx;
        uint16_t cmd[GEN3_CMD_LEN];
        burst(&l, &tx, NULL, cmd);
        ASSERT_MEM_EQ(&tx.word[1], exp[i], sizeof(exp[i]));
    }
    ASSERT_EQ(gen3_link_queued(&l), 0);
}

TEST(test_trade_rounds) {
    const uint16_t request[GEN3_TRADE_ROUNDS] = {1, 1, 1, 3, 4};
    const uint16_t size[GEN3_TRADE_ROUNDS] = {200, 200, 200, 220, 40};
    ASSERT_EQ(GEN3_TRADE_ROUNDS, 5);
    ASSERT_EQ(GEN3_TRADE_ROUND_MAX, 220);
    for(uint8_t r = 1; r <= GEN3_TRADE_ROUNDS; r++) {
        ASSERT_EQ(gen3_trade_round_request(r), request[r - 1]);
        ASSERT_EQ(gen3_trade_round_size(r), size[r - 1]);
        ASSERT_TRUE(gen3_trade_round_size(r) <= GEN3_TRADE_ROUND_MAX);
    }
    ASSERT_EQ(gen3_trade_round_request(0), 0xFFFF);
    ASSERT_EQ(gen3_trade_round_size(0), 0);
    ASSERT_EQ(gen3_trade_round_request(6), 0xFFFF);
    ASSERT_EQ(gen3_trade_round_size(6), 0);
    ASSERT_EQ(gen3_trade_round_request(255), 0xFFFF);
    ASSERT_EQ(gen3_trade_round_size(255), 0);
}

/* gen3_trade_round_block(round, mon) writes exactly gen3_trade_round_size(round) bytes, all zero
 * except mon at [0..99] of round 1. */
static bool round_block_is(uint8_t round, const uint8_t* mon, uint16_t size) {
    uint8_t out[GEN3_TRADE_ROUND_MAX + 1];
    memset(out, 0xA5, sizeof(out));
    gen3_trade_round_block(round, mon, out);
    for(uint16_t i = 0; i < size; i++) {
        uint8_t want = (mon && i < GEN3_PARTY_SIZE) ? mon[i] : 0;
        if(out[i] != want) {
            printf("  round %u: byte %u is 0x%02X, want 0x%02X\n", round, i, out[i], want);
            return false;
        }
    }
    if(out[size] != 0xA5) {
        printf("  round %u: wrote past %u bytes\n", round, size);
        return false;
    }
    return true;
}

TEST(test_trade_round_block) {
    uint8_t mon[GEN3_PARTY_SIZE];
    golden_game(mon);
    ASSERT_TRUE(round_block_is(1, mon, 200));
    ASSERT_TRUE(round_block_is(1, NULL, 200));
    ASSERT_TRUE(round_block_is(2, NULL, 200));
    ASSERT_TRUE(round_block_is(3, NULL, 200));
    ASSERT_TRUE(round_block_is(4, NULL, 220));
    ASSERT_TRUE(round_block_is(5, NULL, 40));
    /* mon goes only into round 1 */
    for(uint8_t r = 2; r <= GEN3_TRADE_ROUNDS; r++) {
        uint8_t out[GEN3_TRADE_ROUND_MAX];
        memset(out, 0xA5, sizeof(out));
        gen3_trade_round_block(r, mon, out);
        for(uint16_t i = 0; i < gen3_trade_round_size(r); i++) ASSERT_EQ(out[i], 0);
    }
    /* Not a round: nothing written */
    ASSERT_TRUE(round_block_is(0, NULL, 0));
    ASSERT_TRUE(round_block_is(6, NULL, 0));
}

static Gen3Refuse offer_with(Gen3Field f, uint32_t value) {
    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, f, value);
    gen3_pkm_fix_checksum(&p);
    return gen3_trade_offer_check(&p);
}

TEST(test_offer_check) {
    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_fix_checksum(&p);
    ASSERT_EQ(gen3_trade_offer_check(&p), Gen3RefuseNone);

    ASSERT_EQ(offer_with(Gen3FieldHeldItem, 200), Gen3RefuseNone);
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, 348), Gen3RefuseNone);
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, 120), Gen3RefuseNone);
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, 133), Gen3RefuseNone);
    /* O3: D8's ranges stand as written. Ruby holds any item below ITEMS_COUNT, a key item or an
     * HM included; only the editor keeps to holdable ones. */
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, 265), Gen3RefuseNone); /* a key item */
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, 339), Gen3RefuseNone); /* HM01 */
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, GEN3_ITEM_MAIL_FIRST), Gen3RefuseMail);
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, GEN3_ITEM_MAIL_LAST), Gen3RefuseMail);
    ASSERT_EQ(offer_with(Gen3FieldMail, 0), Gen3RefuseMail);
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, GEN3_ITEM_ENIGMA_BERRY), Gen3RefuseItem);
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, GEN3_RUBY_ITEMS_COUNT), Gen3RefuseItem);
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, GEN3_ITEM_COUNT - 1), Gen3RefuseItem);
    ASSERT_EQ(offer_with(Gen3FieldIsEgg, 1), Gen3RefuseEgg);
    ASSERT_EQ(offer_with(Gen3FieldEggFlag, 1), Gen3RefuseEgg);
    ASSERT_EQ(offer_with(Gen3FieldBadEgg, 1), Gen3RefuseBadEgg);
    ASSERT_EQ(offer_with(Gen3FieldSpecies, 252), Gen3RefuseBadData); /* internal: OLD_UNOWN_B */
    ASSERT_EQ(offer_with(Gen3FieldSpecies, 0), Gen3RefuseBadData);
    ASSERT_EQ(offer_with(Gen3FieldMove2, 355), Gen3RefuseBadData);
    ASSERT_EQ(offer_with(Gen3FieldHeldItem, GEN3_ITEM_COUNT), Gen3RefuseBadData);

    /* Order: gen3_pkm_check, then mail, then item */
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldIsEgg, 1);
    gen3_pkm_set(&p, Gen3FieldHeldItem, GEN3_ITEM_MAIL_FIRST);
    ASSERT_EQ(gen3_trade_offer_check(&p), Gen3RefuseEgg);
    gen3_pkm_set(&p, Gen3FieldBadEgg, 1);
    ASSERT_EQ(gen3_trade_offer_check(&p), Gen3RefuseBadEgg);
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldMail, 0);
    gen3_pkm_set(&p, Gen3FieldHeldItem, GEN3_ITEM_ENIGMA_BERRY);
    ASSERT_EQ(gen3_trade_offer_check(&p), Gen3RefuseMail);
}

TEST(test_check_incoming) {
    uint8_t game[GEN3_PARTY_SIZE];
    Gen3Pkm scratch;

    golden_game(game);
    memset(&scratch, 0, sizeof(scratch));
    ASSERT_EQ(gen3_trade_check_incoming(game, &scratch), Gen3RefuseNone);
    ASSERT_MEM_EQ(scratch.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    memset(game, 0, sizeof(game));
    ASSERT_EQ(gen3_trade_check_incoming(game, &scratch), Gen3RefuseEmpty);

    /* A failed decrypt leaves scratch as the previous decrypt left it (the club's scratch relies
     * on it) */
    golden_game(game);
    ASSERT_EQ(gen3_trade_check_incoming(game, &scratch), Gen3RefuseNone);
    game[0x30] ^= 1;
    ASSERT_EQ(gen3_trade_check_incoming(game, &scratch), Gen3RefuseBadEgg);
    ASSERT_MEM_EQ(scratch.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    golden_game(game);
    game[0x55] = 0; /* party mail index, outside the checksum */
    ASSERT_EQ(gen3_trade_check_incoming(game, &scratch), Gen3RefuseMail);

    tm_make_egg(game);
    ASSERT_EQ(gen3_trade_check_incoming(game, &scratch), Gen3RefuseEgg);
    tm_make_mail(game);
    ASSERT_EQ(gen3_trade_check_incoming(game, &scratch), Gen3RefuseMail);
    tm_make_bad_egg(game);
    ASSERT_EQ(gen3_trade_check_incoming(game, &scratch), Gen3RefuseBadEgg);

    tm_make(252, 5, 9, game);
    ASSERT_EQ(gen3_trade_check_incoming(game, &scratch), Gen3RefuseNone);
    ASSERT_EQ(gen3_national_from_internal(gen3_pkm_get(&scratch, Gen3FieldSpecies)), 252);
    ASSERT_EQ(gen3_pkm_get(&scratch, Gen3FieldLevel), 5);
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_link_type_values);
    RUN(test_trade_block_layout);
    RUN(test_trade_block_on_wire);
    RUN(test_trade_rounds);
    RUN(test_trade_round_block);
    RUN(test_offer_check);
    RUN(test_check_incoming);
    TEST_EXIT();
}
