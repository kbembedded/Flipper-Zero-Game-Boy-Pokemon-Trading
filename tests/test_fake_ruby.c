#include "test.h"
#include "fake_ruby.h"
#include <stdbool.h>
#include <src/gen3/gen3_block.h>
#include <src/gen3/gen3_link.h>
#include <src/gen3/gen3_text.h>

/* Self-checks of the fake Ruby child: it must be as strict as the real one (§A2-A4). */

static void make_player(Gen3LinkPlayer* lp, const char* name, uint32_t trainer_id) {
    memset(lp, 0, sizeof(*lp));
    lp->version = GEN3_VERSION_RUBY;
    lp->trainer_id = trainer_id;
    gen3_text_encode(name, lp->name, sizeof(lp->name), 7);
    lp->link_type = GEN3_LINKTYPE_TRADE_SETUP;
    lp->language = GEN3_LANGUAGE_ENGLISH;
}

static void fake_init(FakeRuby* g) {
    Gen3LinkPlayer me;
    make_player(&me, "RUBY", (54321u << 16) | 12345u);
    fake_ruby_init(g, &me);
}

static uint16_t one_transfer(FakeRuby* g, uint16_t w) {
    MlinkTx tx;
    MlinkRx rx;
    memset(&tx, 0, sizeof(tx));
    tx.count = 1;
    tx.word[0] = w;
    fake_ruby_frame(g, &tx, &rx);
    return rx.word[0];
}

/* One master frame through gen3_link; xor0 flips bits of transfer #1 (the checksum slot). */
static void master_frame(Gen3Link* l, FakeRuby* g, uint16_t xor0, MlinkRx* rx) {
    MlinkTx tx;
    uint16_t cmd[GEN3_CMD_LEN];
    gen3_link_tx(l, &tx);
    tx.word[0] ^= xor0;
    fake_ruby_frame(g, &tx, rx);
    gen3_link_rx(l, rx, cmd);
}

static bool master_connect(Gen3Link* l, FakeRuby* g) {
    MlinkRx rx;
    gen3_link_init(l);
    fake_ruby_open_link(g);
    for(int i = 0; i < 20 && !gen3_link_connected(l); i++) master_frame(l, g, 0, &rx);
    return gen3_link_connected(l);
}

TEST(test_fake_handshake_timeline) {
    FakeRuby g;
    fake_init(&g);
    ASSERT_EQ(one_transfer(&g, 0xB9A0), 0xFFFF); /* link closed */
    fake_ruby_open_link(&g);
    ASSERT_EQ(fake_ruby_phase(&g), FakeRubyHandshake);
    ASSERT_EQ(one_transfer(&g, 0xB9A0), 0x0000); /* OpenLink enabled serial; frame 1 disables it */
    for(int i = 0; i < 5; i++) ASSERT_EQ(one_transfer(&g, 0xB9A0), 0xFFFF);
    ASSERT_EQ(one_transfer(&g, 0xB9A0), 0x0000); /* first transfer after EnableSerial (§A3) */
    ASSERT_EQ(one_transfer(&g, 0xB9A0), 0xB9A0);
    ASSERT_EQ(fake_ruby_phase(&g), FakeRubyHandshake);
    ASSERT_EQ(one_transfer(&g, 0x8FFF), 0xB9A0); /* count 2 twice and slot 0 = 8FFF: success */
    ASSERT_EQ(fake_ruby_phase(&g), FakeRubyLinkup);

    MlinkTx tx;
    MlinkRx rx;
    memset(&tx, 0, sizeof(tx));
    tx.count = MLINK_BURST_LEN;
    tx.word[0] = 0x1234; /* first burst: checksum not checked */
    fake_ruby_frame(&g, &tx, &rx);
    ASSERT_EQ(rx.word[0], 0xB9A0); /* stale handshake word */
    for(int t = 1; t < MLINK_BURST_LEN; t++) ASSERT_EQ(rx.word[t], 0);
    ASSERT_TRUE(fake_ruby_error(&g) == NULL);
}

TEST(test_fake_checksum_checked) {
    FakeRuby g;
    Gen3Link l;
    MlinkRx rx;
    fake_init(&g);
    ASSERT_TRUE(master_connect(&l, &g));
    master_frame(&l, &g, 0x0001, &rx); /* first burst: not checked */
    master_frame(&l, &g, 0, &rx);
    master_frame(&l, &g, 0, &rx);
    ASSERT_TRUE(fake_ruby_error(&g) == NULL);
    ASSERT_EQ(rx.word[0], 0); /* its checksum of the previous all-zero burst */
    master_frame(&l, &g, 0x0100, &rx);
    ASSERT_TRUE(fake_ruby_error(&g) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&g), "checksum");
    master_frame(&l, &g, 0, &rx);
    for(int t = 0; t < MLINK_BURST_LEN; t++) ASSERT_EQ(rx.word[t], 0xFFFF);
}

TEST(test_fake_lag) {
    FakeRuby g;
    Gen3Link l;
    MlinkRx rx;
    MlinkTx none;
    fake_init(&g);
    ASSERT_TRUE(master_connect(&l, &g));
    master_frame(&l, &g, 0, &rx);
    memset(&none, 0, sizeof(none)); /* a frame without transfers */
    for(int i = 0; i < 10; i++) fake_ruby_frame(&g, &none, &rx);
    ASSERT_TRUE(fake_ruby_error(&g) == NULL);
    fake_ruby_frame(&g, &none, &rx);
    ASSERT_TRUE(fake_ruby_error(&g) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&g), "lag");
}

TEST(test_fake_bad_magic) {
    FakeRuby g;
    Gen3Link l;
    MlinkRx rx;
    Gen3LinkPlayer lp;
    uint8_t block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    const uint16_t req[GEN3_CMD_LEN] = {0x2222, 0x1133};
    fake_init(&g);
    ASSERT_TRUE(master_connect(&l, &g));
    make_player(&lp, "FLIPPER", 22222);
    gen3_link_player_block_build(&lp, block);
    block[0x2C + 4] ^= 0x20; /* "GameFreak" -> "Gamefreak" in magic2 */
    ASSERT_TRUE(gen3_link_send(&l, req));
    ASSERT_TRUE(gen3_block_send(&l, block, sizeof(block), 0));
    for(int i = 0; i < 20; i++) master_frame(&l, &g, 0, &rx);
    ASSERT_TRUE(fake_ruby_error(&g) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&g), "magic");
}

TEST(test_fake_block_pacing) {
    FakeRuby g;
    Gen3Link l;
    MlinkRx rx;
    Gen3LinkPlayer lp;
    uint8_t block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    uint16_t g_cmd[12][GEN3_CMD_LEN];
    const uint16_t req[GEN3_CMD_LEN] = {0x2222, 0x1133};
    fake_init(&g);
    ASSERT_TRUE(master_connect(&l, &g));
    make_player(&lp, "FLIPPER", 22222);
    gen3_link_player_block_build(&lp, block);
    ASSERT_TRUE(gen3_link_send(&l, req));
    ASSERT_TRUE(gen3_block_send(&l, block, sizeof(block), 0));
    for(int i = 0; i < 12; i++) {
        master_frame(&l, &g, 0, &rx);
        memcpy(g_cmd[i], &rx.word[1], sizeof(g_cmd[i]));
    }
    ASSERT_TRUE(fake_ruby_error(&g) == NULL);
    /* burst 0 carries 2222; G answers in burst 2, then two idle bursts, then 5 chunks (§A4) */
    ASSERT_EQ(g_cmd[0][0], 0);
    ASSERT_EQ(g_cmd[1][0], 0);
    const uint16_t header[GEN3_CMD_LEN] = {0xBBBB, 0x003C, 0x0081, 0, 0, 0, 0, 0};
    ASSERT_MEM_EQ(g_cmd[2], header, sizeof(header));
    ASSERT_EQ(g_cmd[3][0], 0);
    ASSERT_EQ(g_cmd[4][0], 0);
    for(int i = 5; i < 10; i++) ASSERT_EQ(g_cmd[i][0], 0x8888);
    ASSERT_EQ(g_cmd[10][0], 0);
    ASSERT_EQ(g_cmd[5][1], 0x6147); /* "Ga", little-endian */

    char name[16];
    gen3_text_decode(fake_ruby_partner(&g)->name, 11, name, sizeof(name));
    ASSERT_STR_EQ(name, "FLIPPER");
}

/* Scripted master: player blocks, then 0xCCCC 0002 and cards, until G has sent its 0x2FFE and
 * sits in LinkCB_StandbyForAll (a non-NULL gLinkCallback) waiting for ours. */
static bool master_to_standby(Gen3Link* l, FakeRuby* g) {
    MlinkRx rx;
    Gen3LinkPlayer lp;
    uint8_t block[GEN3_TRAINER_CARD_BLOCK_SIZE];
    const uint16_t req_players[GEN3_CMD_LEN] = {0x2222, 0x1133};
    const uint16_t req_cards[GEN3_CMD_LEN] = {0xCCCC, 0x0002};
    if(!master_connect(l, g)) return false;
    make_player(&lp, "FLIPPER", 22222);
    gen3_link_player_block_build(&lp, block);
    if(!gen3_link_send(l, req_players)) return false;
    if(!gen3_block_send(l, block, GEN3_LINK_PLAYER_BLOCK_SIZE, 0)) return false;
    for(int i = 0; i < 30; i++) master_frame(l, g, 0, &rx);
    gen3_trainer_card_build(&lp, block);
    if(!gen3_link_send(l, req_cards)) return false;
    if(!gen3_block_send(l, block, GEN3_TRAINER_CARD_BLOCK_SIZE, 0)) return false;
    for(int i = 0; i < 200; i++) {
        master_frame(l, g, 0, &rx);
        if(rx.word[1] == 0x2FFE) return fake_ruby_error(g) == NULL;
    }
    return false;
}

/* Sends one stray request while G waits in standby; true if G starts a block with `header`. */
static bool stray_request_starts_block(uint16_t w0, uint16_t w1, const uint16_t header[3]) {
    FakeRuby g;
    Gen3Link l;
    MlinkRx rx;
    const uint16_t req[GEN3_CMD_LEN] = {w0, w1};
    fake_init(&g);
    if(!master_to_standby(&l, &g)) return false;
    if(fake_ruby_phase(&g) != FakeRubyWalkIn) return false;
    if(!gen3_link_send(&l, req)) return false;
    for(int i = 0; i < 4; i++) { /* request on the wire, then G's reply 2 bursts later */
        master_frame(&l, &g, 0, &rx);
        if(rx.word[1] == header[0] && rx.word[2] == header[1] && rx.word[3] == header[2]) {
            return fake_ruby_error(&g) == NULL;
        }
    }
    return false;
}

TEST(test_fake_stray_card_request_starts_block) {
    /* InitBlockSend replaces LinkCB_StandbyForAll: G sends its card again (§A4, A7) */
    const uint16_t header[3] = {0xBBBB, 0x0064, 0x0081};
    ASSERT_TRUE(stray_request_starts_block(0xCCCC, 0x0002, header));
}

TEST(test_fake_stray_link_type_starts_block) {
    /* a duplicate 0x2222 makes G send a second LinkPlayerBlock (§A4) */
    const uint16_t header[3] = {0xBBBB, 0x003C, 0x0081};
    ASSERT_TRUE(stray_request_starts_block(0x2222, 0x1133, header));
}

/* master_to_standby, then the master's 2FFE: G enters the room and sends CAFE 0011. */
static bool master_to_room(Gen3Link* l, FakeRuby* g) {
    MlinkRx rx;
    const uint16_t standby[GEN3_CMD_LEN] = {0x2FFE};
    if(!master_to_standby(l, g)) return false;
    if(!gen3_link_send(l, standby)) return false;
    for(int i = 0; i < 20; i++) {
        master_frame(l, g, 0, &rx);
        if(rx.word[1] == 0xCAFE && rx.word[2] == 0x0011) {
            return fake_ruby_phase(g) == FakeRubyRoom && fake_ruby_error(g) == NULL;
        }
    }
    return false;
}

/* G leaves the room (§B7). The master answers G's CAFE 0017 `gap` bursts after the first burst
 * it could use (gap < 0: never), and G's 5FFF at once. G's phase 600 frames later, or
 * FakeRubyOff if G errored or never reached the room. */
static FakeRubyPhase leave_room_answering(int gap) {
    FakeRuby g;
    Gen3Link l;
    MlinkRx rx;
    const uint16_t exit_room[GEN3_CMD_LEN] = {0xCAFE, 0x0017};
    const uint16_t close[GEN3_CMD_LEN] = {0x5FFF};
    int first_usable = -1; /* the burst right after the one carrying G's 0x17 */
    fake_init(&g);
    if(!master_to_room(&l, &g)) return FakeRubyOff;
    fake_ruby_leave_room(&g);
    for(int i = 0; i < 600; i++) {
        if(first_usable >= 0 && gap >= 0 && i == first_usable + gap) gen3_link_send(&l, exit_room);
        master_frame(&l, &g, 0, &rx);
        if(first_usable < 0 && rx.word[1] == 0xCAFE && rx.word[2] == 0x0017) first_usable = i + 1;
        if(rx.word[1] == 0x5FFF) gen3_link_send(&l, close);
    }
    return fake_ruby_error(&g) == NULL ? fake_ruby_phase(&g) : FakeRubyOff;
}

TEST(test_fake_leave_waits_for_partner) {
    /* No CAFE 0017 from the master: G waits for all EXITING_ROOM with no timeout and no error. */
    ASSERT_EQ(leave_room_answering(-1), FakeRubyLeaving);
}

TEST(test_fake_leave_drops_hasty_answer) {
    /* sub_8054FF8: in the update after G's own 0x17, the IDLE partner is forced BUSY and that
     * update's key is dropped. An answer in the very next burst is lost; one burst later works. */
    ASSERT_EQ(leave_room_answering(0), FakeRubyLeaving);
    ASSERT_EQ(leave_room_answering(1), FakeRubyClosed);
}

TEST(test_fake_corrupt_next) {
    FakeRuby g;
    Gen3Link l;
    MlinkTx tx;
    MlinkRx rx;
    uint16_t cmd[GEN3_CMD_LEN];
    Gen3LinkEvent ev = Gen3LinkEventNone;
    fake_init(&g);
    ASSERT_TRUE(master_to_room(&l, &g));
    fake_ruby_corrupt_next(&g);
    for(int i = 0; i < 3 && ev != Gen3LinkEventChecksumError; i++) {
        gen3_link_tx(&l, &tx);
        fake_ruby_frame(&g, &tx, &rx);
        ev = gen3_link_rx(&l, &rx, cmd);
    }
    ASSERT_EQ(ev, Gen3LinkEventChecksumError); /* the master's check of G's checksum fails */
    ASSERT_TRUE(fake_ruby_error(&g) != NULL); /* and G's check of the master's */
    ASSERT_STR_EQ(fake_ruby_error(&g), "checksum");
}

TEST(test_fake_corrupt_next_checksum) {
    FakeRuby g;
    Gen3Link l;
    MlinkTx tx;
    MlinkRx rx;
    uint16_t cmd[GEN3_CMD_LEN];
    fake_init(&g);
    ASSERT_TRUE(master_to_room(&l, &g));
    fake_ruby_corrupt_next_checksum(&g);
    gen3_link_tx(&l, &tx);
    fake_ruby_frame(&g, &tx, &rx);
    /* the master's check of G's checksum fails */
    ASSERT_EQ(gen3_link_rx(&l, &rx, cmd), Gen3LinkEventChecksumError);
    ASSERT_EQ(rx.word[1], 0xCAFE); /* the burst's own command words are intact */
    for(int i = 0; i < 60; i++) {
        gen3_link_tx(&l, &tx);
        fake_ruby_frame(&g, &tx, &rx);
        ASSERT_TRUE(gen3_link_rx(&l, &rx, cmd) != Gen3LinkEventChecksumError); /* one burst */
    }
    ASSERT_TRUE(fake_ruby_error(&g) == NULL); /* G's own check passed: it stays healthy */
    ASSERT_EQ(fake_ruby_phase(&g), FakeRubyRoom);
}

int main(void) {
    RUN(test_fake_handshake_timeline);
    RUN(test_fake_checksum_checked);
    RUN(test_fake_lag);
    RUN(test_fake_bad_magic);
    RUN(test_fake_block_pacing);
    RUN(test_fake_stray_card_request_starts_block);
    RUN(test_fake_stray_link_type_starts_block);
    RUN(test_fake_leave_waits_for_partner);
    RUN(test_fake_leave_drops_hasty_answer);
    RUN(test_fake_corrupt_next);
    RUN(test_fake_corrupt_next_checksum);
    TEST_EXIT();
}
