#include "test.h"
#include "bulk_file.h"
#include "club_bench.h"
#include <stdbool.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_text.h>

TEST(test_club_reaches_cards) {
    Bench b;
    char name[16];
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&b.ruby);
    run(&b, 400);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(at_cards_or_later(st.phase));
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_EQ(st.link.checksum_errors, 0);
    ASSERT_TRUE(st.partner_valid);
    gen3_text_decode(st.partner.name, sizeof(st.partner.name), name, sizeof(name));
    ASSERT_STR_EQ(name, "RUBY");
    ASSERT_EQ(st.partner.trainer_id & 0xFFFF, 12345);
    ASSERT_EQ(st.partner.trainer_id >> 16, 54321);
    ASSERT_EQ(st.partner.link_type, GEN3_LINKTYPE_TRADE_SETUP);

    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    gen3_text_decode(p->name, sizeof(p->name), name, sizeof(name));
    ASSERT_STR_EQ(name, "FLIPPER");
    ASSERT_EQ(p->trainer_id, 22222);
    ASSERT_EQ(p->version, GEN3_VERSION_RUBY);
    ASSERT_EQ(p->link_type, GEN3_LINKTYPE_TRADE_SETUP);
    ASSERT_EQ(p->language, GEN3_LANGUAGE_ENGLISH);
    ASSERT_EQ(p->gender, 0);
    ASSERT_EQ(p->lp_field_2, 0);
    ASSERT_EQ(p->id, 0);
    FakeRubyPhase gp = fake_ruby_phase(&b.ruby);
    ASSERT_TRUE(gp == FakeRubyWalkIn || gp == FakeRubyRoom); /* both cards are in */

    /* §A4: 0x2222 and 0xCCCC exactly once each */
    ASSERT_EQ(b.sent_2222, 1);
    ASSERT_EQ(b.sent_cccc, 1);
}

TEST(test_club_gba_not_ready) {
    Bench b;
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    run(&b, 600);
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyOff);
    fake_ruby_open_link(&b.ruby);
    run(&b, 400);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_TRUE(at_cards_or_later(club_status(&b).phase));
}

TEST(test_club_other_room) {
    Bench b;
    bench_init(&b, 0x2233); /* the GBA player chose the Colosseum */
    fake_ruby_open_link(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 400));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorOtherRoom);
    ASSERT_TRUE(!st.partner_valid);
    ASSERT_EQ(st.link_type, 0x2233);
    ASSERT_EQ(b.sent_5fff, 0);

    run(&b, 400);
    ASSERT_EQ(b.sent_5fff, 1);
    ASSERT_EQ(b.sent_cccc, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    /* D2: any other room gets a 0x1133 block */
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->link_type, GEN3_LINKTYPE_TRADE_SETUP);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorOtherRoom);
}

TEST(test_club_link_test_refuses_trade_types) {
    /* D2: a Link Test (trading false) answers every Part B link type with a 0x1133 block, so the
     * GBA stalls before anything happens, and stops with Error/Unexpected. */
    static const uint32_t types[] = {
        GEN3_LINKTYPE_TRADE, GEN3_LINKTYPE_TRADE_CONNECTING, GEN3_LINKTYPE_TRADE_DISCONNECTED};
    for(size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        Bench b;
        bench_init(&b, types[i]);
        fake_ruby_open_link(&b.ruby);
        ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 400));
        Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.last_error, Gen3ClubErrorUnexpected);
        ASSERT_EQ(st.link_type, types[i]);
        ASSERT_TRUE(!st.partner_valid);
        ASSERT_TRUE(!st.trainer_valid);
        run(&b, 400);
        ASSERT_EQ(b.sent_5fff, 1);
        ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        ASSERT_EQ(fake_ruby_partner(&b.ruby)->link_type, GEN3_LINKTYPE_TRADE_SETUP);
        ASSERT_EQ(club_status(&b).last_error, Gen3ClubErrorUnexpected);
    }
}

/* A raw GBA (slot 1) on the club's wire, for a block the fake never sends: it answers the
 * handshake, then puts its next queued command (or zeros) in each burst, with the previous
 * burst's checksum in transfer #1 (§A2). The club's blocks are logged as on the bench. */
typedef struct {
    Gen3Club club;
    MlinkRx rx;
    bool started;
    uint16_t sum; /* this burst's checksum, sent in the next burst's transfer #1 */
    uint16_t cmds[8][GEN3_CMD_LEN];
    uint8_t n_cmds, next_cmd;
    uint32_t bursts;
    Gen3BlockRx f_rx;
    uint32_t f_rx_first;
    uint16_t f_rx_id;
    BenchBlock f_blocks[4];
    uint8_t n_f_blocks;
} RawGba;

static void raw_gba_step(RawGba* r) {
    MlinkTx tx;
    gen3_club_frame(&r->club, r->started ? &r->rx : NULL, &tx);
    r->started = true;
    memset(&r->rx, 0, sizeof(r->rx));
    r->rx.count = tx.count;
    if(tx.count != MLINK_BURST_LEN) {
        r->rx.word[0] = GEN3_SLAVE_HANDSHAKE;
        return;
    }
    r->rx.word[0] = r->sum;
    if(r->next_cmd < r->n_cmds) {
        memcpy(&r->rx.word[1], r->cmds[r->next_cmd++], sizeof(r->cmds[0]));
    }
    uint16_t sum = 0;
    for(int i = 1; i < MLINK_BURST_LEN; i++) sum = (uint16_t)(sum + tx.word[i] + r->rx.word[i]);
    r->sum = sum;
    bench_parse(
        &r->f_rx,
        &r->f_rx_first,
        &r->f_rx_id,
        r->f_blocks,
        &r->n_f_blocks,
        &tx.word[1],
        ++r->bursts);
}

TEST(test_club_bad_magic_still_replies) {
    /* The GBA's LinkPlayerBlock fails the magic check: the Flipper still sends its own block, so
     * the GBA (which checks the Flipper's magic, not its own) never waits for one. */
    static RawGba r;
    Gen3LinkPlayer gba;
    Gen3LinkPlayer flipper;
    uint8_t block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    memset(&r, 0, sizeof(r));
    gen3_club_init(&r.club, &club_cfg);
    memset(&gba, 0, sizeof(gba));
    gba.version = GEN3_VERSION_RUBY;
    gba.trainer_id = 12345;
    gen3_text_encode("RUBY", gba.name, sizeof(gba.name), 7);
    gba.link_type = GEN3_LINKTYPE_TRADE_SETUP;
    gba.language = GEN3_LANGUAGE_ENGLISH;
    gen3_link_player_block_build(&gba, block);
    block[0x2C + 4] ^= 0x20; /* "GameFreak" -> "Gamefreak" in magic2 */
    r.cmds[0][0] = GEN3_CMD_INIT_BLOCK;
    r.cmds[0][1] = GEN3_LINK_PLAYER_BLOCK_SIZE;
    r.cmds[0][2] = 0x81;
    for(uint8_t c = 0; c < 5; c++) {
        r.cmds[1 + c][0] = GEN3_CMD_CONT_BLOCK;
        for(int w = 0; w < 7; w++) {
            size_t off = (size_t)c * GEN3_BLOCK_CHUNK_BYTES + 2 * w;
            uint16_t lo = off < sizeof(block) ? block[off] : 0;
            uint16_t hi = off + 1 < sizeof(block) ? block[off + 1] : 0;
            r.cmds[1 + c][1 + w] = (uint16_t)(lo | (hi << 8));
        }
    }
    r.n_cmds = 6;

    for(int i = 0; i < 40; i++) raw_gba_step(&r);
    Gen3ClubStatus st;
    gen3_club_status(&r.club, &st);
    ASSERT_EQ(st.phase, Gen3ClubPhaseError);
    ASSERT_EQ(st.last_error, Gen3ClubErrorBadPlayerBlock);
    ASSERT_TRUE(!st.partner_valid);
    ASSERT_EQ(st.link.checksum_errors, 0);
    ASSERT_EQ(r.n_f_blocks, 1);
    ASSERT_EQ(r.f_blocks[0].size, GEN3_LINK_PLAYER_BLOCK_SIZE);
    ASSERT_TRUE(gen3_link_player_block_parse(r.f_rx.data, &flipper));
    ASSERT_EQ(flipper.link_type, GEN3_LINKTYPE_TRADE_SETUP);
    ASSERT_EQ(flipper.trainer_id, 22222);
    ASSERT_EQ(gen3_link_queued(&r.club.link), 0);
}

TEST(test_club_card_delay_needed) {
    Bench b;
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    gen3_club_set_card_delay(&b.club, 0);
    fake_ruby_open_link(&b.ruby);
    run(&b, 400);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stale card");
}

TEST(test_club_room_idle) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    run(&b, 3600);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_TRUE(st.room_frames >= 3600);
    ASSERT_EQ(st.link.checksum_errors, 0);
    ASSERT_EQ(b.sent_cafe, 0); /* idle is all-zero commands, not CAFE 0011 (U6) */
}

TEST(test_club_answers_2ffe_once) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    run(&b, 600);
    ASSERT_EQ(b.sent_2ffe, 1);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
}

TEST(test_club_leave_room) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    ASSERT_TRUE(leave_room(&b));
    run(&b, 120);
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_EQ(b.sent_cafe, 1); /* nothing but that answer */
    ASSERT_EQ(b.sent_5fff, 1);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
}

TEST(test_club_second_linkup) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    ASSERT_TRUE(leave_room(&b));
    ruby_relink(&b);
    ASSERT_TRUE(run_until_room(&b, 400));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_TRUE(st.partner_valid);
    ASSERT_EQ(b.sent_2222, 2);
    ASSERT_EQ(b.sent_cccc, 2);
    ASSERT_EQ(b.sent_2ffe, 2);
}

TEST(test_club_gba_reset) {
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    fake_ruby_power_off(&b.ruby);
    run(&b, 120);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_TRUE(!st.partner_valid);
    ruby_relink(&b);
    ASSERT_TRUE(run_until_room(&b, 400));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_club_checksum_error) {
    /* A glitch in G's command words: both checksums diverge (§A2). The club keeps its phase and
     * its queue; G fails its own check and closes the link, which reads as Disconnected. */
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    fake_ruby_corrupt_next(&b.ruby);
    bool waiting = false;
    for(int i = 0; i < 10 && !waiting; i++) {
        step(&b);
        Gen3ClubPhase phase = club_status(&b).phase;
        ASSERT_TRUE(phase == Gen3ClubPhaseRoom || phase == Gen3ClubPhaseWaiting);
        waiting = phase == Gen3ClubPhaseWaiting;
    }
    ASSERT_TRUE(waiting);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorChecksum);
    ASSERT_EQ(st.link.checksum_errors, 1);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "checksum");
    ASSERT_EQ(gen3_link_queued(&b.club.link), 0);
    ruby_relink(&b); /* the player restarts the GBA: the same club links again */
    ASSERT_TRUE(run_until_room(&b, 400));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(club_status(&b).last_error, Gen3ClubErrorChecksum); /* survives the relink */
}

TEST(test_club_checksum_error_mid_block) {
    /* Plan 1's Review Focus 3: a two-sided glitch while our trainer card streams. G errors and
     * closes; the Disconnected reset flushes the rest of the card, so none of it goes to a gone
     * GBA. */
    Bench b;
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&b.ruby);
    for(int i = 0; i < 400 && b.sent_cccc == 0; i++) step(&b);
    ASSERT_EQ(b.sent_cccc, 1);
    uint32_t chunks_before = b.sent_8888;
    fake_ruby_corrupt_next(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 10));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "checksum");
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorChecksum);
    ASSERT_EQ(st.link.checksum_errors, 1);
    ASSERT_EQ(gen3_link_queued(&b.club.link), 0);
    uint32_t chunks_sent = b.sent_8888 - chunks_before;
    ASSERT_TRUE(chunks_sent < 8); /* the card is 8 chunks: it was cut short */
    run(&b, 30);
    ASSERT_EQ(b.sent_8888 - chunks_before, chunks_sent);
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseWaiting);
}

TEST(test_club_checksum_one_sided) {
    /* A glitch on G's transfer #1 only (§A2: not summed): G is healthy and must not be stranded. */
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    fake_ruby_corrupt_next_checksum(&b.ruby);
    run(&b, 5);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorChecksum);
    ASSERT_EQ(st.link.checksum_errors, 1);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_TRUE(leave_room(&b));
    run(&b, 120);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_EQ(b.sent_5fff, 1);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorChecksum);
    ASSERT_EQ(st.link.checksum_errors, 1);
}

TEST(test_club_checksum_one_sided_other_room) {
    /* In the Error phase a checksum error is counted but keeps the error on screen, and G's 5FFF
     * is still answered. */
    Bench b;
    bench_init(&b, 0x2233);
    fake_ruby_open_link(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 400));
    fake_ruby_corrupt_next_checksum(&b.ruby);
    run(&b, 400);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(b.sent_5fff, 1);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.link.checksum_errors, 1);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorOtherRoom);
}

/* Fresh bench; a one-sided glitch on G's checksum `delay` frames after the club linked. True if
 * both sides still reach the room with no GBA error, the glitch counted once, 2FFE answered. */
static bool linkup_with_one_sided_glitch(uint32_t delay) {
    Bench b;
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&b.ruby);
    if(!run_until_phase(&b, Gen3ClubPhaseLinked, 100)) return false;
    run(&b, delay);
    fake_ruby_corrupt_next_checksum(&b.ruby);
    if(!run_until_room(&b, 400) || fake_ruby_error(&b.ruby) != NULL) return false;
    run(&b, 5);
    Gen3ClubStatus st = club_status(&b);
    return st.phase == Gen3ClubPhaseRoom && st.link.checksum_errors == 1 &&
           st.last_error == Gen3ClubErrorChecksum && b.sent_2ffe == 1 && b.sent_cccc == 1;
}

TEST(test_club_checksum_one_sided_linkup) {
    /* Every burst from the first checked one to the room, the walk-in and G's 2FFE included: a
     * burst whose checksum word reads wrong still carries G's command, which must not be lost. */
    Bench ref;
    bench_init(&ref, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&ref.ruby);
    ASSERT_TRUE(run_until_phase(&ref, Gen3ClubPhaseLinked, 100));
    uint32_t linkup = 0;
    for(; linkup < 400 && fake_ruby_phase(&ref.ruby) != FakeRubyRoom; linkup++) step(&ref);
    ASSERT_TRUE(linkup > 30 && linkup < 400); /* the walk-in alone is 30 frames */
    for(uint32_t delay = 0; delay < linkup; delay++) {
        if(!linkup_with_one_sided_glitch(delay)) {
            printf("    glitch %u frames after linking\n", (unsigned)delay);
            ASSERT_TRUE(false);
        }
    }
}

/* Room reached, the GBA player leaves; a one-sided glitch on G's checksum `delay` frames later.
 * True if both sides still finish cleanly: G closed with no error, the club back to Waiting. */
static bool leave_with_one_sided_glitch(uint32_t delay) {
    Bench b;
    if(!reach_room(&b)) return false;
    fake_ruby_leave_room(&b.ruby);
    run(&b, delay);
    fake_ruby_corrupt_next_checksum(&b.ruby);
    if(!run_until_phase(&b, Gen3ClubPhaseWaiting, 600)) return false;
    run(&b, 60);
    Gen3ClubStatus st = club_status(&b);
    return fake_ruby_phase(&b.ruby) == FakeRubyClosed && fake_ruby_error(&b.ruby) == NULL &&
           st.phase == Gen3ClubPhaseWaiting && st.link.checksum_errors == 1 &&
           b.sent_cafe17 == 1 && b.sent_5fff == 1;
}

TEST(test_club_checksum_one_sided_leaving) {
    /* Every burst of the exit (§B7) while G is still linked, its CAFE 0017 and 5FFF included. */
    Bench ref;
    ASSERT_TRUE(reach_room(&ref));
    fake_ruby_leave_room(&ref.ruby);
    uint32_t leaving = 0;
    for(; leaving < 600 && fake_ruby_phase(&ref.ruby) != FakeRubyClosed; leaving++) step(&ref);
    ASSERT_TRUE(leaving > 120 && leaving < 600); /* the prompt and message alone are 120 */
    for(uint32_t delay = 0; delay < leaving; delay++) {
        if(!leave_with_one_sided_glitch(delay)) {
            printf("    glitch %u frames into the exit\n", (unsigned)delay);
            ASSERT_TRUE(false);
        }
    }
}

TEST(test_club_own_block_after_gba) {
    /* D2: the Flipper's LinkPlayerBlock follows the GBA's, so it can carry the GBA's link type. */
    Bench b;
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&b.ruby);
    for(int i = 0; i < 100 && b.bursts == 0; i++) step(&b);
    ASSERT_EQ(b.bursts, 1);
    /* The first burst after Connected: {2222, 1133, 0...}. Ruby doesn't read word 1 (§A1). */
    ASSERT_EQ(b.last_tx.count, MLINK_BURST_LEN);
    ASSERT_EQ(b.last_tx.word[1], 0x2222);
    ASSERT_EQ(b.last_tx.word[2], GEN3_LINKTYPE_TRADE_SETUP);
    for(int i = 3; i < MLINK_BURST_LEN; i++) ASSERT_EQ(b.last_tx.word[i], 0);

    ASSERT_TRUE(run_until_room(&b, 400));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(b.n_g_blocks, 2); /* LinkPlayerBlock, trainer card */
    ASSERT_EQ(b.n_f_blocks, 2);
    const BenchBlock* g_lp = &b.g_blocks[0];
    const BenchBlock* f_lp = &b.f_blocks[0];
    ASSERT_EQ(g_lp->size, GEN3_LINK_PLAYER_BLOCK_SIZE);
    ASSERT_EQ(f_lp->size, GEN3_LINK_PLAYER_BLOCK_SIZE);
    ASSERT_TRUE(f_lp->first_burst > g_lp->last_burst);
    /* §A6 margin: card_delay idle bursts after the Flipper's own last chunk, then 0xCCCC 2. */
    ASSERT_EQ(b.n_f_cccc, 1);
    ASSERT_EQ(b.f_cccc[0], 2);
    ASSERT_EQ(b.f_cccc_burst[0], f_lp->last_burst + GEN3_CLUB_CARD_DELAY_BURSTS + 1);
    ASSERT_EQ(b.f_blocks[1].size, GEN3_TRAINER_CARD_BLOCK_SIZE);
}

TEST(test_club_trainer_kept) {
    Bench b;
    char name[16];
    ASSERT_TRUE(reach_room(&b));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.trainer_valid);
    ASSERT_EQ(st.trainer.trainer_id & 0xFFFF, 12345);
    ASSERT_EQ(st.trainer.trainer_id >> 16, 54321);
    gen3_text_decode(st.trainer.name, sizeof(st.trainer.name), name, sizeof(name));
    ASSERT_STR_EQ(name, "RUBY");
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_SETUP);

    ASSERT_TRUE(leave_room(&b));
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_TRUE(!st.partner_valid);
    ASSERT_EQ(st.link_type, 0);
    ASSERT_TRUE(st.trainer_valid);
    gen3_text_decode(st.trainer.name, sizeof(st.trainer.name), name, sizeof(name));
    ASSERT_STR_EQ(name, "RUBY");

    /* Another GBA links: its player is the trainer now. */
    b.me.trainer_id = (2u << 16) | 1u;
    gen3_text_encode("SAPH", b.me.name, sizeof(b.me.name), 7);
    ruby_relink(&b);
    ASSERT_TRUE(run_until_room(&b, 400));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    st = club_status(&b);
    ASSERT_TRUE(st.trainer_valid);
    gen3_text_decode(st.trainer.name, sizeof(st.trainer.name), name, sizeof(name));
    ASSERT_STR_EQ(name, "SAPH");
    ASSERT_EQ(st.trainer.trainer_id & 0xFFFF, 1);
    ASSERT_EQ(st.trainer.trainer_id >> 16, 2);
}

TEST(test_club_claim_game) {
    static const Gen3ClubConfig cfg = {
        .name = "FLIPPER", .tid = 22222, .sid = 0, .gender = 0, .trading = false};
    static const uint8_t flipper[11] = {
        0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0xFF, 0xFF, 0xFF, 0xFF};
    Gen3LinkPlayer partner; /* a Ruby GBA's: it gets the Ruby claim */
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    uint8_t want_card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    uint8_t setup[GEN3_LINK_PLAYER_BLOCK_SIZE];
    uint8_t menu[GEN3_LINK_PLAYER_BLOCK_SIZE];

    memset(&partner, 0, sizeof(partner));
    partner.version = GEN3_VERSION_RUBY;
    partner.trainer_id = (54321u << 16) | 12345u;
    gen3_text_encode("RUBY", partner.name, sizeof(partner.name), 7);
    partner.link_type = GEN3_LINKTYPE_TRADE_DISCONNECTED;
    partner.language = GEN3_LANGUAGE_ENGLISH;

    gen3_club_claim_game(&cfg, &partner, GEN3_LINKTYPE_TRADE_DISCONNECTED, true, &lp, card);
    ASSERT_EQ(lp.version, 0x4002);
    ASSERT_EQ(lp.lp_field_2, 0x8000);
    ASSERT_EQ(lp.trainer_id, 22222);
    ASSERT_EQ(lp.link_type, 0x1144);
    ASSERT_EQ(lp.id, 0);
    ASSERT_EQ(lp.language, 2);
    ASSERT_EQ(lp.gender, 0);
    ASSERT_MEM_EQ(lp.name, flipper, sizeof(flipper));
    gen3_trainer_card_build(&lp, want_card);
    ASSERT_MEM_EQ(card, want_card, sizeof(card));

    gen3_club_claim_game(&cfg, &partner, GEN3_LINKTYPE_TRADE_DISCONNECTED, false, &lp, card);
    ASSERT_EQ(lp.lp_field_2, 0);
    ASSERT_EQ(lp.link_type, 0x1144);
    gen3_club_claim_game(&cfg, &partner, GEN3_LINKTYPE_TRADE_CONNECTING, true, &lp, card);
    ASSERT_EQ(lp.lp_field_2, 0); /* only the 0x1144 block carries the National Dex (§B4.5) */
    ASSERT_EQ(lp.link_type, 0x1122);
    gen3_trainer_card_build(&lp, want_card);
    ASSERT_MEM_EQ(card, want_card, sizeof(card));
    gen3_link_player_block_build(&lp, menu);

    gen3_club_claim_game(&cfg, &partner, GEN3_LINKTYPE_TRADE_SETUP, false, &lp, card);
    gen3_trainer_card_build(&lp, want_card);
    ASSERT_MEM_EQ(card, want_card, sizeof(card));
    gen3_link_player_block_build(&lp, setup);
    /* §B1.6: every relink's block is byte-identical but for linkType (block bytes 0x24..0x27) */
    for(size_t i = 0; i < sizeof(setup); i++) {
        if(i >= 0x24 && i <= 0x27) continue;
        ASSERT_EQ(setup[i], menu[i]);
    }
    ASSERT_EQ(setup[0x24], 0x33);
    ASSERT_EQ(menu[0x24], 0x22);

    /* trainer_id is sid << 16 | tid; any non-zero gender is sent as 1 (§A5) */
    static const Gen3ClubConfig other = {
        .name = "AB", .tid = 1, .sid = 2, .gender = 7, .trading = true};
    static const uint8_t ab[11] = {
        0xBB, 0xBC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    gen3_club_claim_game(&other, &partner, GEN3_LINKTYPE_TRADE_SETUP, false, &lp, card);
    ASSERT_EQ(lp.trainer_id, 0x00020001);
    ASSERT_EQ(lp.gender, 1);
    ASSERT_MEM_EQ(lp.name, ab, sizeof(ab));
    ASSERT_EQ(card[0], 1); /* the card's gender */
}

int main(void) {
    if(!bulk_file_open()) return 1; /* bench_init makes the GBA's party with the editor */
    RUN(test_club_reaches_cards);
    RUN(test_club_gba_not_ready);
    RUN(test_club_other_room);
    RUN(test_club_link_test_refuses_trade_types);
    RUN(test_club_bad_magic_still_replies);
    RUN(test_club_card_delay_needed);
    RUN(test_club_room_idle);
    RUN(test_club_answers_2ffe_once);
    RUN(test_club_leave_room);
    RUN(test_club_second_linkup);
    RUN(test_club_gba_reset);
    RUN(test_club_checksum_error);
    RUN(test_club_checksum_error_mid_block);
    RUN(test_club_checksum_one_sided);
    RUN(test_club_checksum_one_sided_other_room);
    RUN(test_club_checksum_one_sided_linkup);
    RUN(test_club_checksum_one_sided_leaving);
    RUN(test_club_own_block_after_gba);
    RUN(test_club_trainer_kept);
    RUN(test_club_claim_game);
    TEST_EXIT();
}
