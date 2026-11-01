#include "test.h"
#include "bulk_file.h"
#include "club_bench.h"
#include "gen3_golden.h"
#include "trade_mons.h"
#include <stdbool.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_text.h>
#include <src/gen3/gen3_trade_sync.h>

/* The Flipper (club) and fake Ruby through a Cable Club trade (docs/gen3/link-protocol-trade.md,
 * Part B). Unless a test says otherwise it starts with bench_init_trade: the fake is RUBY
 * 12345/54321 holding bench_init's three Pokemon, the Flipper FLIPPER 22222 offering
 * tm_make(252, 5, 21). */

static const uint8_t zero_mon[GEN3_PARTY_SIZE];

static uint32_t later_burst(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

/* Until the GBA's trade menu has both LinkPlayers of its 0x1122 relink. */
static bool run_until_relinked(Bench* b, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_phase(&b->ruby) == FakeRubyMenu &&
           fake_ruby_relink_frames(&b->ruby) != UINT32_MAX) {
            return true;
        }
    }
    return false;
}

TEST(test_trade_sit_relinks_menu) {
    Bench b;
    char name[16];
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 200 && b.sent_cafe16 == 0; i++) step(&b);
    ASSERT_EQ(b.sent_cafe16, 1);
    /* §B1.4: the GBA sits first; the Flipper answers in the very next burst */
    ASSERT_EQ(b.f_cafe16_burst, b.g_cafe16_burst + 1);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(st.flipper_seated);

    /* §B1.5: zero bursts from the Flipper's CAFE 0016 until it answers the GBA's 5FFF */
    for(int i = 0; i < 300 && b.sent_5fff == 0; i++) {
        step(&b);
        if(b.sent_5fff != 0 || b.last_tx.count != MLINK_BURST_LEN) continue;
        for(int w = 1; w < MLINK_BURST_LEN; w++) ASSERT_EQ(b.last_tx.word[w], 0);
    }
    ASSERT_EQ(b.sent_5fff, 1);

    ASSERT_TRUE(run_until_relinked(&b, 300));
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyMenu);
    ASSERT_TRUE(fake_ruby_relink_frames(&b.ruby) < 100);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseExchange);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_CONNECTING);
    ASSERT_TRUE(st.partner_valid);
    ASSERT_EQ(st.partner.id, 1); /* sub_8007270: the GBA's seat */
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_EQ(b.sent_cafe16, 1);
    ASSERT_TRUE(!st.gba_seated); /* a new room session's seats */
    ASSERT_TRUE(!st.flipper_seated);
    /* D2: the Flipper's 0x1122 LinkPlayerBlock starts after the GBA's is complete */
    int fk = latest_block(b.f_blocks, b.n_f_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    int gk = latest_block(b.g_blocks, b.n_g_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    ASSERT_TRUE(fk >= 0 && gk >= 0);
    ASSERT_TRUE(b.f_blocks[fk].first_burst > b.g_blocks[gk].last_burst);

    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->link_type, GEN3_LINKTYPE_TRADE_CONNECTING);
    ASSERT_EQ(p->trainer_id, 22222);
    gen3_text_decode(p->name, sizeof(p->name), name, sizeof(name));
    ASSERT_STR_EQ(name, "FLIPPER");
    ASSERT_EQ(p->lp_field_2, 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_waits_for_gba) {
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    run(&b, 1200);
    ASSERT_EQ(b.sent_cafe16, 0); /* the Flipper never sits first */
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_link_test_never_sits) {
    Bench b;
    uint8_t mon[GEN3_PARTY_SIZE];
    bench_init(&b, GEN3_LINKTYPE_TRADE_SETUP);
    tm_make(252, 5, 21, mon);
    ASSERT_TRUE(bench_offer(&b, mon));
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 600);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(!st.flipper_seated);
}

TEST(test_trade_no_offer_never_sits) {
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(!club_status(&b).has_offer);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 600);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(!st.flipper_seated);
}

TEST(test_trade_stand_up) {
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 60);
    ASSERT_TRUE(club_status(&b).gba_seated);
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 1), 0x82); /* READY */
    const uint32_t seen = b.g_cafe_seen; /* so the bits below come from the stand-up */
    ASSERT_EQ(seen & ((1u << 0x1D) | (1u << 0x1A)), 0);
    fake_ruby_stand_up(&b.ruby);
    run(&b, 60);
    ASSERT_TRUE((b.g_cafe_seen & (1u << 0x1D)) != 0);
    ASSERT_TRUE((b.g_cafe_seen & (1u << 0x1A)) != 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 1), 0x80); /* IDLE */
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 0), 0x80);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(!st.gba_seated);
    ASSERT_TRUE(!st.flipper_seated);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_late_sit) {
    /* R5: the GBA sits while the Flipper has no offer; the Flipper sits once it has one. */
    Bench b;
    uint8_t mon[GEN3_PARTY_SIZE];
    bench_init_trade(&b);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    run(&b, 300);
    ASSERT_EQ(b.sent_cafe16, 0);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(!st.flipper_seated);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    tm_make(252, 5, 21, mon);
    ASSERT_TRUE(bench_offer(&b, mon));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 200));
    ASSERT_TRUE(run_until_relinked(&b, 200));
    ASSERT_EQ(b.sent_cafe16, 1);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseExchange);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_CONNECTING);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_relink_timeout_trips_fake) {
    /* §B8: the Flipper never relinks; the GBA's 300-frame task (sub_8083C50) gives up. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 400)); /* this step's OpenLink */
    bench_mute(&b, true);
    uint32_t frames = 0;
    while(fake_ruby_error(&b.ruby) == NULL && frames < 400) {
        step(&b);
        frames++;
    }
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "link timeout");
    ASSERT_EQ(frames, 300);
}

TEST(test_trade_link_type_mismatch_trips_fake) {
    /* §B1.6: a Flipper block whose type isn't the GBA's 0x1122 (Ruby hangs silently). */
    Bench b;
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 400));
    gen3_club_claim_game(&club_cfg_trade, &b.me, GEN3_LINKTYPE_TRADE_SETUP, false, &lp, card);
    bench_raw_relink(&b, &lp, 200);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "link type");
}

TEST(test_trade_exchange_parties) {
    /* §B2.1: five rounds, then the GBA builds its trade menu with the Flipper's one Pokemon. */
    static const uint16_t types[GEN3_TRADE_ROUNDS] = {1, 1, 1, 3, 4};
    static const uint16_t sizes[GEN3_TRADE_ROUNDS] = {200, 200, 200, 220, 40};
    Bench b;
    uint8_t offer[GEN3_PARTY_SIZE];
    uint8_t mon[GEN3_PARTY_SIZE];
    bench_init_trade(&b);
    tm_make(252, 5, 21, offer); /* bench_init_trade's offer */
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 400)); /* this step's 0x1122 OpenLink */
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuLoading);
    ASSERT_TRUE(run_until_browsing(&b, 300));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    /* The GBA's side: CalculateEnemyPartyCount */
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 1);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), offer, GEN3_PARTY_SIZE);
    for(uint8_t i = 1; i < 6; i++) {
        ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, i), zero_mon, GEN3_PARTY_SIZE);
    }

    /* The Flipper's side: the GBA's party */
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseMenu);
    ASSERT_EQ(st.round, 5);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    ASSERT_EQ(b.party_count, 3);
    for(uint8_t i = 0; i < 6; i++) {
        ASSERT_MEM_EQ(fake_ruby_party(&b.ruby, i, false), b.party[i], GEN3_PARTY_SIZE);
        ASSERT_MEM_EQ(fake_ruby_party(&b.ruby, i, true), b.party[i], GEN3_PARTY_SIZE);
        gen3_club_gba_mon(&b.club, i, mon);
        ASSERT_MEM_EQ(mon, i < 3 ? b.party[i] : zero_mon, GEN3_PARTY_SIZE);
    }

    /* On the wire: one CCCC per round, and each side's round blocks after its 0x1122
     * LinkPlayerBlock */
    ASSERT_EQ(b.n_f_cccc, GEN3_TRADE_ROUNDS);
    for(int r = 0; r < GEN3_TRADE_ROUNDS; r++) ASSERT_EQ(b.f_cccc[r], types[r]);
    int fk = latest_block(b.f_blocks, b.n_f_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    int gk = latest_block(b.g_blocks, b.n_g_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    ASSERT_TRUE(fk >= 0 && gk >= 0);
    ASSERT_EQ(b.n_f_blocks, fk + 1 + GEN3_TRADE_ROUNDS);
    ASSERT_EQ(b.n_g_blocks, gk + 1 + GEN3_TRADE_ROUNDS);
    for(int r = 0; r < GEN3_TRADE_ROUNDS; r++) {
        ASSERT_EQ(b.f_blocks[fk + 1 + r].size, sizes[r]); /* {BBBB size 0080} */
        ASSERT_EQ(b.f_blocks[fk + 1 + r].id, 0x80);
        ASSERT_EQ(b.g_blocks[gk + 1 + r].size, sizes[r]);
        ASSERT_EQ(b.g_blocks[gk + 1 + r].id, 0x81);
    }
}

TEST(test_trade_exchange_margins) {
    /* §B2.2: >= 8 idle bursts before round 1's CCCC, >= 5 before each later one (both blocks
     * of the previous step complete); the club leaves 10 and 6. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    int fk = latest_block(b.f_blocks, b.n_f_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    int gk = latest_block(b.g_blocks, b.n_g_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    ASSERT_TRUE(fk >= 0 && gk >= 0);
    ASSERT_EQ(b.n_f_blocks, fk + 1 + GEN3_TRADE_ROUNDS);
    ASSERT_EQ(b.n_g_blocks, gk + 1 + GEN3_TRADE_ROUNDS);
    ASSERT_EQ(b.n_f_cccc, GEN3_TRADE_ROUNDS);
    /* f_cccc_burst[r] follows the LinkPlayerBlocks (r = 0) or round r's blocks */
    for(int r = 0; r < GEN3_TRADE_ROUNDS; r++) {
        uint32_t done = later_burst(b.f_blocks[fk + r].last_burst, b.g_blocks[gk + r].last_burst);
        int64_t gap = (int64_t)b.f_cccc_burst[r] - (int64_t)done;
        ASSERT_TRUE(gap >= (r == 0 ? 11 : 7));
    }
}

TEST(test_trade_round1_delay_needed) {
    /* §B2.2: round 1's CCCC right after the LinkPlayerBlocks reaches the GBA before its case 1,
     * which then waits for its own block and clears the Flipper's round-1 flag. Its case 3 is
     * left one block behind: a Ruby master would stop there (the black screen), but the club
     * goes on to round 2, which the GBA answers with its round-1 copy. The fake stops at that
     * first wrong block, before its 600-frame stall check. */
    Bench b;
    bench_init_trade(&b);
    gen3_club_set_round_delays(&b.club, 0, GEN3_CLUB_ROUND_DELAY_BURSTS);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 900 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stale block");
    ASSERT_EQ(club_status(&b).round, 2);
}

/* bench_init_trade with set_round_delays(first, later); to the room and the GBA sits. When the
 * Flipper's 0x1122 LinkPlayerBlock completes (its first block of size 60 in the GBA's trade
 * menu), the GBA's main loop stalls `stall` frames: round 1's margin is all that absorbs it.
 * Then until Browsing (true) or a fake error (false). */
static bool exchange_with_round1_stall(Bench* b, uint8_t first, uint8_t later, uint8_t stall) {
    bench_init_trade(b);
    gen3_club_set_round_delays(&b->club, first, later);
    if(!bench_to_room(b)) return false;
    fake_ruby_sit(&b->ruby, 1);
    bool stalled = false;
    for(int i = 0; i < 900; i++) {
        const uint8_t n = b->n_f_blocks;
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_menu_state(&b->ruby) == FakeRubyMenuBrowsing) return true;
        if(!stalled && b->n_f_blocks > n && fake_ruby_phase(&b->ruby) == FakeRubyMenu &&
           b->f_blocks[b->n_f_blocks - 1].size == GEN3_LINK_PLAYER_BLOCK_SIZE) {
            fake_ruby_stall(&b->ruby, stall);
            stalled = true;
        }
    }
    return false;
}

TEST(test_trade_round1_margin) {
    /* §B2.2: the GBA must take round 1's CCCC at least 3 frames after both LinkPlayerBlocks;
     * the club's 10 idle bursts leave room for an 8-frame stall at the worst moment. */
    Bench b;
    ASSERT_TRUE(exchange_with_round1_stall(&b, GEN3_CLUB_ROUND1_DELAY_BURSTS, 6, 8));
    ASSERT_TRUE(!exchange_with_round1_stall(&b, GEN3_CLUB_ROUND1_DELAY_BURSTS, 6, 9));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stale block");
}

TEST(test_trade_later_rounds_need_no_delay) {
    /* The rounds 2-5 delay is padding (§B2.2, inferred): each CCCC queues on the GBA behind both
     * blocks of the previous round, so even no idle burst works, and the fake invents no
     * hazard. */
    Bench b;
    ASSERT_TRUE(exchange_with_round1_stall(&b, GEN3_CLUB_ROUND1_DELAY_BURSTS, 0, 0));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseMenu);
}

TEST(test_trade_missing_block_stalls_fake) {
    /* §B2.1: no timeout in any case. The Flipper's round-1 block never comes (its queue is
     * dropped and only its link layer runs on, sending idle bursts): case 3 waits forever. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 400 && club_status(&b).round == 0; i++) step(&b);
    ASSERT_EQ(club_status(&b).round, 1); /* round 1's CCCC is on the wire, its block queued */
    gen3_link_flush(&b.club.link);
    uint32_t frames = 0;
    while(fake_ruby_error(&b.ruby) == NULL && frames < 700) {
        MlinkTx tx;
        uint16_t cmd[GEN3_CMD_LEN];
        gen3_link_rx(&b.club.link, &b.rx, cmd);
        gen3_link_tx(&b.club.link, &tx);
        fake_ruby_frame(&b.ruby, &tx, &b.rx);
        frames++;
    }
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "exchange stalled");
    /* 600 frames into case 3, which began 8 frames before the GBA got round 1's CCCC (G0 + 3
     * against G0 + 11) */
    ASSERT_EQ(frames, 592);
}

TEST(test_trade_exchange_with_backlog) {
    /* Review Focus 4: the GBA's main loop stalls 4 frames (a map load, a flash write) as each
     * round starts, so its receive queue backs up; the margins absorb it. */
    Bench b;
    uint8_t offer[GEN3_PARTY_SIZE];
    uint8_t round = 0;
    int stalls = 0;
    bench_init_trade(&b);
    tm_make(252, 5, 21, offer);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 900; i++) {
        step(&b);
        if(fake_ruby_error(&b.ruby) != NULL) break;
        if(fake_ruby_menu_state(&b.ruby) == FakeRubyMenuBrowsing) break;
        const uint8_t now = club_status(&b).round;
        if(now != round) {
            fake_ruby_stall(&b.ruby, 4);
            round = now;
            stalls++;
        }
    }
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    ASSERT_EQ(stalls, GEN3_TRADE_ROUNDS);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), offer, GEN3_PARTY_SIZE);
}

TEST(test_trade_exchange_empty_offer) {
    /* R8: with no offer the Flipper sends an empty party; Ruby opens its menu with an empty
     * partner column. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 400));
    ASSERT_EQ(club_status(&b).round, 0); /* before round 1 */
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(run_until_browsing(&b, 600));
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 0);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), zero_mon, GEN3_PARTY_SIZE);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseMenu);
    ASSERT_EQ(st.round, 5);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_bad_offer_trips_fake) {
    /* The club trusts its offer (the app checks it); a bad checksum reaches Ruby as a Bad Egg. */
    Bench b;
    uint8_t bad[GEN3_PARTY_SIZE];
    golden_game(bad);
    bad[0x30] ^= 1;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_offer(&b, bad));
    ASSERT_TRUE(!bench_reach_menu(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad egg");
}

TEST(test_trade_bad_item_trips_fake) {
    /* §B2.5 rule 3: the ENIGMA BERRY, poked raw past every check. */
    Bench b;
    Gen3Pkm p;
    uint8_t mon[GEN3_PARTY_SIZE];
    tm_make_pkm(252, 5, 21, &p);
    gen3_pkm_set(&p, Gen3FieldHeldItem, GEN3_ITEM_ENIGMA_BERRY);
    gen3_pkm_encrypt(&p, mon);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_offer(&b, mon));
    ASSERT_TRUE(!bench_reach_menu(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad offer");
}

/* bench_init_trade offering tm_make_pkm(252, 5, 21) with `field` poked raw to `value` (checksum
 * fixed, past every check), then bench_reach_menu: the fake's error, or NULL. */
static const char* offer_poked(Bench* b, Gen3Field field, uint32_t value) {
    Gen3Pkm p;
    uint8_t mon[GEN3_PARTY_SIZE];
    tm_make_pkm(252, 5, 21, &p);
    gen3_pkm_set(&p, field, value);
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, mon);
    bench_init_trade(b);
    if(!bench_offer(b, mon)) return "offer refused";
    bench_reach_menu(b);
    return fake_ruby_error(&b->ruby);
}

TEST(test_trade_bad_move_trips_fake) {
    /* §B2.5 rule 2: moves 0..354 (NUM_MOVES = 355) */
    Bench b;
    const char* error = offer_poked(&b, Gen3FieldMove2, 355);
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "bad offer");
}

TEST(test_trade_mail_byte_trips_fake) {
    /* §B2.5 rule 4: no mail data goes with the Flipper's Pokemon, so its mail byte stays 0xFF */
    Bench b;
    const char* error = offer_poked(&b, Gen3FieldMail, 0);
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "bad offer");
}

TEST(test_trade_mail_item_trips_fake) {
    /* §B2.5 rule 4: a mail item (121..132) without mail data */
    Bench b;
    const char* error = offer_poked(&b, Gen3FieldHeldItem, 132);
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "bad offer");
}

TEST(test_trade_bad_request_trips_fake) {
    /* sBlockRequestLookupTable has 5 entries; Ruby would read past it for type 5. */
    static const uint16_t request[GEN3_CMD_LEN] = {0xCCCC, 5};
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(gen3_link_send(&b.club.link, request)); /* raw, past the club */
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad request");
}

TEST(test_trade_seat_step_while_busy) {
    /* The fake's avatar: a step onto a seat while this GBA's player isn't IDLE (here BUSY in the
     * exit prompt) runs no seat script, but the avatar still ends on the seat tile. */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_TRUE(!fake_ruby_on_seat(&b.ruby));
    fake_ruby_leave_room(&b.ruby);
    for(int i = 0; i < 60 && fake_ruby_link_state(&b.ruby, 1) != 0x81; i++) step(&b);
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 1), 0x81); /* BUSY */
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 10);
    ASSERT_TRUE(fake_ruby_on_seat(&b.ruby));
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyLeaving); /* no seat script */
    ASSERT_EQ(fake_ruby_link_state(&b.ruby, 1), 0x81);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_extra_request_trips_fake) {
    /* U3: a sixth CCCC after the exchange would get gBlockSendBuffer's round-5 bytes. */
    static const uint16_t request[GEN3_CMD_LEN] = {0xCCCC, 1};
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(gen3_link_send(&b.club.link, request)); /* raw, past the club */
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stale block");
}

/* ---- Trade menu (§B3) ---- */

static size_t n_trade_blocks(const BenchBlock* blocks, uint8_t n) {
    uint16_t codes[BENCH_BLOCKS];
    return bench_codes(blocks, n, codes, BENCH_BLOCKS);
}

/* blk is a 20-byte block {code, arg}; else prints what it is. */
static bool trade_block_is(const BenchBlock* blk, uint16_t code, uint16_t arg) {
    if(!blk) {
        printf("  no such block\n");
        return false;
    }
    if(blk->code != code || blk->arg != arg) {
        printf("  block {%04X %u}, want {%04X %u}\n", blk->code, blk->arg, code, arg);
        return false;
    }
    return true;
}

/* The Flipper's reply went out after 6 idle bursts or more, counted from the burst of the last
 * chunk of the GBA's block (D4; §B3.4 asks >= 5). A literal, not the club's constant, so a change
 * there shows here. */
static bool replied_after(const BenchBlock* gba, const BenchBlock* flipper) {
    return gba && flipper && flipper->first_burst >= gba->last_burst + 7;
}

TEST(test_trade_offer_confirm_ccdd) {
    /* §B3.5: AABB 2 -> DDDD 0 -> BBBB -> CCDD, each reply 6 idle bursts after the GBA's block */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuConfirming, 60));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
    const BenchBlock* aabb = trade_block(b.g_blocks, b.n_g_blocks, 0);
    ASSERT_TRUE(trade_block_is(aabb, GEN3_TRADE_READY, 2));
    ASSERT_EQ(aabb->id, 0x81);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* dddd = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(dddd, GEN3_TRADE_SET_MONS, 0));
    ASSERT_EQ(dddd->id, 0x80);
    ASSERT_TRUE(replied_after(aabb, dddd));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.menu, Gen3ClubMenuOffered);
    ASSERT_EQ(st.gba_slot, 2);
    ASSERT_EQ(st.refused, Gen3RefuseNone);
    ASSERT_TRUE(!st.trade_pending);

    /* 20 frames of slide-in, then "Is this trade okay?" after 120 more */
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 200));
    fake_ruby_answer(&b.ruby, true);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuClosing, 60));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 2);
    const BenchBlock* bbbb = trade_block(b.g_blocks, b.n_g_blocks, 1);
    ASSERT_TRUE(trade_block_is(bbbb, GEN3_TRADE_CONFIRM, 2)); /* §B3.1: arg is stale (AABB's) */
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    const BenchBlock* ccdd = trade_block(b.f_blocks, b.n_f_blocks, 1);
    ASSERT_TRUE(trade_block_is(ccdd, GEN3_TRADE_START, 0));
    ASSERT_TRUE(replied_after(bbbb, ccdd));
    st = club_status(&b);
    ASSERT_EQ(st.menu, Gen3ClubMenuStarting);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_EQ(st.gba_slot, 2);

    /* The GBA fades out and closes the link for the trade (§B4.1; test_trade_round_trip goes on) */
    const uint32_t answers = b.sent_5fff;
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyTrade, 120));
    ASSERT_EQ(b.sent_5fff, answers + 1);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 60));
    ASSERT_TRUE(club_status(&b).trade_pending); /* trade context (D3) */
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_gba_says_no) {
    /* §B3.4: BBCC gets DDEE; the GBA shows "The trade has been canceled." until A */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, false);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 60));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 2);
    const BenchBlock* bbcc = trade_block(b.g_blocks, b.n_g_blocks, 1);
    ASSERT_TRUE(trade_block_is(bbcc, GEN3_TRADE_CONFIRM_NO, 2));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    ASSERT_TRUE(trade_block_is(trade_block(b.f_blocks, b.n_f_blocks, 0), GEN3_TRADE_SET_MONS, 0));
    const BenchBlock* ddee = trade_block(b.f_blocks, b.n_f_blocks, 1);
    ASSERT_TRUE(trade_block_is(ddee, GEN3_TRADE_PLAYER_CANCEL, 0));
    ASSERT_TRUE(replied_after(bbcc, ddee));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.menu, Gen3ClubMenuIdle);
    ASSERT_TRUE(!st.trade_pending);

    fake_ruby_press_a(&b.ruby);
    ASSERT_TRUE(run_until_browsing(&b, 5));
    run(&b, 300);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_gba_cancels) {
    /* §B3.4, §B6.3: EEAA gets EEBB; the GBA fades out, closes the link and heads for the room */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_cancel_trade(&b.ruby);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuClosing, 60));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
    const BenchBlock* eeaa = trade_block(b.g_blocks, b.n_g_blocks, 0);
    ASSERT_TRUE(trade_block_is(eeaa, GEN3_TRADE_CANCEL, 0));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* eebb = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(eebb, GEN3_TRADE_BOTH_CANCEL, 0));
    ASSERT_TRUE(replied_after(eeaa, eebb));
    ASSERT_EQ(club_status(&b).menu, Gen3ClubMenuLeaving);

    const uint32_t answers = b.sent_5fff;
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
    ASSERT_EQ(b.sent_5fff, answers + 1); /* the Flipper answered the GBA's 5FFF */
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseClosing);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 60));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1); /* no CCDD */
    ASSERT_TRUE(!club_status(&b).trade_pending);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    /* §B6.3: the 0x1111 relink back into the room, the avatar on its seat */
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE);
    ASSERT_TRUE(st.resit_needed);
    ASSERT_TRUE(fake_ruby_on_seat(&b.ruby));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    /* A restart and a 0x1133 linkup walk the GBA in from the counter: no re-sit needed */
    fake_ruby_power_off(&b.ruby);
    run(&b, 30);
    ruby_relink(&b);
    ASSERT_TRUE(run_until_room(&b, 420));
    st = club_status(&b);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_SETUP);
    ASSERT_TRUE(!st.resit_needed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

/* bench_init_trade with the GBA party {tm_make(1, 5, 11), slot1, tm_make(7, 10, 13)}, then
 * bench_reach_menu. */
static bool reach_menu_with_slot1(Bench* b, const uint8_t slot1[GEN3_PARTY_SIZE]) {
    uint8_t party[3][GEN3_PARTY_SIZE];
    tm_make(1, 5, 11, party[0]);
    memcpy(party[1], slot1, GEN3_PARTY_SIZE);
    tm_make(7, 10, 13, party[2]);
    bench_init_trade(b);
    bench_set_party(b, party, 3);
    return bench_reach_menu(b);
}

/* The GBA offers its slot 1, which the Flipper refuses: DDEE, never DDDD; the GBA shows the
 * message. The reason, or -1 on another outcome. */
static int refused_slot1(Bench* b, const uint8_t slot1[GEN3_PARTY_SIZE]) {
    if(!reach_menu_with_slot1(b, slot1)) return -1;
    fake_ruby_offer(&b->ruby, 1);
    if(!run_until_menu_state(b, FakeRubyMenuMessage, 60)) return -1;
    const BenchBlock* aabb = trade_block(b->g_blocks, b->n_g_blocks, 0);
    const BenchBlock* ddee = trade_block(b->f_blocks, b->n_f_blocks, 0);
    if(n_trade_blocks(b->g_blocks, b->n_g_blocks) != 1 ||
       n_trade_blocks(b->f_blocks, b->n_f_blocks) != 1 ||
       !trade_block_is(aabb, GEN3_TRADE_READY, 1) ||
       !trade_block_is(ddee, GEN3_TRADE_PLAYER_CANCEL, 0) || !replied_after(aabb, ddee)) {
        return -1;
    }
    Gen3ClubStatus st = club_status(b);
    if(st.menu != Gen3ClubMenuIdle || st.trade_pending) return -1;
    return (int)st.refused;
}

TEST(test_trade_refuses_egg) {
    /* D8: the Flipper takes no Egg; the GBA may then pick another Pokemon */
    Bench b;
    uint8_t egg[GEN3_PARTY_SIZE];
    tm_make_egg(egg);
    ASSERT_EQ(refused_slot1(&b, egg), Gen3RefuseEgg);
    fake_ruby_press_a(&b.ruby);
    ASSERT_TRUE(run_until_browsing(&b, 5));
    fake_ruby_offer(&b.ruby, 0);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuConfirming, 60));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    ASSERT_TRUE(trade_block_is(trade_block(b.f_blocks, b.n_f_blocks, 1), GEN3_TRADE_SET_MONS, 0));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.refused, Gen3RefuseNone);
    ASSERT_EQ(st.gba_slot, 0);
    ASSERT_EQ(st.menu, Gen3ClubMenuOffered);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_refuses_mail) {
    /* D8: nor a Mail holder (the Flipper would have to keep its mail) */
    Bench b;
    uint8_t mail[GEN3_PARTY_SIZE];
    tm_make_mail(mail);
    ASSERT_EQ(refused_slot1(&b, mail), Gen3RefuseMail);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_refuses_bad_egg) {
    /* A checksum mismatch: Ruby shows it as a Bad Egg */
    Bench b;
    uint8_t bad[GEN3_PARTY_SIZE];
    tm_make_bad_egg(bad);
    ASSERT_EQ(refused_slot1(&b, bad), Gen3RefuseBadEgg);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_only_pokemon_stays) {
    /* §B3.2: TRADE on the GBA's last battle-able Pokemon sends nothing ("That's your only
     * POKéMON for battle."); its party here is that one Pokemon */
    Bench b;
    uint8_t party[1][GEN3_PARTY_SIZE];
    tm_make(1, 5, 11, party[0]);
    bench_init_trade(&b);
    bench_set_party(&b, party, 1);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 0);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 5));
    run(&b, 60);
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 0);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

/* bench_init_trade with the offer replaced by 100 zero bytes before round 1 (R8): the GBA's menu
 * shows no Flipper Pokemon. A test-only offer: the club holds one, unchanged since round 1, so
 * it sends no EEBB (test_trade_empty_offer_eebb has no offer at all), and round 1 shows nothing
 * (no hasSpecies bit). Until Browsing. */
static bool reach_menu_empty_offer(Bench* b) {
    bench_init_trade(b);
    if(!bench_to_room(b)) return false;
    fake_ruby_sit(&b->ruby, 1);
    if(!run_until_fake_phase(b, FakeRubyMenu, 400) || club_status(b).round != 0) return false;
    if(!gen3_club_set_offer(&b->club, zero_mon, 0)) return false;
    return run_until_browsing(b, 600) && fake_ruby_enemy_count(&b->ruby) == 0;
}

TEST(test_trade_no_dddd_to_empty_party) {
    /* R8: the GBA shows no Flipper Pokemon, so a DDDD would point at a partner icon Ruby never
     * made. Its AABB gets DDEE (the R8 gate: round 1 showed nothing). */
    Bench b;
    ASSERT_TRUE(reach_menu_empty_offer(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 60));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* ddee = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(ddee, GEN3_TRADE_PLAYER_CANCEL, 0));
    ASSERT_TRUE(replied_after(trade_block(b.g_blocks, b.n_g_blocks, 0), ddee));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.menu, Gen3ClubMenuIdle);
    ASSERT_EQ(st.refused, Gen3RefuseNone); /* the GBA's Pokemon is fine; the Flipper has none */
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_dddd_to_empty_party_trips_fake) {
    /* R8: a DDDD to an empty partner column, as soon as the GBA's AABB is complete */
    Bench b;
    ASSERT_TRUE(reach_menu_empty_offer(&b));
    fake_ruby_offer(&b.ruby, 2);
    for(int i = 0; i < 60 && n_trade_blocks(b.g_blocks, b.n_g_blocks) == 0; i++) step(&b);
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
    ASSERT_TRUE(bench_inject_code(&b, GEN3_TRADE_SET_MONS, 0));
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "empty party");
}

TEST(test_trade_ccdd_rule_trips_fake) {
    /* §B3.4: Ruby starts the trade on any CCDD, with whatever its cursor holds */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_inject_code(&b, GEN3_TRADE_START, 0));
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad ccdd");
}

TEST(test_trade_unasked_dddd_trips_fake) {
    /* §B3.4: DDDD only in reply to AABB */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_inject_code(&b, GEN3_TRADE_SET_MONS, 0));
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "unexpected dddd");
}

TEST(test_trade_ccdd_before_bbbb_trips_fake) {
    /* §B3.4: AABB -> DDDD, then a CCDD at "Is this trade okay?" before the GBA's BBBB */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    ASSERT_TRUE(bench_inject_code(&b, GEN3_TRADE_START, 0));
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad ccdd");
}

TEST(test_trade_ccdd_after_ddee_trips_fake) {
    /* §B3.4: BBCC -> DDEE, then a CCDD */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, false);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 60));
    ASSERT_TRUE(bench_inject_code(&b, GEN3_TRADE_START, 0));
    for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "bad ccdd");
}

/* From Browsing: the GBA cancels (EEBB's fade) or trades its slot 2 (CCDD's fade); `delay` frames
 * into the fade a stray {2222, 1122} arrives, so the GBA resends its LinkPlayerBlock and
 * gLinkCallback is busy when the fade ends. The fake's error. */
static const char* stray_link_type_in_fade(Bench* b, bool cancel, uint32_t delay) {
    bench_init_trade(b);
    if(!bench_reach_menu(b)) return "no menu";
    if(cancel) {
        fake_ruby_cancel_trade(&b->ruby);
    } else {
        fake_ruby_offer(&b->ruby, 2);
        if(!run_until_menu_state(b, FakeRubyMenuPrompt, 260)) return "no prompt";
        fake_ruby_answer(&b->ruby, true);
    }
    if(!run_until_menu_state(b, FakeRubyMenuClosing, 60)) return "no fade";
    run(b, delay);
    bench_inject_cmd(b, 0x2222, GEN3_LINKTYPE_TRADE_CONNECTING);
    for(int i = 0; i < 40 && fake_ruby_error(&b->ruby) == NULL; i++) step(b);
    return fake_ruby_error(&b->ruby);
}

TEST(test_trade_eebb_hang_trips_fake) {
    /* sub_8049D9C: SetCloseLinkCallback is ignored while a block goes out; state 12 never ends */
    Bench b;
    const char* error = stray_link_type_in_fade(&b, true, 12);
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "eebb hang");
}

TEST(test_trade_ccdd_hang_trips_fake) {
    /* sub_8048A50: the same in state 10; state 13 never ends */
    Bench b;
    const char* error = stray_link_type_in_fade(&b, false, 12);
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "ccdd hang");
}

TEST(test_trade_press_r_gets_no_reply) {
    /* §B3.2, §B3.4: R sends the debug block {0000, 1, 2, ...}; the arbiter ignores it, and the
     * menu goes on */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_press_r(&b.ruby);
    run(&b, 60);
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
    ASSERT_TRUE(trade_block_is(trade_block(b.g_blocks, b.n_g_blocks, 0), 0x0000, 1));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 0);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    ASSERT_EQ(club_status(&b).menu, Gen3ClubMenuIdle);
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuConfirming, 60));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    ASSERT_TRUE(trade_block_is(trade_block(b.f_blocks, b.n_f_blocks, 0), GEN3_TRADE_SET_MONS, 0));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_club_confirm_while_idle) {
    /* §B3.4: a BBBB with no DDDD of the Flipper's before it gets DDEE, never CCDD (Ruby never
     * sends one; a scripted GBA does) */
    Bench b;
    BenchRawGba g;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    raw_gba_init(&g, &b, GEN3_TRADE_CONFIRM, 0);
    raw_gba_run(&g, &b, 30);
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* ddee = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(ddee, GEN3_TRADE_PLAYER_CANCEL, 0));
    ASSERT_TRUE(replied_after(trade_block(b.g_blocks, b.n_g_blocks, 0), ddee));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.menu, Gen3ClubMenuIdle);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
}

TEST(test_club_ready_past_party) {
    /* An AABB for slot 6 or later (past any GBA party, as a scripted GBA sends it): refused as
     * Empty, DDEE */
    Bench b;
    BenchRawGba g;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    raw_gba_init(&g, &b, GEN3_TRADE_READY, 6);
    raw_gba_run(&g, &b, 30);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* ddee = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(ddee, GEN3_TRADE_PLAYER_CANCEL, 0));
    ASSERT_TRUE(replied_after(trade_block(b.g_blocks, b.n_g_blocks, 0), ddee));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.refused, Gen3RefuseEmpty);
    ASSERT_EQ(st.menu, Gen3ClubMenuIdle);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
}

/* ---- The trade and the link save (§B4, §B5, §B6.1) ---- */

/* Y (bench_init_trade's offer), decrypted, as the GBA holds it after TradeMons: Friendship 70,
 * checksum refixed (§B4.4 step 3), every other byte as offered. */
static void traded_offer(Gen3Pkm* out) {
    tm_make_pkm(252, 5, 21, out);
    gen3_pkm_set(out, Gen3FieldFriendship, 70);
    gen3_pkm_fix_checksum(out);
}

TEST(test_trade_round_trip) {
    /* §B4-§B6.1: the GBA trades its slot 2 (X) for the Flipper's Y and saves; the club
     * finalizes at sync 3, and the automatic exchange after the save shows X as the Flipper's */
    Bench b;
    uint8_t x[GEN3_PARTY_SIZE];
    uint8_t got[GEN3_PARTY_SIZE];
    uint16_t codes[8];
    uint32_t done = 0;
    Gen3Pkm want;
    Gen3Pkm saved;
    bench_init_trade(&b);
    memcpy(x, b.party[2], GEN3_PARTY_SIZE);
    ASSERT_TRUE(bench_reach_menu(&b));
    const uint32_t answers = b.sent_2ffe;
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    /* The GBA's save: Y with Friendship 70 in slot 2, the rest as it was */
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_TRUE(!fake_ruby_save_corrupt(&b.ruby));
    ASSERT_TRUE(gen3_pkm_decrypt(fake_ruby_party(&b.ruby, 2, true), GEN3_PARTY_SIZE, &saved));
    traded_offer(&want);
    ASSERT_MEM_EQ(saved.raw, want.raw, GEN3_PARTY_SIZE);
    for(uint8_t i = 0; i < 2; i++) {
        ASSERT_MEM_EQ(fake_ruby_party(&b.ruby, i, true), b.party[i], GEN3_PARTY_SIZE);
    }

    /* The club: done, X received and offered back */
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseMenu);
    ASSERT_EQ(st.trades_done, 1);
    ASSERT_EQ(st.last_result, Gen3TradeResultDone);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_TRUE(!st.save_window);
    ASSERT_EQ(st.save_syncs, 3);
    ASSERT_TRUE(st.has_offer);
    gen3_club_get_received(&b.club, got, &done);
    ASSERT_EQ(done, 1);
    ASSERT_MEM_EQ(got, x, GEN3_PARTY_SIZE);
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 1); /* §B6.1 */
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), x, GEN3_PARTY_SIZE);

    /* On the wire: the Flipper's trade blocks, and one 2FFE per GBA 2FFE, after it (§B5.3) */
    ASSERT_EQ(bench_codes(b.f_blocks, b.n_f_blocks, codes, 8), 3);
    ASSERT_EQ(codes[0], GEN3_TRADE_SET_MONS);
    ASSERT_EQ(codes[1], GEN3_TRADE_START);
    ASSERT_EQ(codes[2], GEN3_TRADE_ANIM_ACK);
    const BenchBlock* dcba = trade_block(b.f_blocks, b.n_f_blocks, 2);
    ASSERT_TRUE(dcba != NULL);
    ASSERT_TRUE(b.g_5fff_burst > dcba->first_burst);
    uint32_t f_syncs[BENCH_SYNCS];
    uint32_t g_syncs[BENCH_SYNCS];
    uint8_t n_f = 0;
    uint8_t n_g = 0;
    for(uint8_t k = 0; k < b.n_f_2ffe; k++) {
        const uint32_t at = b.f_2ffe_burst[k];
        if(at > dcba->first_burst && at < b.g_5fff_burst) f_syncs[n_f++] = at;
    }
    for(uint8_t k = 0; k < b.n_g_2ffe; k++) {
        const uint32_t at = b.g_2ffe_burst[k];
        if(at > dcba->first_burst && at < b.g_5fff_burst) g_syncs[n_g++] = at;
    }
    ASSERT_EQ(n_f, 3);
    ASSERT_EQ(n_g, 3);
    for(uint8_t k = 0; k < 3; k++) ASSERT_TRUE(f_syncs[k] > g_syncs[k]);
    ASSERT_TRUE(f_syncs[0] < g_syncs[1] && f_syncs[1] < g_syncs[2]);
    ASSERT_EQ(b.sent_2ffe, answers + 3);
}

TEST(test_trade_dcba_after_abcd) {
    /* §B4.3: DCBA only after the GBA's ABCD (the club leaves 6 idle bursts) */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 3); /* AABB, BBBB, ABCD */
    const BenchBlock* abcd = trade_block(b.g_blocks, b.n_g_blocks, 2);
    ASSERT_TRUE(abcd != NULL);
    ASSERT_EQ(abcd->code, GEN3_TRADE_ANIM_DONE);
    const BenchBlock* dcba = trade_block(b.f_blocks, b.n_f_blocks, 2);
    ASSERT_TRUE(trade_block_is(dcba, GEN3_TRADE_ANIM_ACK, 0));
    ASSERT_TRUE(replied_after(abcd, dcba));
}

TEST(test_trade_save_window) {
    /* D10: save_window from the club's sync-1 answer to sync 3, and the trade counts at sync 3 */
    Bench b;
    bool answered = false;
    bool saw[GEN3_CLUB_SAVE_SYNCS + 1] = {false};
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    Gen3ClubStatus prev = club_status(&b);
    for(int i = 0; i < 4000; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        if(!answered && fake_ruby_menu_state(&b.ruby) == FakeRubyMenuPrompt) {
            fake_ruby_answer(&b.ruby, true);
            answered = true;
        }
        const Gen3ClubStatus st = club_status(&b);
        ASSERT_TRUE(st.save_syncs <= GEN3_CLUB_SAVE_SYNCS);
        ASSERT_TRUE(st.save_syncs == prev.save_syncs || st.save_syncs == prev.save_syncs + 1);
        ASSERT_EQ(st.save_window, st.save_syncs == 1 || st.save_syncs == 2);
        ASSERT_EQ(st.trades_done, st.save_syncs == 3 ? 1 : 0);
        saw[st.save_syncs] = true;
        prev = st;
        if(st.trades_done == 1 && fake_ruby_menu_state(&b.ruby) == FakeRubyMenuBrowsing) break;
    }
    ASSERT_TRUE(saw[1] && saw[2] && saw[3]);
    ASSERT_EQ(prev.trades_done, 1);
    ASSERT_TRUE(!prev.save_window);
}

/* bench_init_trade with set_national_dex(on), then one trade: lp_field_2 of the Flipper's
 * LinkPlayer as the GBA parsed it in the room (0x1133), the trade menu (0x1122) and the trade
 * (0x1144). Runs on until the GBA's link save is committed. */
static bool lp_field_2_in_trade(Bench* b, bool on, uint16_t out[3]) {
    bench_init_trade(b);
    gen3_club_set_national_dex(&b->club, on);
    if(!bench_to_room(b)) return false;
    const Gen3LinkPlayer* p = fake_ruby_partner(&b->ruby);
    if(p->link_type != GEN3_LINKTYPE_TRADE_SETUP) return false;
    out[0] = p->lp_field_2;
    fake_ruby_sit(&b->ruby, 1);
    if(!run_until_relinked(b, 300) || p->link_type != GEN3_LINKTYPE_TRADE_CONNECTING) return false;
    out[1] = p->lp_field_2;
    if(!run_until_browsing(b, 600) || !run_until_trade_link(b, 600)) return false;
    out[2] = p->lp_field_2;
    for(int i = 0; i < 2000 && fake_ruby_saved_trades(&b->ruby) == 0; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
    }
    return fake_ruby_saved_trades(&b->ruby) == 1;
}

TEST(test_trade_national_dex_off) {
    /* §B4.5: off by default; no block carries 0x8000 */
    Bench b;
    uint16_t lp_field_2[3];
    ASSERT_TRUE(lp_field_2_in_trade(&b, false, lp_field_2));
    ASSERT_EQ(lp_field_2[0], 0);
    ASSERT_EQ(lp_field_2[1], 0);
    ASSERT_EQ(lp_field_2[2], 0);
    ASSERT_TRUE(!fake_ruby_national_dex(&b.ruby));
}

TEST(test_trade_national_dex_on) {
    /* §B4.5: only the 0x1144 block carries 0x8000; the GBA saves its National Dex */
    Bench b;
    uint16_t lp_field_2[3];
    ASSERT_TRUE(lp_field_2_in_trade(&b, true, lp_field_2));
    ASSERT_EQ(lp_field_2[0], 0);
    ASSERT_EQ(lp_field_2[1], 0);
    ASSERT_EQ(lp_field_2[2], 0x8000);
    ASSERT_TRUE(fake_ruby_national_dex(&b.ruby));
}

TEST(test_trade_lost_before_sync1) {
    /* D5: the GBA is switched off in the animation: nothing is written on it; the Flipper keeps
     * its Pokemon (Canceled) */
    Bench b;
    uint8_t offer[GEN3_PARTY_SIZE];
    tm_make(252, 5, 21, offer);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_trade_link(&b, 600));
    run(&b, 60); /* past the pictures, into the animation */
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseTrade);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyTrade);
    fake_ruby_power_off(&b.ruby);
    run(&b, 30);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultCanceled);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_TRUE(!st.save_window);
    ASSERT_EQ(st.trades_done, 0);
    ASSERT_TRUE(st.has_offer);
    ASSERT_MEM_EQ(b.club.offer, offer, GEN3_PARTY_SIZE);
    ASSERT_TRUE(saved_party_untouched(&b));
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
    ASSERT_TRUE(!fake_ruby_save_corrupt(&b.ruby));
}

TEST(test_trade_lost_in_window) {
    /* D5: the Flipper stops bursting after its sync-1 answer; the GBA's save is half-written, so
     * it reports a corrupt save and loads the pre-trade one: the Flipper keeps its Pokemon
     * (RolledBack) */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_save_syncs(&b, 1, 1000));
    ASSERT_TRUE(club_status(&b).save_window);
    run(&b, 4);
    bench_mute(&b, true);
    run(&b, 30);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "lag");
    ASSERT_TRUE(fake_ruby_save_corrupt(&b.ruby));
    bench_mute(&b, false);
    run(&b, 60);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultRolledBack);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_TRUE(!st.save_window);
    ASSERT_EQ(st.trades_done, 0);
    ASSERT_TRUE(saved_party_untouched(&b));
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
}

TEST(test_trade_lost_after_sync2) {
    /* D5: the GBA is switched off after the club's sync-2 answer: it has committed, so the
     * Flipper takes its Pokemon (Assumed) */
    Bench b;
    uint8_t x[GEN3_PARTY_SIZE];
    uint8_t got[GEN3_PARTY_SIZE];
    uint32_t done = 0;
    bench_init_trade(&b);
    memcpy(x, b.party[2], GEN3_PARTY_SIZE);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_save_syncs(&b, 2, 1000));
    run(&b, 4);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_TRUE(club_status(&b).save_window);
    fake_ruby_power_off(&b.ruby);
    run(&b, 30);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultAssumed);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_TRUE(!st.save_window);
    ASSERT_EQ(st.trades_done, 1);
    gen3_club_get_received(&b.club, got, &done);
    ASSERT_EQ(done, 1);
    ASSERT_MEM_EQ(got, x, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!fake_ruby_save_corrupt(&b.ruby));
}

TEST(test_trade_save_long_stalls) {
    /* Review Focus 4: each flash sector stalls the GBA's main loop 10 frames */
    Bench b;
    bench_init_trade(&b);
    fake_ruby_set_sector_stall(&b.ruby, 10);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_TRUE(!fake_ruby_save_corrupt(&b.ruby));
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_EQ(club_status(&b).last_result, Gen3TradeResultDone);
}

TEST(test_trade_early_dcba_trips_fake) {
    /* §B4.3: a DCBA in the animation makes Ruby skip TradeMons and save the untraded party */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_trade_link(&b, 600));
    run(&b, 40);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyTrade);
    ASSERT_TRUE(bench_inject_code(&b, GEN3_TRADE_ANIM_ACK, 0));
    for(int i = 0; i < 30 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "early dcba");
}

TEST(test_trade_extra_2ffe_trips_fake) {
    /* §B5.3: a second 2FFE for sync 1 would let sync 2 finish without the Flipper */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_save_syncs(&b, 1, 1000));
    run(&b, 3);
    bench_inject_cmd(&b, 0x2FFE, 0);
    for(int i = 0; i < 30 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stray 2ffe");
}

TEST(test_club_offer_guard) {
    /* D6: an app that hasn't seen the latest trade can't overwrite the received Pokemon */
    Bench b;
    uint8_t x[GEN3_PARTY_SIZE];
    uint8_t z[GEN3_PARTY_SIZE];
    tm_make(255, 12, 31, z);
    bench_init_trade(&b);
    memcpy(x, b.party[2], GEN3_PARTY_SIZE);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_TRUE(!gen3_club_set_offer(&b.club, z, 0));
    ASSERT_TRUE(club_status(&b).has_offer);
    ASSERT_MEM_EQ(b.club.offer, x, GEN3_PARTY_SIZE); /* still the received Pokemon */
    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 1));
    ASSERT_MEM_EQ(b.club.offer, z, GEN3_PARTY_SIZE);
}

TEST(test_trade_unexpected_1144) {
    /* D2: a 0x1144 block while no trade is pending gets a 0x1133 block (the GBA stalls before
     * anything is written) and Error/Unexpected */
    Bench b;
    uint8_t mon[GEN3_PARTY_SIZE];
    bench_init_cfg(&b, GEN3_LINKTYPE_TRADE_DISCONNECTED, &club_cfg_trade);
    tm_make(252, 5, 21, mon);
    ASSERT_TRUE(bench_offer(&b, mon));
    fake_ruby_open_link(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 400));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorUnexpected);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_DISCONNECTED);
    run(&b, 400);
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->link_type, GEN3_LINKTYPE_TRADE_SETUP);
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->lp_field_2, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(club_status(&b).last_result, Gen3TradeResultNone);
}

TEST(test_trade_pending_dropped_by_relink) {
    /* D3: a pending trade outlives the GBA closing the menu link, but the GBA's next block of a
     * type other than 0x1144 (it restarted) drops it: the trade never began on the GBA */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, true);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyTrade, 120)); /* it has just closed the link */
    fake_ruby_power_off(&b.ruby);
    run(&b, 30);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_EQ(st.last_result, Gen3TradeResultNone);
    ruby_relink(&b);
    ASSERT_TRUE(run_until_room(&b, 420));
    st = club_status(&b);
    ASSERT_EQ(st.last_result, Gen3TradeResultCanceled);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_EQ(st.trades_done, 0);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_SETUP);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

/* From Browsing: trade until the frame in which the club queues its answer to the GBA's sync-2
 * 2FFE (inside gen3_club_frame, before that frame's burst). `late` frames later the GBA is
 * switched off between the club's frame and its own: with late 0 the burst that would carry the
 * answer never reaches it. Then 30 frames. */
static bool power_off_after_sync2_answer(Bench* b, uint32_t late) {
    int64_t queued = -1;
    if(!run_until_trade_link(b, 600)) return false;
    for(uint32_t i = 0; i < 1000; i++) {
        MlinkTx tx;
        gen3_club_frame(&b->club, &b->rx, &tx);
        count_sent(b, &tx);
        if(queued < 0 && club_status(b).save_syncs == 2) queued = i;
        const bool off = queued >= 0 && i == queued + late;
        if(off) fake_ruby_power_off(&b->ruby);
        fake_ruby_frame(&b->ruby, &tx, &b->rx);
        bench_record(b, &tx);
        if(off) {
            run(b, 30);
            return true;
        }
        if(fake_ruby_error(&b->ruby) != NULL) return false;
    }
    return false;
}

TEST(test_trade_sync2_answer_lost) {
    /* D5: the GBA goes before the burst that would carry the club's sync-2 answer. It never
     * commits, so the club counts no trade: RolledBack */
    Bench b;
    uint8_t got[GEN3_PARTY_SIZE];
    uint32_t done = 1;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(power_off_after_sync2_answer(&b, 0));
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
    ASSERT_TRUE(fake_ruby_save_corrupt(&b.ruby));
    ASSERT_TRUE(saved_party_untouched(&b));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultRolledBack);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_TRUE(!st.save_window);
    ASSERT_EQ(st.trades_done, 0);
    gen3_club_get_received(&b.club, got, &done);
    ASSERT_EQ(done, 0);
    ASSERT_MEM_EQ(got, zero_mon, GEN3_PARTY_SIZE);
}

TEST(test_trade_sync2_answer_delivered) {
    /* One frame later the burst carrying the answer has reached the GBA, which commits: Assumed */
    Bench b;
    uint8_t x[GEN3_PARTY_SIZE];
    uint8_t got[GEN3_PARTY_SIZE];
    uint32_t done = 0;
    bench_init_trade(&b);
    memcpy(x, b.party[2], GEN3_PARTY_SIZE);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(power_off_after_sync2_answer(&b, 1));
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_TRUE(!fake_ruby_save_corrupt(&b.ruby));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultAssumed);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_EQ(st.trades_done, 1);
    gen3_club_get_received(&b.club, got, &done);
    ASSERT_EQ(done, 1);
    ASSERT_MEM_EQ(got, x, GEN3_PARTY_SIZE);
}

/* From Browsing: trade until the frame in which the club queues its sync-2 answer. In that
 * frame's burst the GBA answers transfers 0..answered-1 only and is gone: the club reads the rest
 * as timeouts. Those words come from a copy of the fake that took the whole burst; the fake
 * itself is switched off before the burst, since Ruby queues a command only after all 8 command
 * transfers (DoRecv). With answered 9 the fake takes the whole burst and is switched off after
 * it. Then 30 frames. */
static bool cut_sync2_answer_burst(Bench* b, uint8_t answered) {
    static FakeRuby copy;
    if(!run_until_trade_link(b, 600)) return false;
    for(uint32_t i = 0; i < 1000; i++) {
        MlinkTx tx;
        gen3_club_frame(&b->club, &b->rx, &tx);
        count_sent(b, &tx);
        if(club_status(b).save_syncs != 2) {
            fake_ruby_frame(&b->ruby, &tx, &b->rx);
            bench_record(b, &tx);
            if(fake_ruby_error(&b->ruby) != NULL) return false;
            continue;
        }
        if(answered >= MLINK_BURST_LEN) {
            fake_ruby_frame(&b->ruby, &tx, &b->rx);
        } else {
            copy = b->ruby;
            fake_ruby_frame(&copy, &tx, &b->rx);
            for(uint8_t t = answered; t < MLINK_BURST_LEN; t++) {
                b->rx.word[t] = MLINK_NO_DATA;
                b->rx.timeout_mask |= (uint16_t)(1u << t);
            }
        }
        fake_ruby_power_off(&b->ruby);
        bench_record(b, &tx);
        run(b, 30);
        return true;
    }
    return false;
}

TEST(test_trade_sync2_answer_partial_burst) {
    /* Only a whole burst after the sync-2 answer confirms it. Cut after 1..8 of its 9 transfers,
     * the GBA never took the answer: RolledBack. All 9: it committed, Assumed */
    for(uint8_t answered = 1; answered <= MLINK_BURST_LEN; answered++) {
        Bench b;
        const bool whole = answered == MLINK_BURST_LEN;
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        ASSERT_TRUE(cut_sync2_answer_burst(&b, answered));
        ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), whole ? 1 : 0);
        Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
        ASSERT_EQ(st.last_result, whole ? Gen3TradeResultAssumed : Gen3TradeResultRolledBack);
        ASSERT_EQ(st.trades_done, whole ? 1 : 0);
        ASSERT_TRUE(!st.trade_pending);
    }
}

TEST(test_trade_sync2_answer_checksum_error) {
    /* A burst that fails the checksum check doesn't confirm the sync-2 answer. One idle word of
     * the GBA's sync-2 burst is glitched as the Flipper reads it, so the two sides' checksums of
     * it differ: the GBA's check fails in the frame it takes our answer, and its CheckErrorStatus
     * link-errors before callback2 can commit; the club reads that burst as a ChecksumError and
     * keeps the answer unconfirmed */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_trade_link(&b, 600));
    bool glitched = false;
    for(int i = 0; i < 1000 && !glitched; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        if(b.rx.word[1] == 0x2FFE && club_status(&b).save_syncs == 1) {
            b.rx.word[8] ^= 1; /* the GBA's sync-2 2FFE is in; its last word is idle (0) */
            glitched = true;
        }
    }
    ASSERT_TRUE(glitched);
    const uint32_t errors = club_status(&b).link.checksum_errors;
    step(&b); /* the club answers; the GBA takes it and link-errors */
    ASSERT_EQ(club_status(&b).save_syncs, 2);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "checksum");
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
    ASSERT_TRUE(fake_ruby_save_corrupt(&b.ruby));
    step(&b); /* the burst that carried the answer: complete, but a checksum error */
    ASSERT_EQ(club_status(&b).link.checksum_errors, errors + 1);
    ASSERT_TRUE(b.club.sync2_unconfirmed);
    fake_ruby_power_off(&b.ruby);
    run(&b, 30);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultRolledBack);
    ASSERT_EQ(st.trades_done, 0);
    ASSERT_TRUE(!st.trade_pending);
}

TEST(test_trade_early_2ffe_trips_fake) {
    /* §B5.3: a Flipper 2FFE that answers nothing, from the start of the GBA's save until its own
     * sync-1 2FFE is on the wire (Link standby set, or its 2FFE built but not sent yet), trips
     * the fake in the frame it arrives */
    uint32_t tried = 0;
    for(uint32_t delay = 0; delay < 20; delay++) {
        Bench b;
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        ASSERT_TRUE(run_until_trade_link(&b, 600));
        ASSERT_TRUE(run_until_fake_phase(&b, FakeRubySave, 1000));
        const uint8_t gba_syncs = b.n_g_2ffe;
        run(&b, delay);
        if(b.n_g_2ffe != gba_syncs) break; /* the GBA's 2FFE is out: an answer may follow */
        bench_inject_cmd(&b, 0x2FFE, 0);
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
        ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "stray 2ffe");
        tried++;
    }
    ASSERT_TRUE(tried >= 4);
}

TEST(test_trade_offer_without_species_bit) {
    /* R8: round 1 counts as showing a Pokemon only with its hasSpecies bit (byte 0x13 bit 1,
     * outside the encryption). Without it the GBA's AABB gets DDEE, never DDDD. Conservative: Ruby
     * counts by species and still shows this one. */
    Bench b;
    uint8_t mon[GEN3_PARTY_SIZE];
    tm_make(252, 5, 21, mon);
    ASSERT_TRUE((mon[0x13] & 0x02) != 0);
    mon[0x13] &= (uint8_t)~0x02;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_offer(&b, mon));
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 1);
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 60));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* ddee = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(ddee, GEN3_TRADE_PLAYER_CANCEL, 0));
    ASSERT_TRUE(replied_after(trade_block(b.g_blocks, b.n_g_blocks, 0), ddee));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.refused, Gen3RefuseNone);
    ASSERT_EQ(st.menu, Gen3ClubMenuIdle);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_second_trade) {
    /* CCDD zeroes save_syncs, so the last trade's 3 doesn't carry into the next; the second
     * trade gives the GBA's slot 2 (now Y with Friendship 70) for X */
    Bench b;
    uint8_t x[GEN3_PARTY_SIZE];
    uint8_t y_traded[GEN3_PARTY_SIZE];
    uint8_t got[GEN3_PARTY_SIZE];
    uint32_t done = 0;
    bench_init_trade(&b);
    memcpy(x, b.party[2], GEN3_PARTY_SIZE);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_EQ(club_status(&b).save_syncs, 3);
    memcpy(y_traded, fake_ruby_party(&b.ruby, 2, false), GEN3_PARTY_SIZE);
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, true);
    for(int i = 0; i < 60 && !club_status(&b).trade_pending; i++) step(&b);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_EQ(st.save_syncs, 0);
    ASSERT_TRUE(!st.save_window);
    ASSERT_EQ(st.trades_done, 1);
    for(int i = 0; i < 4000; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        st = club_status(&b);
        if(st.trades_done == 2 && fake_ruby_menu_state(&b.ruby) == FakeRubyMenuBrowsing) break;
    }
    ASSERT_EQ(st.trades_done, 2);
    ASSERT_EQ(st.last_result, Gen3TradeResultDone);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 2);
    gen3_club_get_received(&b.club, got, &done);
    ASSERT_EQ(done, 2);
    ASSERT_MEM_EQ(got, y_traded, GEN3_PARTY_SIZE);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), y_traded, GEN3_PARTY_SIZE);
    /* X is back in the GBA's save, Friendship 70 */
    Gen3Pkm want;
    Gen3Pkm saved;
    ASSERT_TRUE(gen3_pkm_decrypt(x, GEN3_PARTY_SIZE, &want));
    gen3_pkm_set(&want, Gen3FieldFriendship, 70);
    gen3_pkm_fix_checksum(&want);
    ASSERT_TRUE(gen3_pkm_decrypt(fake_ruby_party(&b.ruby, 2, true), GEN3_PARTY_SIZE, &saved));
    ASSERT_MEM_EQ(saved.raw, want.raw, GEN3_PARTY_SIZE);
}

TEST(test_trade_sync_skipped_trips_fake) {
    /* A stray 2222 makes the GBA resend its LinkPlayerBlock. Before sync 1 begins, the busy
     * gLinkCallback makes sub_80084A4 ignore it; once it has begun, InitBlockSend replaces the
     * standby callback. Either way sync 1 would pass without the Flipper. Swept from the start of
     * the save until the GBA's own 2FFE is on the wire */
    uint32_t tried = 0;
    for(uint32_t delay = 0; delay < 20; delay++) {
        Bench b;
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        ASSERT_TRUE(run_until_trade_link(&b, 600));
        ASSERT_TRUE(run_until_fake_phase(&b, FakeRubySave, 1000));
        const uint8_t gba_syncs = b.n_g_2ffe;
        run(&b, delay);
        if(b.n_g_2ffe != gba_syncs) break;
        bench_inject_cmd(&b, 0x2222, GEN3_LINKTYPE_TRADE_DISCONNECTED);
        for(int i = 0; i < 20 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
        ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "sync skipped");
        tried++;
    }
    ASSERT_TRUE(tried >= 4);
}

TEST(test_trade_close_skipped_trips_fake) {
    /* The same for the save's SetCloseLinkCallback (state 8, 37 frames after sync 3): Ruby would
     * hang in state 9. The stray 2222 lands mid-window (29-36 frames after sync 3 trip it) */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_save_syncs(&b, 3, 1000));
    run(&b, 32);
    bench_inject_cmd(&b, 0x2222, GEN3_LINKTYPE_TRADE_DISCONNECTED);
    for(int i = 0; i < 30 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "close skipped");
}

TEST(test_trade_lost_before_menu_close) {
    /* The GBA is lost after CCDD, before the club answered its 5FFF: it never left the trade menu,
     * so the trade never began on it (Canceled) */
    Bench b;
    uint8_t offer[GEN3_PARTY_SIZE];
    tm_make(252, 5, 21, offer);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, true);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuClosing, 60)); /* CCDD's fade */
    ASSERT_TRUE(club_status(&b).trade_pending);
    const uint32_t answers = b.sent_5fff;
    fake_ruby_power_off(&b.ruby);
    run(&b, 30);
    ASSERT_EQ(b.sent_5fff, answers);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultCanceled);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_EQ(st.trades_done, 0);
    ASSERT_TRUE(st.has_offer);
    ASSERT_MEM_EQ(b.club.offer, offer, GEN3_PARTY_SIZE);
}

TEST(test_trade_pending_survives_menu_close) {
    /* D3, the normal path: the club answers the GBA's 5FFF, the menu link closes, and the trade
     * stays pending into the 0x1144 session */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, true);
    const uint32_t answers = b.sent_5fff;
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 120)); /* the menu link closed */
    ASSERT_EQ(b.sent_5fff, answers + 1);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_EQ(st.last_result, Gen3TradeResultNone);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseTrade, 200));
    st = club_status(&b);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_EQ(st.last_result, Gen3TradeResultNone);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_DISCONNECTED);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_sync2_answer_framing_error) {
    /* A stop-bit error on the GBA's word in the burst that carried the club's sync-2 answer, every
     * transfer answered: the burst still confirms the answer. Ruby took it and commits, so a link
     * lost the next frame counts as Assumed. Reporting RolledBack there would lose the GBA's
     * traded Pokemon on both sides; the opposite misreport clones the GBA's Pokemon and loses
     * only the Flipper's offer, which can be made again */
    Bench b;
    MlinkTx tx;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_trade_link(&b, 600));
    for(int i = 0; i < 1000 && club_status(&b).save_syncs != 2; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
    ASSERT_EQ(club_status(&b).save_syncs, 2); /* the answer went out in this step's burst */
    ASSERT_TRUE(b.club.sync2_unconfirmed);
    ASSERT_EQ(b.rx.timeout_mask, 0);
    b.rx.framing_mask |= 1u << 4; /* complete and checksummed, but one stop bit low */
    gen3_club_frame(&b.club, &b.rx, &tx);
    count_sent(&b, &tx);
    ASSERT_TRUE(!b.club.sync2_unconfirmed);
    fake_ruby_power_off(&b.ruby);
    fake_ruby_frame(&b.ruby, &tx, &b.rx);
    bench_record(&b, &tx);
    run(&b, 30);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultAssumed);
    ASSERT_EQ(st.trades_done, 1);
    ASSERT_TRUE(!st.trade_pending);
}

TEST(test_trade_sync2_answer_still_queued) {
    /* The queue clause: a command queued ahead of the sync-2 answer keeps it in the queue through
     * the next clean burst, so it is still unconfirmed; the GBA is gone before the answer leaves,
     * never commits, and the club counts RolledBack */
    Bench b;
    MlinkTx tx;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_trade_link(&b, 600));
    bool sync2 = false;
    for(int i = 0; i < 1000 && !sync2; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        sync2 = b.rx.word[1] == 0x2FFE && club_status(&b).save_syncs == 1;
    }
    ASSERT_TRUE(sync2); /* the GBA's sync-2 2FFE is in b.rx; the club reads it next */
    bench_inject_cmd(&b, 0, 0); /* an idle burst, queued ahead of the answer */
    gen3_club_frame(&b.club, &b.rx, &tx);
    count_sent(&b, &tx);
    ASSERT_EQ(club_status(&b).save_syncs, 2);
    ASSERT_EQ(gen3_link_queued(&b.club.link), 1);
    ASSERT_EQ(tx.word[1], 0);
    fake_ruby_frame(&b.ruby, &tx, &b.rx);
    bench_record(&b, &tx);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(b.rx.timeout_mask, 0);
    gen3_club_frame(&b.club, &b.rx, &tx); /* a whole clean burst, the answer still queued */
    count_sent(&b, &tx);
    ASSERT_TRUE(b.club.sync2_unconfirmed);
    ASSERT_EQ(tx.word[1], 0x2FFE); /* it leaves only now */
    fake_ruby_power_off(&b.ruby);
    fake_ruby_frame(&b.ruby, &tx, &b.rx);
    bench_record(&b, &tx);
    run(&b, 30);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultRolledBack);
    ASSERT_EQ(st.trades_done, 0);
}

/* ---- Editing between trades: EEBB, the 0x1111 room, re-sitting (§B6.3, §B7) ---- */

/* Z, the Pokemon the Flipper switches to */
static void make_z(uint8_t out[GEN3_PARTY_SIZE]) {
    tm_make(255, 12, 31, out);
}

/* How many of the Flipper's 20-byte blocks carry `code`. */
static size_t f_code_count(const Bench* b, uint16_t code) {
    uint16_t codes[BENCH_BLOCKS];
    const size_t n = bench_codes(b->f_blocks, b->n_f_blocks, codes, BENCH_BLOCKS);
    size_t count = 0;
    for(size_t i = 0; i < n; i++) count += codes[i] == code;
    return count;
}

/* After the Flipper's EEBB: until the GBA is back in the room (fake Return, then Room). */
static bool run_until_back_in_room(Bench* b) {
    return run_until_fake_phase(b, FakeRubyReturn, 200) &&
           run_until_fake_phase(b, FakeRubyRoom, 200);
}

TEST(test_trade_unexpected_1111) {
    /* D2: a 0x1111 block that no EEBB of the Flipper's led to gets a 0x1133 block (the GBA hangs
     * before its room) and Error/Unexpected */
    Bench b;
    bench_init_cfg(&b, GEN3_LINKTYPE_TRADE, &club_cfg_trade);
    fake_ruby_open_link(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 400));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorUnexpected);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE);
    ASSERT_TRUE(!st.resit_needed);
    run(&b, 400);
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->link_type, GEN3_LINKTYPE_TRADE_SETUP);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_edit_in_menu_sends_eebb) {
    /* Review Focus 1, §B7: the offer changes while the GBA browses. One unsolicited EEBB sends it
     * back to the room (0x1111, its avatar still on its seat); it steps off and back on, and the
     * fresh exchange shows the new Pokemon */
    Bench b;
    uint8_t z[GEN3_PARTY_SIZE];
    char name[16];
    make_z(z);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(!club_status(&b).offer_stale);
    const uint32_t at = b.bursts;
    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 0));
    step(&b);
    ASSERT_TRUE(club_status(&b).offer_stale);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* eebb = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(eebb, GEN3_TRADE_BOTH_CANCEL, 0));
    ASSERT_TRUE(eebb->first_burst <= at + 12);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    ASSERT_TRUE(fake_ruby_relink_frames(&b.ruby) < 100);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE);
    ASSERT_TRUE(st.partner_valid);
    ASSERT_TRUE(st.resit_needed);
    ASSERT_TRUE(!st.offer_stale);
    /* §B6.3: the GBA checked the Flipper's 0x1111 LinkPlayer against its Part A copy */
    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->link_type, GEN3_LINKTYPE_TRADE);
    ASSERT_EQ(p->trainer_id, 22222);
    gen3_text_decode(p->name, sizeof(p->name), name, sizeof(name));
    ASSERT_STR_EQ(name, "FLIPPER");
    ASSERT_TRUE(fake_ruby_on_seat(&b.ruby));

    /* Standing on the seat never triggers it again (§B1.1) */
    const uint32_t sits = b.sent_cafe16;
    fake_ruby_sit(&b.ruby, 1);
    run(&b, 300);
    ASSERT_EQ(b.sent_cafe16, sits);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_TRUE(club_status(&b).resit_needed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    fake_ruby_resit(&b.ruby);
    ASSERT_TRUE(run_until_browsing(&b, 800));
    ASSERT_EQ(b.sent_cafe16, sits + 1);
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 1);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), z, GEN3_PARTY_SIZE);
    st = club_status(&b);
    ASSERT_TRUE(!st.resit_needed);
    ASSERT_EQ(st.phase, Gen3ClubPhaseMenu);
    ASSERT_TRUE(!st.offer_stale);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_BOTH_CANCEL), 1);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_edit_after_dddd_never_ccdd) {
    /* Save safety (§B3.4): the offer changes after the DDDD, while the GBA confirms. No EEBB
     * interrupts its prompt; its YES (BBBB) gets EEBB, never CCDD */
    Bench b;
    uint8_t z[GEN3_PARTY_SIZE];
    uint16_t codes[8];
    make_z(z);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuConfirming, 60));
    ASSERT_EQ(club_status(&b).menu, Gen3ClubMenuOffered);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 0));
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 200));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1); /* the DDDD only */
    fake_ruby_answer(&b.ruby, true);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
    ASSERT_EQ(bench_codes(b.f_blocks, b.n_f_blocks, codes, 8), 2);
    ASSERT_EQ(codes[0], GEN3_TRADE_SET_MONS);
    ASSERT_EQ(codes[1], GEN3_TRADE_BOTH_CANCEL);
    const BenchBlock* bbbb = trade_block(b.g_blocks, b.n_g_blocks, 1);
    ASSERT_TRUE(trade_block_is(bbbb, GEN3_TRADE_CONFIRM, 2));
    ASSERT_TRUE(replied_after(bbbb, trade_block(b.f_blocks, b.n_f_blocks, 1)));
    ASSERT_TRUE(!club_status(&b).trade_pending);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    run(&b, 100);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_START), 0);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_edit_before_reexchange) {
    /* §B6.1, D6: an offer set before the post-save exchange's round 1 is what that exchange shows,
     * with no EEBB and no GBA input */
    Bench b;
    uint8_t z[GEN3_PARTY_SIZE];
    bool answered = false;
    bool set = false;
    make_z(z);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    for(int i = 0; i < 4000; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        const FakeRubyMenuState menu = fake_ruby_menu_state(&b.ruby);
        if(!answered && menu == FakeRubyMenuPrompt) {
            fake_ruby_answer(&b.ruby, true);
            answered = true;
        }
        if(!set && club_status(&b).trades_done == 1) {
            ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 1)); /* the frame of sync 3 */
            set = true;
        }
        if(set && menu == FakeRubyMenuBrowsing) break;
    }
    ASSERT_TRUE(set);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), z, GEN3_PARTY_SIZE);
    run(&b, 2000);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_BOTH_CANCEL), 0);
    ASSERT_TRUE(!club_status(&b).offer_stale);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_two_round_trips) {
    /* Review Focus 1: trade, edit the offer (EEBB, back to the room, re-sit), trade again */
    Bench b;
    uint8_t z[GEN3_PARTY_SIZE];
    uint8_t y_traded[GEN3_PARTY_SIZE];
    uint8_t got[GEN3_PARTY_SIZE];
    uint32_t done = 0;
    Gen3Pkm want;
    Gen3Pkm have;
    make_z(z);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 1));
    ASSERT_TRUE(run_until_back_in_room(&b));
    fake_ruby_resit(&b.ruby);
    ASSERT_TRUE(run_until_browsing(&b, 800));
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), z, GEN3_PARTY_SIZE);
    memcpy(y_traded, fake_ruby_party(&b.ruby, 2, false), GEN3_PARTY_SIZE);
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    /* The GBA saved Z with Friendship 70 in slot 2 */
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 2);
    tm_make_pkm(255, 12, 31, &want);
    gen3_pkm_set(&want, Gen3FieldFriendship, 70);
    gen3_pkm_fix_checksum(&want);
    ASSERT_TRUE(gen3_pkm_decrypt(fake_ruby_party(&b.ruby, 2, true), GEN3_PARTY_SIZE, &have));
    ASSERT_MEM_EQ(have.raw, want.raw, GEN3_PARTY_SIZE);

    /* The Flipper received the GBA's slot 2 before trade 2: Y with Friendship 70 */
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.trades_done, 2);
    ASSERT_EQ(st.last_result, Gen3TradeResultDone);
    gen3_club_get_received(&b.club, got, &done);
    ASSERT_EQ(done, 2);
    ASSERT_MEM_EQ(got, y_traded, GEN3_PARTY_SIZE);
    traded_offer(&want);
    ASSERT_TRUE(gen3_pkm_decrypt(got, GEN3_PARTY_SIZE, &have));
    ASSERT_MEM_EQ(have.raw, want.raw, GEN3_PARTY_SIZE);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_BOTH_CANCEL), 1);
}

TEST(test_trade_eebb_retry_after_summary) {
    /* Review Focus 4, §B3.4 (link-protocol-trade.md T2): an EEBB that arrives while the GBA shows
     * a summary is lost; the club resends it once GEN3_CLUB_EEBB_RETRY_BURSTS pass without the
     * GBA's 5FFF */
    Bench b;
    uint8_t z[GEN3_PARTY_SIZE];
    make_z(z);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_open_summary(&b.ruby);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuSummary, 5));
    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 0));
    run(&b, 100);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* first = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(first, GEN3_TRADE_BOTH_CANCEL, 0));
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuSummary);
    fake_ruby_close_summary(&b.ruby);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuBrowsing, 5)); /* the EEBB is lost */
    const uint32_t answers = b.sent_5fff;
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 200));
    ASSERT_EQ(b.sent_5fff, answers + 1);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 2);
    const BenchBlock* second = trade_block(b.f_blocks, b.n_f_blocks, 1);
    ASSERT_TRUE(trade_block_is(second, GEN3_TRADE_BOTH_CANCEL, 0));
    ASSERT_TRUE(second->first_burst >= first->first_burst + 180);
    ASSERT_TRUE(second->first_burst <= first->first_burst + 190);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_BOTH_CANCEL), 2);
    ASSERT_EQ(club_status(&b).link_type, GEN3_LINKTYPE_TRADE);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_checksum_glitch_resits) {
    /* Review Focus 4, D4: a checksum error the Flipper counts in the party exchange (a glitch at
     * its own receiver: the GBA is fine) taints the session. The GBA gets an unsolicited EEBB,
     * never DDDD; it re-sits, and the clean exchange trades */
    Bench b;
    uint16_t codes[8];
    bool glitched = false;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 900 && club_status(&b).phase != Gen3ClubPhaseMenu; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        if(!glitched && club_status(&b).round == 2) {
            fake_ruby_corrupt_next_checksum(&b.ruby);
            glitched = true;
        }
    }
    ASSERT_TRUE(glitched);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseMenu);
    ASSERT_EQ(st.link.checksum_errors, 1);
    ASSERT_TRUE(st.offer_stale);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 200));
    ASSERT_EQ(bench_codes(b.f_blocks, b.n_f_blocks, codes, 8), 1);
    ASSERT_EQ(codes[0], GEN3_TRADE_BOTH_CANCEL);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    fake_ruby_resit(&b.ruby);
    ASSERT_TRUE(run_until_browsing(&b, 800));
    st = club_status(&b);
    ASSERT_EQ(st.link.checksum_errors, 1);
    ASSERT_TRUE(!st.offer_stale);
    ASSERT_TRUE(bench_trade(&b, 0));
    ASSERT_EQ(bench_codes(b.f_blocks, b.n_f_blocks, codes, 8), 4);
    ASSERT_EQ(codes[0], GEN3_TRADE_BOTH_CANCEL);
    ASSERT_EQ(codes[1], GEN3_TRADE_SET_MONS);
    ASSERT_EQ(codes[2], GEN3_TRADE_START);
    ASSERT_EQ(codes[3], GEN3_TRADE_ANIM_ACK);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

/* From Browsing, set_offer(Z) sends the GBA back to the room. At its 0x1111 OpenLink a raw
 * master relinks instead of the club, with the club's 0x1111 LinkPlayer but this trainer id and
 * name. The fake's error after 200 frames. */
static const char* relink_1111_as(Bench* b, uint32_t trainer_id, const char* name) {
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    uint8_t z[GEN3_PARTY_SIZE];
    make_z(z);
    bench_init_trade(b);
    gen3_club_claim_game(&club_cfg_trade, &b->me, GEN3_LINKTYPE_TRADE, false, &lp, card);
    lp.trainer_id = trainer_id;
    gen3_text_encode(name, lp.name, sizeof(lp.name), 7);
    if(!bench_reach_menu(b)) return "no menu";
    if(!gen3_club_set_offer(&b->club, z, 0)) return "no offer";
    if(!run_until_fake_phase(b, FakeRubyReturn, 120)) return "no return";
    for(int i = 0; i < 40 && fake_ruby_relink_frames(&b->ruby) != UINT32_MAX; i++) step(b);
    if(fake_ruby_relink_frames(&b->ruby) != UINT32_MAX) return "no open link";
    bench_raw_relink(b, &lp, 200);
    return fake_ruby_error(&b->ruby);
}

TEST(test_trade_relink_identity_checked) {
    /* §B6.3 sub_800826C: after an EEBB the GBA checks the 0x1111 LinkPlayer against its Part A
     * copy, trainer id and name */
    Bench b;
    const char* error = relink_1111_as(&b, 22223, "FLIPPER");
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "relink mismatch");
    error = relink_1111_as(&b, 22222, "FLIPPEX");
    ASSERT_TRUE(error != NULL);
    ASSERT_STR_EQ(error, "relink mismatch");
    /* The club's own identity passes: the GBA reaches its room */
    ASSERT_TRUE(relink_1111_as(&b, 22222, "FLIPPER") == NULL);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
}

TEST(test_trade_empty_offer_eebb) {
    /* R8, D4: no offer at round 1, so the GBA's menu shows no Flipper Pokemon. Once round 5 is
     * done an unsolicited EEBB sends the GBA back to the room; never a DDDD */
    Bench b;
    uint16_t codes[8];
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 400));
    ASSERT_EQ(club_status(&b).round, 0);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 600));
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 0);
    ASSERT_EQ(bench_codes(b.f_blocks, b.n_f_blocks, codes, 8), 1);
    ASSERT_EQ(codes[0], GEN3_TRADE_BOTH_CANCEL);
    /* 6 idle bursts (and a little) after both round-5 blocks */
    const int fk = latest_block(b.f_blocks, b.n_f_blocks, 40);
    const int gk = latest_block(b.g_blocks, b.n_g_blocks, 40);
    ASSERT_TRUE(fk >= 0 && gk >= 0);
    const uint32_t done = later_burst(b.f_blocks[fk].last_burst, b.g_blocks[gk].last_burst);
    const BenchBlock* eebb = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(eebb != NULL);
    ASSERT_TRUE(eebb->first_burst >= done + 7);
    ASSERT_TRUE(eebb->first_burst <= done + 12);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_TRUE(st.resit_needed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_editing_sends_eebb) {
    /* O1: the app opens the editor while the GBA browses. One EEBB; the GBA re-sits, but the
     * Flipper doesn't sit until the edit is done, then joins it (R5) */
    Bench b;
    uint8_t offer[GEN3_PARTY_SIZE];
    tm_make(252, 5, 21, offer); /* bench_init_trade's offer */
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    const uint32_t at = b.bursts;
    ASSERT_TRUE(gen3_club_set_editing(&b.club, true, 0));
    ASSERT_TRUE(club_status(&b).editing);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    const BenchBlock* eebb = trade_block(b.f_blocks, b.n_f_blocks, 0);
    ASSERT_TRUE(trade_block_is(eebb, GEN3_TRADE_BOTH_CANCEL, 0));
    ASSERT_TRUE(eebb->first_burst <= at + 12);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE);
    ASSERT_TRUE(st.resit_needed);
    ASSERT_TRUE(st.editing); /* app state: survives the relinks */

    const uint32_t sits = b.sent_cafe16;
    fake_ruby_resit(&b.ruby);
    for(int i = 0; i < 200 && !club_status(&b).gba_seated; i++) step(&b);
    st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(!st.resit_needed);
    run(&b, 300);
    ASSERT_EQ(b.sent_cafe16, sits); /* no sit while editing */
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);

    ASSERT_TRUE(gen3_club_set_editing(&b.club, false, 0));
    ASSERT_TRUE(!club_status(&b).editing);
    for(int i = 0; i < 10 && b.sent_cafe16 == sits; i++) step(&b);
    ASSERT_EQ(b.sent_cafe16, sits + 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 200));
    ASSERT_TRUE(run_until_browsing(&b, 600));
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), offer, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!club_status(&b).offer_stale);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_editing_blocks_ccdd) {
    /* O1: while editing, the GBA's BBBB gets EEBB, never CCDD; and its AABB, never DDDD */
    {
        Bench b;
        uint16_t codes[8];
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        fake_ruby_offer(&b.ruby, 2);
        ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
        ASSERT_TRUE(gen3_club_set_editing(&b.club, true, 0));
        fake_ruby_answer(&b.ruby, true);
        ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
        ASSERT_EQ(bench_codes(b.f_blocks, b.n_f_blocks, codes, 8), 2);
        ASSERT_EQ(codes[0], GEN3_TRADE_SET_MONS);
        ASSERT_EQ(codes[1], GEN3_TRADE_BOTH_CANCEL);
        ASSERT_EQ(bench_codes(b.g_blocks, b.n_g_blocks, codes, 8), 2);
        ASSERT_EQ(codes[1], GEN3_TRADE_CONFIRM);
        ASSERT_TRUE(!club_status(&b).trade_pending);
        run(&b, 200);
        ASSERT_EQ(f_code_count(&b, GEN3_TRADE_START), 0);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
    {
        Bench b;
        uint16_t codes[8];
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        fake_ruby_offer(&b.ruby, 2); /* the same frame */
        ASSERT_TRUE(gen3_club_set_editing(&b.club, true, 0));
        ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
        ASSERT_EQ(bench_codes(b.g_blocks, b.n_g_blocks, codes, 8), 1);
        ASSERT_EQ(codes[0], GEN3_TRADE_READY);
        ASSERT_EQ(bench_codes(b.f_blocks, b.n_f_blocks, codes, 8), 1);
        ASSERT_EQ(codes[0], GEN3_TRADE_BOTH_CANCEL);
        run(&b, 200);
        ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
    {
        /* Editing begins once the AABB is in and filed, before its reply: the reply, decided as it
         * goes out, is the EEBB, with the usual delay */
        Bench b;
        uint16_t codes[8];
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        fake_ruby_offer(&b.ruby, 2);
        for(int i = 0; i < 60 && n_trade_blocks(b.g_blocks, b.n_g_blocks) == 0; i++) step(&b);
        step(&b); /* the club files the AABB from this burst's rx */
        ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
        ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 0);
        ASSERT_TRUE(gen3_club_set_editing(&b.club, true, 0));
        ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
        ASSERT_EQ(bench_codes(b.f_blocks, b.n_f_blocks, codes, 8), 1);
        ASSERT_EQ(codes[0], GEN3_TRADE_BOTH_CANCEL);
        const BenchBlock* aabb = trade_block(b.g_blocks, b.n_g_blocks, 0);
        ASSERT_TRUE(trade_block_is(aabb, GEN3_TRADE_READY, 2));
        ASSERT_TRUE(replied_after(aabb, trade_block(b.f_blocks, b.n_f_blocks, 0)));
        run(&b, 200);
        ASSERT_EQ(f_code_count(&b, GEN3_TRADE_SET_MONS), 0);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
}

TEST(test_trade_editing_refused_while_pending) {
    /* O1: editing can't begin once CCDD is queued, nor before the app has seen the trade's
     * Pokemon; clearing it always works */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, true);
    for(int i = 0; i < 60 && !club_status(&b).trade_pending; i++) step(&b);
    ASSERT_TRUE(club_status(&b).trade_pending); /* the frame CCDD is queued */
    ASSERT_EQ(club_status(&b).menu, Gen3ClubMenuStarting);
    ASSERT_TRUE(!gen3_club_set_editing(&b.club, true, 0));
    ASSERT_TRUE(!club_status(&b).editing);
    for(int i = 0; i < 4000 && club_status(&b).trades_done == 0; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        ASSERT_TRUE(!club_status(&b).editing);
    }
    ASSERT_EQ(club_status(&b).trades_done, 1);
    ASSERT_TRUE(!club_status(&b).trade_pending);
    ASSERT_TRUE(!gen3_club_set_editing(&b.club, true, 0)); /* trade 1 not seen */
    ASSERT_TRUE(!club_status(&b).editing);
    ASSERT_TRUE(gen3_club_set_editing(&b.club, true, 1));
    ASSERT_TRUE(club_status(&b).editing);
    /* The post-save menu then gets an EEBB */
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 600));
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_BOTH_CANCEL), 1);
    ASSERT_TRUE(gen3_club_set_editing(&b.club, false, 7)); /* clearing ignores trades_seen */
    ASSERT_TRUE(!club_status(&b).editing);
}

/* ---- The arbiter and the GBA's other blocks; context that outlives a session ---- */

TEST(test_trade_r_mash_eebb) {
    /* Only a block the arbiter files (or a chunk of one) restarts its delay. The GBA player mashes
     * R every 5 frames (the debug block 0000, §B3.2; each press overwrites the previous block's
     * last chunk, as InitBlockSend's BuildSendCmd does on Ruby) while an EEBB is due: the EEBB
     * still goes out */
    Bench b;
    uint8_t z[GEN3_PARTY_SIZE];
    uint32_t headers = 0;
    make_z(z);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    int i = 0;
    for(; i < 30; i++) {
        if(i % 5 == 0) fake_ruby_press_r(&b.ruby);
        step(&b);
        headers += b.rx.word[1] == 0xBBBB;
    }
    ASSERT_TRUE(headers >= 5);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 0));
    for(; i < 300 && n_trade_blocks(b.f_blocks, b.n_f_blocks) == 0; i++) {
        if(i % 5 == 0) fake_ruby_press_r(&b.ruby);
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    ASSERT_TRUE(
        trade_block_is(trade_block(b.f_blocks, b.n_f_blocks, 0), GEN3_TRADE_BOTH_CANCEL, 0));
    for(; i < 300 && fake_ruby_phase(&b.ruby) == FakeRubyMenu; i++) {
        if(i % 5 == 0) fake_ruby_press_r(&b.ruby);
        step(&b);
    }
    ASSERT_TRUE(run_until_back_in_room(&b));
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_trade_broken_block_released) {
    /* A GBA block that stops short is dropped on the 6th burst in a row without a chunk for it
     * (GEN3_CLUB_REPLY_DELAY_BURSTS), so it can't hold the arbiter */
    {
        /* A glitched size (0x34 > 28): the block can't complete. The Flipper counted a checksum
         * error, so the tainted session's EEBB goes out (a 52-byte block is never filed, so it
         * doesn't hold the arbiter), and the block is dropped */
        Bench b;
        BenchRawGba g;
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        raw_gba_init(&g, &b, GEN3_TRADE_READY, 2);
        g.wire_size = 0x34;
        raw_gba_run(&g, &b, 9); /* the club has the header, both chunks and 5 idle bursts */
        ASSERT_TRUE(b.club.block_rx.active); /* 28 of 52 bytes */
        raw_gba_run(&g, &b, 1);
        ASSERT_TRUE(!b.club.block_rx.active); /* the 6th burst without a chunk */
        Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.link.checksum_errors, 1);
        ASSERT_TRUE(st.offer_stale);
        ASSERT_EQ(st.menu, Gen3ClubMenuLeaving);
        ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
        ASSERT_TRUE(
            trade_block_is(trade_block(b.f_blocks, b.n_f_blocks, 0), GEN3_TRADE_BOTH_CANCEL, 0));
    }
    {
        /* A 20-byte AABB whose second chunk never comes, with an EEBB due (the offer changed
         * meanwhile): the block holds the arbiter until it is dropped, then the EEBB goes out */
        Bench b;
        BenchRawGba g;
        uint8_t z[GEN3_PARTY_SIZE];
        make_z(z);
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        raw_gba_init(&g, &b, GEN3_TRADE_READY, 2);
        g.chunks = 1;
        const uint32_t chunk = b.bursts + 2; /* the burst of the one chunk */
        raw_gba_run(&g, &b, 2); /* the club has the header */
        ASSERT_TRUE(b.club.block_rx.active);
        ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 0));
        raw_gba_run(&g, &b, 6); /* the chunk, then 5 bursts without */
        ASSERT_TRUE(b.club.block_rx.active);
        ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 0);
        raw_gba_run(&g, &b, 1);
        ASSERT_TRUE(!b.club.block_rx.active);
        raw_gba_run(&g, &b, 20);
        ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
        const BenchBlock* eebb = trade_block(b.f_blocks, b.n_f_blocks, 0);
        ASSERT_TRUE(trade_block_is(eebb, GEN3_TRADE_BOTH_CANCEL, 0));
        /* The chunk restarted the delay and the block held it until the drop, on the 6th burst
         * after the chunk: then 6 idle bursts */
        ASSERT_TRUE(eebb->first_burst >= chunk + 13);
        ASSERT_TRUE(eebb->first_burst <= chunk + 14);
        ASSERT_EQ(club_status(&b).menu, Gen3ClubMenuLeaving);
        ASSERT_EQ(club_status(&b).link.checksum_errors, 0);
    }
}

TEST(test_trade_lagged_block_answered) {
    /* A filed AABB whose second chunk comes after GBA lag frames (2, and 5, the most the drop
     * allows) is still complete, and gets its one DDDD with the usual delay */
    static const uint8_t lags[] = {2, 5};
    for(size_t i = 0; i < sizeof(lags); i++) {
        Bench b;
        BenchRawGba g;
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        raw_gba_init(&g, &b, GEN3_TRADE_READY, 2);
        g.lag = lags[i];
        raw_gba_run(&g, &b, 30);
        ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 1);
        const BenchBlock* aabb = trade_block(b.g_blocks, b.n_g_blocks, 0);
        ASSERT_TRUE(trade_block_is(aabb, GEN3_TRADE_READY, 2));
        ASSERT_EQ(aabb->last_burst, aabb->first_burst + 2u + lags[i]);
        ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
        const BenchBlock* dddd = trade_block(b.f_blocks, b.n_f_blocks, 0);
        ASSERT_TRUE(trade_block_is(dddd, GEN3_TRADE_SET_MONS, 0));
        ASSERT_TRUE(replied_after(aabb, dddd));
        const Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.menu, Gen3ClubMenuOffered);
        ASSERT_EQ(st.gba_slot, 2);
        ASSERT_EQ(st.link.checksum_errors, 0);
    }
}

TEST(test_trade_resit_needed_survives_disconnect) {
    /* resit_needed is about the GBA's avatar, which a lost link doesn't move: it stays set through
     * a Disconnected (only the GBA's CAFE 0016 or a 0x1133 linkup clears it) */
    Bench b;
    uint8_t z[GEN3_PARTY_SIZE];
    make_z(z);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 0));
    ASSERT_TRUE(run_until_back_in_room(&b));
    ASSERT_TRUE(club_status(&b).resit_needed);
    fake_ruby_power_off(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 30));
    ASSERT_TRUE(club_status(&b).resit_needed);
    run(&b, 100);
    ASSERT_TRUE(club_status(&b).resit_needed);
}

TEST(test_trade_return_due_consumed) {
    /* The 0x1111 the Flipper's EEBB announced is followed only by the GBA's next LinkPlayerBlock,
     * whatever its type. Here the GBA restarts after the EEBB and walks in with 0x1133; a stray
     * 0x1111 later gets 0x1133 and Error/Unexpected (D2) */
    Bench b;
    uint8_t z[GEN3_PARTY_SIZE];
    make_z(z);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 0));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120)); /* before its 0x1111 OpenLink */
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_BOTH_CANCEL), 1);
    fake_ruby_power_off(&b.ruby);
    run(&b, 30);
    ruby_relink(&b);
    ASSERT_TRUE(run_until_room(&b, 420));
    ASSERT_EQ(club_status(&b).link_type, GEN3_LINKTYPE_TRADE_SETUP);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    fake_ruby_power_off(&b.ruby);
    run(&b, 30);
    b.me.link_type = GEN3_LINKTYPE_TRADE;
    ruby_relink(&b);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 400));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorUnexpected);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE);
    run(&b, 100);
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->link_type, GEN3_LINKTYPE_TRADE_SETUP);
}

/* ---- Leaving the Cable Club (D9, §B7) ---- */

/* Until the GBA has closed the link of its walk out (fake Closed), then left within 3 frames:
 * the exit's close is the end at once, with no relink awaited. */
static bool run_until_closed_then_left(Bench* b, uint32_t max_frames) {
    return run_until_fake_phase(b, FakeRubyClosed, max_frames) && run_until_left(b, 3);
}

/* After left: the GBA stays out (the link closed, Closed: no relink), the club in Waiting, and no
 * further CAFE 0017 or 5FFF. */
static bool stays_left(Bench* b) {
    const uint32_t exits = b->sent_cafe17;
    const uint32_t closes = b->sent_5fff;
    for(int i = 0; i < 200; i++) {
        step(b);
        const Gen3ClubStatus st = club_status(b);
        if(!st.left || st.leaving || st.phase != Gen3ClubPhaseWaiting) return false;
        if(fake_ruby_phase(&b->ruby) != FakeRubyClosed || fake_ruby_error(&b->ruby)) return false;
    }
    return b->sent_cafe17 == exits && b->sent_5fff == closes;
}

TEST(test_leave_from_room) {
    /* D9, §B7: one CAFE 0017 makes the idle GBA run its own exit (TradeRoom_TerminateLink). Its
     * own 0017 then isn't answered; its 5FFF is, and it closes the link */
    Bench b;
    ASSERT_TRUE(reach_room(&b));
    run(&b, 40);
    gen3_club_request_leave(&b.club);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.leaving);
    ASSERT_TRUE(!st.left);
    ASSERT_TRUE(run_until_closed_then_left(&b, 600));
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_TRUE(b.g_cafe_seen & (1u << 0x17)); /* the GBA's own 0017 */
    ASSERT_EQ(b.sent_5fff, 1);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_TRUE(!st.leaving);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_TRUE(stays_left(&b));
}

TEST(test_leave_from_menu) {
    /* D9: in the trade menu the request makes the offer stale: EEBB, the 0x1111 room, then the
     * CAFE 0017 once the GBA's fade-in is over. Leaving survives the relink's Waiting */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    gen3_club_request_leave(&b.club);
    ASSERT_TRUE(club_status(&b).offer_stale);
    bool returned = false;
    for(int i = 0; i < 400 && fake_ruby_phase(&b.ruby) != FakeRubyRoom; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        returned = returned || fake_ruby_phase(&b.ruby) == FakeRubyReturn;
        const Gen3ClubStatus st = club_status(&b);
        ASSERT_TRUE(st.leaving);
        ASSERT_TRUE(!st.left);
        ASSERT_TRUE(!st.flipper_seated);
        ASSERT_EQ(b.sent_cafe17, 0); /* not before the fade-in ends (fake Room) */
    }
    ASSERT_TRUE(returned);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_EQ(club_status(&b).link_type, GEN3_LINKTYPE_TRADE);
    ASSERT_EQ(n_trade_blocks(b.f_blocks, b.n_f_blocks), 1);
    ASSERT_TRUE(
        trade_block_is(trade_block(b.f_blocks, b.n_f_blocks, 0), GEN3_TRADE_BOTH_CANCEL, 0));
    ASSERT_TRUE(run_until_closed_then_left(&b, 600));
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_TRUE(b.g_cafe_seen & (1u << 0x17));
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
    ASSERT_TRUE(stays_left(&b));
}

TEST(test_leave_as_trade_starts) {
    /* D9: asked once both sit, when the GBA starts the trade menu (§B1.3 row 4): no 0017 in that
     * room. The club follows the GBA to the menu (EEBB) and back to the room (the one 0017) */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    run(&b, 40);
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 200 && !club_status(&b).flipper_seated; i++) step(&b);
    ASSERT_TRUE(club_status(&b).flipper_seated);
    gen3_club_request_leave(&b.club);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 200));
    ASSERT_EQ(b.sent_cafe17, 0);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 800)); /* the exchange, then EEBB */
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    ASSERT_TRUE(run_until_closed_then_left(&b, 600));
    ASSERT_EQ(b.sent_cafe16, 1);
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_BOTH_CANCEL), 1);
    ASSERT_TRUE(stays_left(&b));
}

TEST(test_leave_while_seated) {
    /* §B1.3 row 1: the Flipper's 0017 makes a seated GBA cancel its seat; the seat script ends
     * with 0x1A (IDLE), then the forced exit sends 0x17 */
    Bench b;
    uint32_t idle_burst = 0;
    uint32_t exit_burst = 0;
    bench_init_trade(&b);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 60);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(club_status(&b).gba_seated);
    gen3_club_request_leave(&b.club);
    for(int i = 0; i < 600 && !club_status(&b).left; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        if(b.rx.word[1] == 0xCAFE && b.rx.word[2] == 0x001A && !idle_burst) idle_burst = b.bursts;
        if(b.rx.word[1] == 0xCAFE && b.rx.word[2] == 0x0017 && !exit_burst) exit_burst = b.bursts;
    }
    ASSERT_TRUE(club_status(&b).left);
    ASSERT_TRUE(idle_burst != 0);
    ASSERT_TRUE(exit_burst > idle_burst);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_TRUE(!club_status(&b).gba_seated);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(stays_left(&b));
}

TEST(test_leave_never_sits) {
    /* D9 with R5: once leaving, the club never sits, even with an offer and the GBA seated. A
     * Flipper 0x16 after its 0x17 would turn its EXITING slot READY and start the trade menu */
    Bench b;
    bool seated_while_leaving = false;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    gen3_club_request_leave(&b.club);
    for(int i = 0; i < 600 && !club_status(&b).left; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        const Gen3ClubStatus st = club_status(&b);
        seated_while_leaving = seated_while_leaving || (st.gba_seated && st.leaving);
    }
    ASSERT_TRUE(club_status(&b).left);
    ASSERT_TRUE(seated_while_leaving);
    ASSERT_TRUE(b.g_cafe_seen & (1u << 0x16));
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_TRUE(stays_left(&b));
}

TEST(test_leave_gba_walks_back_in) {
    /* D9: left is sticky. A GBA that walks back into the Cable Club after End trade and sits gets
     * no Flipper seat, so no trade can start: neither as the answer to its seat, nor as a late
     * sit (R5) once the Flipper has a new offer */
    for(int late = 0; late < 2; late++) {
        Bench b;
        uint8_t z[GEN3_PARTY_SIZE];
        make_z(z);
        bench_init_trade(&b);
        if(late) ASSERT_TRUE(gen3_club_set_offer(&b.club, NULL, 0));
        ASSERT_TRUE(bench_to_room(&b));
        run(&b, 40);
        gen3_club_request_leave(&b.club);
        ASSERT_TRUE(run_until_closed_then_left(&b, 600));
        run(&b, 60);
        ruby_relink(&b); /* the GBA player walks back in */
        ASSERT_TRUE(run_until_room(&b, 600));
        ASSERT_TRUE(club_status(&b).left);
        fake_ruby_sit(&b.ruby, 1);
        for(int i = 0; i < 600; i++) {
            step(&b);
            ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
            if(late && i == 300) ASSERT_TRUE(bench_offer(&b, z));
        }
        ASSERT_EQ(b.sent_cafe16, 0);
        const Gen3ClubStatus st = club_status(&b);
        ASSERT_TRUE(st.gba_seated);
        ASSERT_TRUE(!st.flipper_seated);
        ASSERT_TRUE(st.left);
        ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
        ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
        ASSERT_TRUE(!st.trade_pending);
        ASSERT_EQ(st.trades_done, 0);
        ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
    }
}

TEST(test_leave_save_outcome_shown) {
    /* D5 with D9: End trade during a trade, then the link lost in the GBA's save. The leave is
     * over (left), and the guide shows the save's outcome, not "Link ended": lost after the
     * sync-1 answer, RolledBack; after the sync-2 answer, Assumed */
    for(uint8_t syncs = 1; syncs <= 2; syncs++) {
        Bench b;
        char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        ASSERT_TRUE(run_until_trade_link(&b, 600));
        gen3_club_request_leave(&b.club);
        for(int i = 0; i < 1000 && club_status(&b).save_syncs < syncs; i++) {
            step(&b);
            ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        }
        ASSERT_EQ(club_status(&b).save_syncs, syncs);
        run(&b, 4);
        fake_ruby_power_off(&b.ruby);
        run(&b, 30);
        const Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
        ASSERT_TRUE(st.left);
        ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), syncs - 1);
        ASSERT_EQ(st.last_result, syncs == 1 ? Gen3TradeResultRolledBack : Gen3TradeResultAssumed);
        ASSERT_TRUE(gen3_trade_guide(&st, true, true, Gen3RefuseNone, Gen3PickupOk, out));
        ASSERT_STR_EQ(out[0], syncs == 1 ? "Link lost while the" : "Link lost at the end.");
    }
}

TEST(test_leave_waits_for_save) {
    /* D9, R6: asked in the frame CCDD is queued, the leave waits for the trade and its link save
     * (no 0017, no EEBB, never left); the post-save menu then gets EEBB, and the room the 0017 */
    Bench b;
    uint16_t codes[8];
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, true);
    for(int i = 0; i < 60 && !club_status(&b).trade_pending; i++) step(&b);
    ASSERT_TRUE(club_status(&b).trade_pending);
    ASSERT_EQ(club_status(&b).menu, Gen3ClubMenuStarting);
    gen3_club_request_leave(&b.club);
    for(int i = 0; i < 4000 && club_status(&b).trades_done == 0; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        const Gen3ClubStatus st = club_status(&b);
        ASSERT_TRUE(st.leaving);
        ASSERT_TRUE(!st.left);
    }
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.trades_done, 1);
    ASSERT_EQ(st.last_result, Gen3TradeResultDone);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_BOTH_CANCEL), 0);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 1500)); /* the post-save menu's EEBB */
    ASSERT_EQ(bench_codes(b.f_blocks, b.n_f_blocks, codes, 8), 4);
    ASSERT_EQ(codes[0], GEN3_TRADE_SET_MONS);
    ASSERT_EQ(codes[1], GEN3_TRADE_START);
    ASSERT_EQ(codes[2], GEN3_TRADE_ANIM_ACK);
    ASSERT_EQ(codes[3], GEN3_TRADE_BOTH_CANCEL);
    ASSERT_EQ(b.sent_cafe17, 0);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    ASSERT_TRUE(run_until_left(&b, 600));
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_EQ(club_status(&b).last_result, Gen3TradeResultDone);
    ASSERT_TRUE(stays_left(&b));
}

TEST(test_leave_left_menu_stale) {
    /* End trade in the gap before a pending trade's 0x1144 relink, which comes only after
     * GEN3_CLUB_RELINK_WAIT_FRAMES: the GBA is taken as gone (left) with the trade still pending.
     * The late relink then finishes the trade and its link save, and the post-save trade menu is
     * stale (left): EEBB, never a second trade, and no seat in the room the GBA goes back to */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, true);
    /* Until the GBA has closed the CCDD's 0x1122 link: its main loop then stalls, so its 0x1144
     * OpenLink waits past the club's relink wait */
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyTrade, 400));
    fake_ruby_stall(&b.ruby, 255);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 10));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.relinking);
    ASSERT_TRUE(st.trade_pending);
    gen3_club_request_leave(&b.club);
    ASSERT_TRUE(club_status(&b).leaving);
    for(int i = 0; i <= GEN3_CLUB_RELINK_WAIT_FRAMES && !club_status(&b).left; i++) {
        if(i % 200 == 0) fake_ruby_stall(&b.ruby, 255);
        step(&b);
    }
    st = club_status(&b);
    ASSERT_TRUE(st.left);
    ASSERT_TRUE(!st.leaving);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    /* The late relink: the trade and the link save */
    for(int i = 0; i < 4000 && club_status(&b).trades_done == 0; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
    st = club_status(&b);
    ASSERT_EQ(st.trades_done, 1);
    ASSERT_EQ(st.last_result, Gen3TradeResultDone);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);

    /* The post-save menu. The GBA can't offer in it: the club's EEBB comes while the GBA still
     * builds the menu, and its first menu update handles that EEBB before any input. So the GBA
     * sends no trade block after the first trade's ABCD */
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseMenu, 1500));
    ASSERT_TRUE(club_status(&b).offer_stale);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 400));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    ASSERT_EQ(n_trade_blocks(b.g_blocks, b.n_g_blocks), 3); /* AABB, BBBB, ABCD */
    ASSERT_EQ(trade_block(b.g_blocks, b.n_g_blocks, 2)->code, GEN3_TRADE_ANIM_DONE);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_SET_MONS), 1); /* the first trade's only */
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_START), 1);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_BOTH_CANCEL), 1);
    const uint32_t sat = b.sent_cafe16;
    fake_ruby_resit(&b.ruby);
    run(&b, 300);
    ASSERT_EQ(b.sent_cafe16, sat);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_SET_MONS), 1);
    ASSERT_EQ(f_code_count(&b, GEN3_TRADE_START), 1);
    ASSERT_EQ(club_status(&b).trades_done, 1);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_leave_when_waiting) {
    /* D9: no GBA linked: left at once */
    Bench b;
    bench_init_trade(&b);
    run(&b, 10);
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseWaiting);
    gen3_club_request_leave(&b.club);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.left);
    ASSERT_TRUE(!st.leaving);
    run(&b, 10);
    st = club_status(&b);
    ASSERT_TRUE(st.left);
    ASSERT_TRUE(!st.leaving);
    ASSERT_EQ(b.sent_cafe17, 0);
}

TEST(test_leave_in_relink_gap) {
    /* D9: Waiting between the EEBB's close and the GBA's 0x1111 relink is no "no GBA linked": a
     * request there waits for the room and walks the GBA out */
    Bench b;
    uint8_t z[GEN3_PARTY_SIZE];
    make_z(z);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 0));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 60));
    gen3_club_request_leave(&b.club);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.leaving);
    ASSERT_TRUE(!st.left);
    ASSERT_TRUE(run_until_left(&b, 800));
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_TRUE(b.g_cafe_seen & (1u << 0x17));
    ASSERT_TRUE(stays_left(&b));
}

TEST(test_leave_gba_gone) {
    /* D9: the link lost for good while leaving is left too: a power-off with the GBA linked at
     * once, and in a relink's gap after GEN3_CLUB_RELINK_WAIT_FRAMES */
    {
        Bench b;
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        gen3_club_request_leave(&b.club);
        step(&b);
        fake_ruby_power_off(&b.ruby);
        ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 30));
        Gen3ClubStatus st = club_status(&b);
        ASSERT_TRUE(st.left);
        ASSERT_TRUE(!st.leaving);
    }
    {
        Bench b;
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        gen3_club_request_leave(&b.club);
        ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
        ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 60)); /* the Disconnected frame */
        fake_ruby_power_off(&b.ruby);
        run(&b, 298);
        Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
        ASSERT_TRUE(st.leaving);
        ASSERT_TRUE(!st.left);
        step(&b); /* 300 frames since the Disconnected */
        st = club_status(&b);
        ASSERT_TRUE(st.left);
        ASSERT_TRUE(!st.leaving);
    }
    {
        /* Without a request, the gap's end makes a later request immediate */
        Bench b;
        uint8_t z[GEN3_PARTY_SIZE];
        make_z(z);
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 0));
        ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 120));
        ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 60));
        fake_ruby_power_off(&b.ruby);
        run(&b, 298);
        gen3_club_request_leave(&b.club);
        ASSERT_TRUE(club_status(&b).leaving);
        step(&b);
        ASSERT_TRUE(club_status(&b).left);
        gen3_club_request_leave(&b.club);
        ASSERT_TRUE(club_status(&b).left);
        ASSERT_TRUE(!club_status(&b).leaving);
    }
}

TEST(test_status_relinking) {
    /* Waiting in a relink's gap is no "no GBA linked". status.relinking shows it from the club's
     * 5FFF answer until the GBA connects again, or until the relink wait runs out (the GBA gone),
     * and the end decisions read it: End trade asks, "Stop link?" is refused while the trade is
     * pending, and both stop once the wait has run out (D1). */
    {
        /* The 0x1144 gap after CCDD, the relink made */
        Bench b;
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        ASSERT_TRUE(!club_status(&b).relinking);
        fake_ruby_offer(&b.ruby, 2);
        ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
        fake_ruby_answer(&b.ruby, true);
        ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 120)); /* the menu link closed */
        Gen3ClubStatus st = club_status(&b);
        ASSERT_TRUE(st.trade_pending);
        ASSERT_TRUE(st.relinking);
        ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndAsk);
        ASSERT_TRUE(!gen3_trade_stop_allowed(&st));
        ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseTrade, 200));
        ASSERT_TRUE(!club_status(&b).relinking);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
    {
        /* The same gap with the GBA switched off in it: relinking until the wait runs out */
        Bench b;
        bench_init_trade(&b);
        ASSERT_TRUE(bench_reach_menu(&b));
        fake_ruby_offer(&b.ruby, 2);
        ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
        fake_ruby_answer(&b.ruby, true);
        ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 120)); /* the Disconnected frame */
        fake_ruby_power_off(&b.ruby);
        run(&b, GEN3_CLUB_RELINK_WAIT_FRAMES - 2);
        Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
        ASSERT_TRUE(st.trade_pending);
        ASSERT_TRUE(st.relinking);
        ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndAsk);
        ASSERT_EQ(gen3_trade_stop_link_action(&st, true, true), Gen3TradeEndAsk);
        step(&b); /* GEN3_CLUB_RELINK_WAIT_FRAMES since the Disconnected */
        st = club_status(&b);
        ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
        ASSERT_TRUE(!st.relinking);
        ASSERT_TRUE(gen3_trade_stop_allowed(&st));
        ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndStop);
        ASSERT_EQ(gen3_trade_stop_link_action(&st, true, true), Gen3TradeEndStop);
    }
}

/* The trade screen's guide from the real status, frame by frame (test_guide_through_relinks) */
typedef struct {
    bool in_club; /* the club has reached Room */
    bool in_gap; /* the last frame was Waiting with relinking */
    uint32_t gaps; /* relink gaps entered */
    uint32_t gap_frames;
} GuideWatch;

/* One step. False on a fake error, a guide text cut, or, once the club has reached Room, a
 * Waiting row ("Go to the Cable Club", "Link lost ...") or, in a gap, anything but "Linking..."
 * or "Trading...". */
static bool guide_step(Bench* b, GuideWatch* w) {
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
    step(b);
    const Gen3ClubStatus st = club_status(b);
    const bool gap = st.phase == Gen3ClubPhaseWaiting && st.relinking;
    if(st.phase == Gen3ClubPhaseRoom) w->in_club = true;
    if(gap && !w->in_gap) w->gaps++;
    if(gap) w->gap_frames++;
    w->in_gap = gap;
    if(fake_ruby_error(&b->ruby) != NULL) return false;
    if(!gen3_trade_guide(&st, true, true, Gen3RefuseNone, Gen3PickupOk, out)) return false;
    if(!w->in_club) return true;
    const bool waiting_row = strcmp(out[0], "Go to the Cable Club:") == 0 ||
                             strncmp(out[0], "Link lost", strlen("Link lost")) == 0;
    const bool gap_row = strcmp(out[0], "Linking...") == 0 || strcmp(out[0], "Trading...") == 0;
    if(waiting_row || (gap && !gap_row)) {
        printf(
            "  guide at burst %u, phase %d relinking %d: \"%s\"\n",
            (unsigned)b->bursts,
            st.phase,
            st.relinking,
            out[0]);
        return false;
    }
    return true;
}

/* Steps with guide_step until cond holds; fails the test after max frames */
#define GUIDE_UNTIL(b, w, cond, max) \
    do { \
        uint32_t n_ = 0; \
        while(!(cond)) { \
            ASSERT_TRUE(n_++ < (max)); \
            ASSERT_TRUE(guide_step((b), (w))); \
        } \
    } while(0)

TEST(test_guide_other_room) {
    /* A GBA linked as another room: the guide names its own game's Trade Center counter, never
     * the one the player just used. Ruby's Record Corner (0x3311) is its right counter, the Trade
     * Center the middle one; FireRed's COLOSSEUM (0x2233) is a choice at its right-hand
     * attendant, as TRADE CENTER is. Once the GBA has closed the link, no GBA is linked: the
     * Waiting row names both counters */
    static const struct {
        bool frlg;
        uint32_t link_type;
        const char* l2;
        const char* l3;
    } cases[] = {
        {false, 0x3311, "room. Use the middle", "counter: Trade Center"},
        {true, 0x2233, "room. Right counter,", "then TRADE CENTER."},
    };
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        Bench b;
        bench_init_cfg(&b, cases[i].link_type, &club_cfg_trade);
        if(cases[i].frlg) {
            Gen3LinkPlayer me = bench_frlg_player(GEN3_VERSION_FIRERED);
            me.link_type = cases[i].link_type;
            bench_swap_gba(&b, &me, false);
        }
        fake_ruby_open_link(&b.ruby);
        ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 400));
        Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.last_error, Gen3ClubErrorOtherRoom);
        ASSERT_EQ(st.partner_rules.game, cases[i].frlg ? Gen3LinkGameFRLG : Gen3LinkGameRS);
        ASSERT_TRUE(guide_says(&st, "The GBA chose another", cases[i].l2, cases[i].l3));
        ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 400));
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        st = club_status(&b);
        ASSERT_EQ(st.last_error, Gen3ClubErrorOtherRoom);
        ASSERT_TRUE(guide_says(
            &st, "Go to the Cable Club:", "PC 2F, Trade Center:", "RS middle, FRLG right"));
    }
}

TEST(test_guide_trade_link_late) {
    /* The trade's 0x1144 link doesn't come within the club's relink wait: the GBA stalls here, as
     * a GBA turned off in the gap would, or one whose own deadline on that link has run out
     * (FRLG: fr: trade_scene.c: CB2_LinkTrade, CheckLinkTimeout), which shows a link error. The
     * trade stays pending (the club waits for the GBA's next LinkPlayerBlock), but no GBA is
     * linked: the guide says the link was lost before the trade, not "Trading...". A late relink
     * then trades, and the guide follows it */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 260));
    fake_ruby_answer(&b.ruby, true);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyTrade, 400));
    fake_ruby_stall(&b.ruby, 255);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 10));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.relinking);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_TRUE(guide_says(&st, "Trading...", "Watch the GBA.", ""));
    for(int i = 0; i <= GEN3_CLUB_RELINK_WAIT_FRAMES && club_status(&b).relinking; i++) {
        if(i % 200 == 0) fake_ruby_stall(&b.ruby, 255);
        step(&b);
    }
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_TRUE(!st.relinking);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_TRUE(!st.left);
    ASSERT_TRUE(
        guide_says(&st, "Link lost before the", "trade. On a GBA link", "error, restart it."));

    /* The late relink: "Trading..." again once linked, then the trade and its link save */
    bool trading_shown = false;
    for(int i = 0; i < 4000 && club_status(&b).trades_done == 0; i++) {
        char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        st = club_status(&b);
        ASSERT_TRUE(gen3_trade_guide(&st, true, true, Gen3RefuseNone, Gen3PickupOk, out));
        if(st.phase != Gen3ClubPhaseWaiting) trading_shown |= strcmp(out[0], "Trading...") == 0;
    }
    st = club_status(&b);
    ASSERT_TRUE(trading_shown);
    ASSERT_EQ(st.trades_done, 1);
    ASSERT_EQ(st.last_result, Gen3TradeResultDone);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
}

TEST(test_guide_through_relinks) {
    /* Every normal relink has a gap of about 20 frames in Waiting. The guide reads it as linked:
     * "Linking...", or "Trading..." for the trade's own relink, never "Go to the Cable Club". Five
     * gaps: the sit, the trade's 0x1144, the post-save 0x1122, the EEBB's 0x1111 back to the room,
     * the re-sit. */
    Bench b;
    GuideWatch w = {0};
    uint8_t z[GEN3_PARTY_SIZE];
    bool answered = false;
    make_z(z);
    bench_init_trade(&b);
    fake_ruby_open_link(&b.ruby);
    GUIDE_UNTIL(
        &b,
        &w,
        club_status(&b).phase == Gen3ClubPhaseRoom && fake_ruby_phase(&b.ruby) == FakeRubyRoom,
        420);
    ASSERT_EQ(w.gaps, 0);

    fake_ruby_sit(&b.ruby, 1);
    GUIDE_UNTIL(&b, &w, fake_ruby_menu_state(&b.ruby) == FakeRubyMenuBrowsing, 600);
    ASSERT_EQ(w.gaps, 1);

    fake_ruby_offer(&b.ruby, 2);
    for(uint32_t i = 0;
        !(club_status(&b).trades_done == 1 &&
          fake_ruby_menu_state(&b.ruby) == FakeRubyMenuBrowsing);
        i++) {
        ASSERT_TRUE(i < 4000);
        if(!answered && fake_ruby_menu_state(&b.ruby) == FakeRubyMenuPrompt) {
            fake_ruby_answer(&b.ruby, true);
            answered = true;
        }
        ASSERT_TRUE(guide_step(&b, &w));
    }
    ASSERT_EQ(w.gaps, 3);

    ASSERT_TRUE(gen3_club_set_offer(&b.club, z, 1));
    GUIDE_UNTIL(&b, &w, fake_ruby_phase(&b.ruby) == FakeRubyReturn, 200);
    GUIDE_UNTIL(
        &b,
        &w,
        club_status(&b).phase == Gen3ClubPhaseRoom && fake_ruby_phase(&b.ruby) == FakeRubyRoom,
        200);
    ASSERT_EQ(w.gaps, 4);

    fake_ruby_resit(&b.ruby);
    GUIDE_UNTIL(&b, &w, fake_ruby_menu_state(&b.ruby) == FakeRubyMenuBrowsing, 800);
    ASSERT_EQ(w.gaps, 5);
    ASSERT_TRUE(w.gap_frames >= 5);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
}

#undef GUIDE_UNTIL

TEST(test_leave_and_gba_exit) {
    /* D9: the GBA's own 0017 and the request make one 0017 between them */
    {
        /* The GBA walks out just after the request: its 0017 comes after the Flipper's */
        Bench b;
        ASSERT_TRUE(reach_room(&b));
        run(&b, 40);
        gen3_club_request_leave(&b.club);
        fake_ruby_leave_room(&b.ruby);
        ASSERT_TRUE(run_until_left(&b, 800));
        ASSERT_EQ(b.sent_cafe17, 1);
        ASSERT_TRUE(b.g_cafe_seen & (1u << 0x17));
        ASSERT_TRUE(stays_left(&b));
    }
    {
        /* The GBA's 0017 is in, its answer due, when the request comes: that answer is the one */
        Bench b;
        ASSERT_TRUE(reach_room(&b));
        fake_ruby_leave_room(&b.ruby);
        for(int i = 0; i < 400 && !(b.g_cafe_seen & (1u << 0x17)); i++) step(&b);
        ASSERT_TRUE(b.g_cafe_seen & (1u << 0x17));
        ASSERT_EQ(b.sent_cafe17, 0);
        gen3_club_request_leave(&b.club);
        ASSERT_TRUE(run_until_left(&b, 400));
        ASSERT_EQ(b.sent_cafe17, 1);
        ASSERT_TRUE(stays_left(&b));
    }
    {
        /* The GBA's 0017 already answered: the request adds nothing, and the exit's close is
         * the end */
        Bench b;
        ASSERT_TRUE(reach_room(&b));
        fake_ruby_leave_room(&b.ruby);
        for(int i = 0; i < 400 && b.sent_cafe17 == 0; i++) step(&b);
        ASSERT_EQ(b.sent_cafe17, 1);
        gen3_club_request_leave(&b.club);
        ASSERT_TRUE(run_until_left(&b, 400));
        ASSERT_EQ(b.sent_cafe17, 1);
        ASSERT_TRUE(stays_left(&b));
    }
}

/* reach_room, then: with gba_leaves, the GBA player walks out at once (fake_ruby_leave_room);
 * with request_at 0 the leave is asked at once, else just before the step that sends burst
 * request_at, together with a CAFE 0011 queued on the club's link (bench_inject_cmd). Runs until
 * left (false on a fake error or after 800 frames), noting the bursts of the Flipper's first
 * 0017 and 0011 and of the GBA's first 0017 (0 for none). */
typedef struct {
    uint32_t f_exit, f_idle, g_exit;
} LeaveBursts;

static bool leave_with_queued(Bench* b, bool gba_leaves, uint32_t request_at, LeaveBursts* out) {
    memset(out, 0, sizeof(*out));
    if(!reach_room(b)) return false;
    if(gba_leaves) fake_ruby_leave_room(&b->ruby);
    if(request_at == 0) gen3_club_request_leave(&b->club);
    for(int i = 0; i < 800 && !club_status(b).left; i++) {
        if(request_at != 0 && b->bursts + 1 == request_at) {
            gen3_club_request_leave(&b->club);
            bench_inject_cmd(b, 0xCAFE, 0x0011);
        }
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        const uint16_t* f = &b->last_tx.word[1];
        const uint16_t* g = &b->rx.word[1];
        if(f[0] == 0xCAFE && f[1] == 0x0017 && !out->f_exit) out->f_exit = b->bursts;
        if(f[0] == 0xCAFE && f[1] == 0x0011 && !out->f_idle) out->f_idle = b->bursts;
        if(g[0] == 0xCAFE && g[1] == 0x0017 && !out->g_exit) out->g_exit = b->bursts;
    }
    return club_status(b).left;
}

TEST(test_leave_queue_busy) {
    /* D9: the 0017 goes out only from an empty queue (leave_step) */
    {
        /* With a command queued in the frame it is due, it goes out exactly one burst later, and
         * only once */
        Bench b;
        LeaveBursts ref;
        LeaveBursts got;
        ASSERT_TRUE(leave_with_queued(&b, false, 0, &ref));
        ASSERT_TRUE(ref.f_exit != 0);
        ASSERT_EQ(ref.f_idle, 0);
        ASSERT_TRUE(leave_with_queued(&b, false, ref.f_exit, &got));
        ASSERT_EQ(got.f_idle, ref.f_exit);
        ASSERT_EQ(got.f_exit, ref.f_exit + 1);
        ASSERT_EQ(b.sent_cafe17, 1);
        ASSERT_TRUE(stays_left(&b));
    }
    {
        /* The case it guards: the GBA's own 0017 comes in that very burst. Waiting, the club sees
         * it first and answers it (the one 0017); a 0017 queued behind the command would follow
         * the GBA's in the next burst, which the GBA drops (sub_8054FF8): it would wait forever */
        Bench b;
        LeaveBursts got;
        uint32_t g_exit = 0;
        ASSERT_TRUE(reach_room(&b)); /* the same run with no request: when the GBA sends 0017 */
        fake_ruby_leave_room(&b.ruby);
        for(int i = 0; i < 400 && !g_exit; i++) {
            step(&b);
            if(b.rx.word[1] == 0xCAFE && b.rx.word[2] == 0x0017) g_exit = b.bursts;
        }
        ASSERT_TRUE(g_exit != 0);
        ASSERT_TRUE(leave_with_queued(&b, true, g_exit, &got));
        ASSERT_EQ(got.g_exit, g_exit);
        ASSERT_EQ(got.f_idle, g_exit);
        ASSERT_TRUE(got.f_exit > g_exit + 1); /* the answer, after the GBA's next command */
        ASSERT_EQ(b.sent_cafe17, 1);
        ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
        ASSERT_TRUE(stays_left(&b));
    }
}

TEST(test_leave_during_linkup) {
    /* D9: asked in each Part A linkup phase, the leave waits: the linkup ends, the GBA walks in to
     * the room, then the one 0017 walks it out, with no link error */
    static const Gen3ClubPhase phases[] = {
        Gen3ClubPhaseLinked,
        Gen3ClubPhasePlayers,
        Gen3ClubPhaseCards,
    };
    for(size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); i++) {
        Bench b;
        bench_init_trade(&b);
        fake_ruby_open_link(&b.ruby);
        ASSERT_TRUE(run_until_phase(&b, phases[i], 400));
        gen3_club_request_leave(&b.club);
        Gen3ClubStatus st = club_status(&b);
        ASSERT_TRUE(st.leaving);
        ASSERT_TRUE(!st.left);
        ASSERT_TRUE(run_until_room(&b, 420));
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        ASSERT_EQ(b.sent_cafe17, 0);
        st = club_status(&b);
        ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE_SETUP);
        ASSERT_TRUE(st.leaving);
        ASSERT_TRUE(run_until_closed_then_left(&b, 600));
        ASSERT_EQ(b.sent_cafe17, 1);
        ASSERT_TRUE(b.g_cafe_seen & (1u << 0x17));
        ASSERT_EQ(b.sent_cafe16, 0);
        ASSERT_TRUE(stays_left(&b));
    }
}

TEST(test_club_size) {
    ASSERT_TRUE(sizeof(Gen3Club) <= 2600);
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_trade_sit_relinks_menu);
    RUN(test_trade_waits_for_gba);
    RUN(test_trade_link_test_never_sits);
    RUN(test_trade_no_offer_never_sits);
    RUN(test_trade_stand_up);
    RUN(test_trade_late_sit);
    RUN(test_trade_relink_timeout_trips_fake);
    RUN(test_trade_link_type_mismatch_trips_fake);
    RUN(test_trade_exchange_parties);
    RUN(test_trade_exchange_margins);
    RUN(test_trade_round1_delay_needed);
    RUN(test_trade_round1_margin);
    RUN(test_trade_later_rounds_need_no_delay);
    RUN(test_trade_missing_block_stalls_fake);
    RUN(test_trade_exchange_with_backlog);
    RUN(test_trade_exchange_empty_offer);
    RUN(test_trade_bad_offer_trips_fake);
    RUN(test_trade_bad_item_trips_fake);
    RUN(test_trade_bad_move_trips_fake);
    RUN(test_trade_mail_byte_trips_fake);
    RUN(test_trade_mail_item_trips_fake);
    RUN(test_trade_bad_request_trips_fake);
    RUN(test_trade_seat_step_while_busy);
    RUN(test_trade_extra_request_trips_fake);
    RUN(test_trade_offer_confirm_ccdd);
    RUN(test_trade_gba_says_no);
    RUN(test_trade_gba_cancels);
    RUN(test_trade_refuses_egg);
    RUN(test_trade_refuses_mail);
    RUN(test_trade_refuses_bad_egg);
    RUN(test_trade_only_pokemon_stays);
    RUN(test_trade_no_dddd_to_empty_party);
    RUN(test_trade_dddd_to_empty_party_trips_fake);
    RUN(test_trade_ccdd_rule_trips_fake);
    RUN(test_trade_unasked_dddd_trips_fake);
    RUN(test_trade_ccdd_before_bbbb_trips_fake);
    RUN(test_trade_ccdd_after_ddee_trips_fake);
    RUN(test_trade_eebb_hang_trips_fake);
    RUN(test_trade_ccdd_hang_trips_fake);
    RUN(test_trade_press_r_gets_no_reply);
    RUN(test_club_confirm_while_idle);
    RUN(test_club_ready_past_party);
    RUN(test_trade_round_trip);
    RUN(test_trade_dcba_after_abcd);
    RUN(test_trade_save_window);
    RUN(test_trade_national_dex_off);
    RUN(test_trade_national_dex_on);
    RUN(test_trade_lost_before_sync1);
    RUN(test_trade_lost_in_window);
    RUN(test_trade_lost_after_sync2);
    RUN(test_trade_save_long_stalls);
    RUN(test_trade_early_dcba_trips_fake);
    RUN(test_trade_extra_2ffe_trips_fake);
    RUN(test_club_offer_guard);
    RUN(test_trade_unexpected_1144);
    RUN(test_trade_pending_dropped_by_relink);
    RUN(test_trade_sync2_answer_lost);
    RUN(test_trade_sync2_answer_delivered);
    RUN(test_trade_sync2_answer_partial_burst);
    RUN(test_trade_sync2_answer_checksum_error);
    RUN(test_trade_early_2ffe_trips_fake);
    RUN(test_trade_offer_without_species_bit);
    RUN(test_trade_second_trade);
    RUN(test_trade_sync_skipped_trips_fake);
    RUN(test_trade_close_skipped_trips_fake);
    RUN(test_trade_lost_before_menu_close);
    RUN(test_trade_pending_survives_menu_close);
    RUN(test_trade_sync2_answer_framing_error);
    RUN(test_trade_sync2_answer_still_queued);
    RUN(test_trade_unexpected_1111);
    RUN(test_trade_edit_in_menu_sends_eebb);
    RUN(test_trade_edit_after_dddd_never_ccdd);
    RUN(test_trade_edit_before_reexchange);
    RUN(test_trade_two_round_trips);
    RUN(test_trade_eebb_retry_after_summary);
    RUN(test_trade_checksum_glitch_resits);
    RUN(test_trade_relink_identity_checked);
    RUN(test_trade_empty_offer_eebb);
    RUN(test_trade_editing_sends_eebb);
    RUN(test_trade_editing_blocks_ccdd);
    RUN(test_trade_editing_refused_while_pending);
    RUN(test_trade_r_mash_eebb);
    RUN(test_trade_broken_block_released);
    RUN(test_trade_lagged_block_answered);
    RUN(test_trade_resit_needed_survives_disconnect);
    RUN(test_trade_return_due_consumed);
    RUN(test_leave_from_room);
    RUN(test_leave_from_menu);
    RUN(test_leave_as_trade_starts);
    RUN(test_leave_while_seated);
    RUN(test_leave_never_sits);
    RUN(test_leave_gba_walks_back_in);
    RUN(test_leave_save_outcome_shown);
    RUN(test_leave_waits_for_save);
    RUN(test_leave_left_menu_stale);
    RUN(test_leave_when_waiting);
    RUN(test_leave_in_relink_gap);
    RUN(test_leave_gba_gone);
    RUN(test_status_relinking);
    RUN(test_guide_through_relinks);
    RUN(test_guide_other_room);
    RUN(test_guide_trade_link_late);
    RUN(test_leave_and_gba_exit);
    RUN(test_leave_queue_busy);
    RUN(test_leave_during_linkup);
    RUN(test_club_size);
    TEST_EXIT();
}
