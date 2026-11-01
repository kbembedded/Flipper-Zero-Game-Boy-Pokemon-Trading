#include "test.h"
#include "bulk_file.h"
#include "club_bench.h"
#include "trade_mons.h"
#include <stdbool.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_partner.h>
#include <src/gen3/gen3_text.h>

/* The Flipper (club) and the fake in FRLG mode (docs/gen3/link-protocol-trade.md Part C). Unless a
 * test says otherwise it starts with bench_init_frlg(&b, GEN3_VERSION_FIRERED, false): the fake is
 * FireRed, RED 13579/24680 without the National Dex, holding bench_init's three Pokemon; the
 * Flipper is FLIPPER 22222 (club_cfg_trade) offering CHARMANDER. */

/* FLIPPER, EOS-filled to 8 bytes (the name in a card, and name[0..7] of a LinkPlayer) */
static const uint8_t flipper_name[8] = {0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0xFF};
static const uint16_t link_types[] = {
    GEN3_LINKTYPE_TRADE_SETUP,
    GEN3_LINKTYPE_TRADE_CONNECTING,
    GEN3_LINKTYPE_TRADE_DISCONNECTED,
    GEN3_LINKTYPE_TRADE,
};

/* name (11 bytes) decodes to want, up to its first EOS */
static bool name_is(const uint8_t name[11], const char* want) {
    char s[12];
    gen3_text_decode(name, 11, s, sizeof(s));
    return strcmp(s, want) == 0;
}

/* gen3_club_claim_game for a partner of `version` (link type partner_type), a session of
 * link_type and the National Dex opt-in natdex gives the FRLG claim (§C1, §C2): that version,
 * lp_field_2 0x8000, progress 01 00 01 and the FRLG card with the version byte. */
static bool
    frlg_claim_ok(uint16_t version, uint32_t partner_type, uint32_t link_type, bool natdex) {
    static const uint8_t name[11] = {
        0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0xFF, 0x01, 0x00, 0x01};
    Gen3LinkPlayer partner = bench_frlg_player(version);
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    uint8_t want[GEN3_TRAINER_CARD_BLOCK_SIZE];
    partner.link_type = partner_type;
    memset(&lp, 0xAA, sizeof(lp));
    memset(card, 0xAA, sizeof(card));
    gen3_club_claim_game(&club_cfg_trade, &partner, link_type, natdex, &lp, card);
    gen3_trainer_card_build_frlg(&lp, want);
    const bool ok = lp.version == version && lp.lp_field_2 == 0x8000 && lp.trainer_id == 22222 &&
                    lp.gender == 0 && lp.id == 0 && lp.language == 2 &&
                    lp.link_type == link_type && memcmp(lp.name, name, sizeof(name)) == 0 &&
                    memcmp(card, want, sizeof(card)) == 0 && card[0x38] == (version & 0xFF);
    if(!ok) {
        printf(
            "  claim for 0x%04X (type 0x%04X), link type 0x%04X, natdex %d: version 0x%04X\n",
            version,
            (unsigned)partner_type,
            (unsigned)link_type,
            natdex,
            lp.version);
    }
    return ok;
}

/* The same for partner (NULL allowed) gives the Ruby claim, byte for byte as before FRLG
 * support (test_club_claim_game). */
static bool ruby_claim_ok(const Gen3LinkPlayer* partner, uint32_t link_type, bool natdex) {
    static const uint8_t name[11] = {
        0xC0, 0xC6, 0xC3, 0xCA, 0xCA, 0xBF, 0xCC, 0xFF, 0xFF, 0xFF, 0xFF};
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    uint8_t want[GEN3_TRAINER_CARD_BLOCK_SIZE];
    memset(&lp, 0xAA, sizeof(lp));
    memset(card, 0xAA, sizeof(card));
    gen3_club_claim_game(&club_cfg_trade, partner, link_type, natdex, &lp, card);
    gen3_trainer_card_build(&lp, want);
    const uint16_t field2 = link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED && natdex ? 0x8000 : 0;
    const bool ok = lp.version == 0x4002 && lp.lp_field_2 == field2 && lp.trainer_id == 22222 &&
                    lp.gender == 0 && lp.id == 0 && lp.language == 2 &&
                    lp.link_type == link_type && memcmp(lp.name, name, sizeof(name)) == 0 &&
                    memcmp(card, want, sizeof(card)) == 0;
    if(!ok) {
        printf(
            "  claim for 0x%04X, link type 0x%04X, natdex %d: version 0x%04X\n",
            partner ? partner->version : 0,
            (unsigned)link_type,
            natdex,
            lp.version);
    }
    return ok;
}

TEST(test_frlg_claim_game) {
    static const uint16_t ruby_versions[] = {0x4001, 0x4002, 0x4003, 0x4000};
    ASSERT_EQ(GEN3_LP_VERSION_BASE, 0x4000);
    ASSERT_EQ(GEN3_VERSION_RUBY, 0x4002);
    for(size_t t = 0; t < sizeof(link_types) / sizeof(link_types[0]); t++) {
        for(int natdex = 0; natdex <= 1; natdex++) {
            ASSERT_TRUE(frlg_claim_ok(0x4004, link_types[t], link_types[t], natdex));
            ASSERT_TRUE(frlg_claim_ok(0x4005, link_types[t], link_types[t], natdex));
            ASSERT_TRUE(ruby_claim_ok(NULL, link_types[t], natdex));
            for(size_t v = 0; v < sizeof(ruby_versions) / sizeof(ruby_versions[0]); v++) {
                Gen3LinkPlayer partner = bench_frlg_player(ruby_versions[v]);
                partner.link_type = link_types[t];
                ASSERT_TRUE(ruby_claim_ok(&partner, link_types[t], natdex));
            }
            /* A bad block or a reset leaves the club's partner zeroed: Unknown, so Ruby */
            Gen3LinkPlayer zero;
            memset(&zero, 0, sizeof(zero));
            ASSERT_TRUE(ruby_claim_ok(&zero, link_types[t], natdex));
        }
    }
    /* An FRLG GBA in another room gets the FRLG claim too; the link types differ first */
    ASSERT_TRUE(frlg_claim_ok(0x4004, 0x2233, GEN3_LINKTYPE_TRADE_SETUP, false));
}

TEST(test_frlg_gba_block) {
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, true);
    ASSERT_TRUE(bench_to_room(&b));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.partner_valid);
    ASSERT_EQ(st.partner.version, 0x4004);
    ASSERT_EQ(st.partner.lp_field_2, 0x8000);
    ASSERT_EQ(st.partner.name[8], 0x01);
    ASSERT_EQ(st.partner.name[9], 0x00);
    ASSERT_EQ(st.partner.name[10], 0x00); /* a GBA never writes its own progressFlagsCopy */
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(st.partner_rules.national_dex);
    ASSERT_TRUE(st.trainer_valid);
    ASSERT_TRUE(name_is(st.trainer.name, "RED"));
    ASSERT_EQ(st.trainer.trainer_id, (24680u << 16) | 13579u);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_to_room(&b));
    st = club_status(&b);
    ASSERT_EQ(st.partner.name[8], 0x00);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(!st.partner_rules.national_dex);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_claim_on_wire) {
    Bench b;
    char name[12];
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    /* The Flipper's LinkPlayer as FireRed converted it */
    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->version, 0x4004);
    ASSERT_EQ(p->lp_field_2, 0x8000);
    ASSERT_EQ(p->trainer_id, 22222);
    gen3_text_decode(p->name, sizeof(p->name), name, sizeof(name));
    ASSERT_STR_EQ(name, "FLIPPER");
    ASSERT_EQ(p->name[8], 0x01);
    ASSERT_EQ(p->name[9], 0x00);
    ASSERT_EQ(p->name[10], 0x01);
    /* The FRLG card: Ruby's layout, then the version byte, then zeros (§C2) */
    const uint8_t* card = fake_ruby_partner_card(&b.ruby);
    ASSERT_EQ(card[0x00], 0); /* gender */
    ASSERT_EQ(card[0x01], 0); /* stars */
    ASSERT_EQ(card[0x02], 1); /* hasPokedex */
    ASSERT_EQ(card[0x0E], 0xCE); /* 22222 */
    ASSERT_EQ(card[0x0F], 0x56);
    ASSERT_MEM_EQ(card + 0x30, flipper_name, sizeof(flipper_name));
    ASSERT_EQ(card[0x38], 0x04);
    for(size_t i = 0x39; i < GEN3_TRAINER_CARD_BLOCK_SIZE; i++) ASSERT_EQ(card[i], 0);
}

TEST(test_frlg_leafgreen) {
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_LEAFGREEN, true);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->version, 0x4005);
    ASSERT_EQ(fake_ruby_partner_card(&b.ruby)[0x38], 0x05);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(name_is(st.trainer.name, "LEAF"));
    ASSERT_EQ(st.trainer.gender, 1);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(st.partner_rules.national_dex);
}

/* Bursts from the raw master's Connected to its card request: both LinkPlayerBlocks are in by
 * about 15, and a GBA past its linkup checks has its card ready 3 bursts later */
#define RAW_CARD_REQUEST_BURSTS 40

/* bench_raw_relink with a card request: for `frames` frames a fresh Gen3Link is the master
 * instead of the club. On Connected it queues {2222, 1133} and lp's LinkPlayerBlock, then
 * {CCCC, 2} RAW_CARD_REQUEST_BURSTS later, and never its own card, so the GBA then waits for it.
 * True if the GBA sent its 100-byte card: it got past its linkup checks (a GBA still before them
 * would trip "stale card"). */
static bool raw_linkup(Bench* b, const Gen3LinkPlayer* lp, uint32_t frames) {
    static const uint16_t req[GEN3_CMD_LEN] = {0x2222, GEN3_LINKTYPE_TRADE_SETUP};
    static const uint16_t card_req[GEN3_CMD_LEN] = {0xCCCC, 2};
    uint8_t block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    Gen3Link link;
    uint32_t since = 0;
    bool sent = false;
    gen3_link_player_block_build(lp, block);
    gen3_link_init(&link);
    for(uint32_t i = 0; i < frames; i++) {
        MlinkTx tx;
        uint16_t cmd[GEN3_CMD_LEN];
        /* b->rx answers the club's last tx: a fresh link starts without it */
        if(i > 0 && gen3_link_rx(&link, &b->rx, cmd) == Gen3LinkEventConnected && !sent) {
            gen3_link_send(&link, req);
            gen3_block_send(&link, block, sizeof(block), 0);
            sent = true;
        }
        if(sent && ++since == RAW_CARD_REQUEST_BURSTS) gen3_link_send(&link, card_req);
        gen3_link_tx(&link, &tx);
        fake_ruby_frame(&b->ruby, &tx, &b->rx);
        bench_record(b, &tx);
    }
    for(uint8_t i = 0; i < b->n_g_blocks; i++) {
        if(b->g_blocks[i].size == GEN3_TRAINER_CARD_BLOCK_SIZE) return true;
    }
    return false;
}

TEST(test_frlg_ruby_claim_refused) {
    /* §C1: FRLG refuses a Ruby partner at the counter unless the save may link with R/S */
    Bench b;
    Gen3LinkPlayer ruby;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    gen3_club_claim_game(&club_cfg_trade, NULL, GEN3_LINKTYPE_TRADE_SETUP, false, &ruby, card);
    ASSERT_EQ(ruby.version, GEN3_VERSION_RUBY);

    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    fake_ruby_open_link(&b.ruby);
    bench_raw_relink(&b, &ruby, 200);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "not ready");
    ASSERT_EQ(b.n_g_close, 0); /* a plain CloseLink: no 5FFF */
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);

    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    fake_ruby_set_frlg_progress(&b.ruby, false, true);
    fake_ruby_open_link(&b.ruby);
    ASSERT_TRUE(raw_linkup(&b, &ruby, 200)); /* past the gate: its card went out */
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(b.n_g_close, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyLinkup); /* awaiting the Flipper's card */
    /* An R/S sender's progress bytes are zeroed on receipt (fr: link.c: ProcessRecvCmds) */
    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->version, GEN3_VERSION_RUBY);
    ASSERT_EQ(p->name[8], 0);
    ASSERT_EQ(p->name[9], 0);
    ASSERT_EQ(p->name[10], 0);
}

/* The fake's linkup gate against a raw Flipper LinkPlayer: the Ruby claim with `version` and raw
 * name[8], name[10], against a FireRed save with or without FLAG_SYS_CAN_LINK_WITH_RS. "passed"
 * once the fake is past the gate (raw_linkup: its card went out, and it waits in the linkup for
 * the Flipper's), else the fake's error, or "waiting". */
static const char* gate(uint16_t version, uint8_t name8, uint8_t name10, bool can_link_rs) {
    Bench b;
    Gen3LinkPlayer lp;
    uint8_t card[GEN3_TRAINER_CARD_BLOCK_SIZE];
    gen3_club_claim_game(&club_cfg_trade, NULL, GEN3_LINKTYPE_TRADE_SETUP, false, &lp, card);
    lp.version = version;
    lp.name[8] = name8;
    lp.name[9] = 0;
    lp.name[10] = name10;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    fake_ruby_set_frlg_progress(&b.ruby, false, can_link_rs);
    fake_ruby_open_link(&b.ruby);
    const bool card_sent = raw_linkup(&b, &lp, 200);
    if(fake_ruby_error(&b.ruby)) return fake_ruby_error(&b.ruby);
    return card_sent && fake_ruby_phase(&b.ruby) == FakeRubyLinkup ? "passed" : "waiting";
}

TEST(test_frlg_fake_gate) {
    /* fr: trade.c: GetGameProgressForLinkTrade. An FRLG partner is always ready */
    ASSERT_STR_EQ(gate(0x4004, 0x00, 0x00, false), "passed");
    ASSERT_STR_EQ(gate(0x4005, 0x00, 0x00, false), "passed");
    /* R/S: only this save's own 0xF0 bits count; the partner's bytes are zeroed */
    ASSERT_STR_EQ(gate(0x4002, 0xFF, 0xFF, true), "passed"); /* the Ruby claim as sent */
    ASSERT_STR_EQ(gate(0x4001, 0x10, 0x10, true), "passed");
    ASSERT_STR_EQ(gate(0x4002, 0x10, 0x10, false), "not ready");
    /* Emerald or other: both saves. The partner's copy is the receiver's: name[10] = name[8]
     * (fr: link.c: ConvertLinkPlayerName), whatever was sent in name[10] */
    ASSERT_STR_EQ(gate(0x4003, 0x10, 0x00, true), "passed");
    ASSERT_STR_EQ(gate(0x4000, 0x80, 0x00, true), "passed");
    ASSERT_STR_EQ(gate(0x4003, 0x00, 0x10, true), "not ready");
    ASSERT_STR_EQ(gate(0x4003, 0x01, 0x01, true), "not ready");
    ASSERT_STR_EQ(gate(0x4003, 0x10, 0x10, false), "not ready");
}

/* The club's card for this session, poked before it goes out (white box: the club builds it with
 * its LinkPlayerBlock and sends it card_delay idle bursts later), then the run to the room. The
 * GBA is FireRed (frlg) or Ruby (bench_init_trade). */
static const char* card_poked(bool frlg, uint8_t offset, uint8_t value) {
    Bench b;
    if(frlg) {
        bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    } else {
        bench_init_trade(&b);
    }
    fake_ruby_open_link(&b.ruby);
    if(!run_until_phase(&b, Gen3ClubPhasePlayers, 200)) return "no players";
    b.club.own_card[offset] = value;
    if(run_until_room(&b, 400) && fake_ruby_error(&b.ruby) == NULL) return "room";
    return fake_ruby_error(&b.ruby) ? fake_ruby_error(&b.ruby) : "stuck";
}

TEST(test_frlg_fake_bad_card) {
    /* fr: cable_club.c: Task_LinkupAwaitTrainerCardData copies an FRLG partner's card whole */
    ASSERT_STR_EQ(card_poked(true, 0x38, 0x04), "room");
    ASSERT_STR_EQ(card_poked(true, 0x38, 0x00), "bad card"); /* Ruby's card, FireRed claim */
    ASSERT_STR_EQ(card_poked(true, 0x38, 0x05), "bad card"); /* LeafGreen's card, FireRed claim */
    ASSERT_STR_EQ(card_poked(true, 0x01, 0x04), "room"); /* 4 stars: the last palette */
    ASSERT_STR_EQ(card_poked(true, 0x01, 0x05), "bad card");
}

TEST(test_ruby_fake_bad_card) {
    /* Ruby mode: sub_8083314 keeps the Flipper's card, and its stars index the 5
     * gTrainerCardPalettes and, less one, the 4 gTrainerCardColorNames (§A5) */
    ASSERT_STR_EQ(card_poked(false, 0x01, 0x04), "room");
    ASSERT_STR_EQ(card_poked(false, 0x01, 0x05), "bad card");
    ASSERT_STR_EQ(card_poked(false, 0x01, 0xFF), "bad card");
    /* Ruby's card ends at 0x37: it reads no version byte */
    ASSERT_STR_EQ(card_poked(false, 0x38, 0x05), "room");
}

TEST(test_frlg_rules_from_init) {
    /* {Unknown, true} until the first GBA block: a memset alone would read national_dex false */
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameUnknown);
    ASSERT_TRUE(st.partner_rules.national_dex);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_EQ(club_status(&b).partner_rules.game, Gen3LinkGameFRLG);
    gen3_club_init(&b.club, &club_cfg_trade); /* init forgets the GBA */
    st = club_status(&b);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameUnknown);
    ASSERT_TRUE(st.partner_rules.national_dex);
}

TEST(test_frlg_rules_kept_on_bad_block) {
    /* A LinkPlayerBlock that doesn't parse keeps the rules of the latest one that did */
    Bench b;
    BenchRawGba raw;
    Gen3LinkPlayer flipper;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_TRUE(leave_room(&b));
    ruby_relink(&b);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseLinked, 100));
    /* A raw GBA takes over before the fake's LinkPlayerBlock, and sends a 20-byte block instead */
    raw_gba_init(&raw, &b, 0xAABB, 0);
    raw_gba_run(&raw, &b, 30);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseError);
    ASSERT_EQ(st.last_error, Gen3ClubErrorBadPlayerBlock);
    ASSERT_TRUE(!st.partner_valid);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(!st.partner_rules.national_dex);
    /* The partner is zeroed, an Unknown game: the Flipper's own block is the Ruby claim */
    ASSERT_TRUE(gen3_link_player_block_parse(b.f_rx.data, &flipper));
    ASSERT_EQ(flipper.version, GEN3_VERSION_RUBY);
}

TEST(test_frlg_rules_other_room) {
    /* A block of any link type sets the rules: an FRLG GBA that chose the Colosseum */
    Bench b;
    Gen3LinkPlayer me = bench_frlg_player(GEN3_VERSION_FIRERED);
    me.link_type = 0x2233;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    bench_swap_gba(&b, &me, false);
    fake_ruby_open_link(&b.ruby);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseError, 400));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.last_error, Gen3ClubErrorOtherRoom);
    ASSERT_TRUE(!st.partner_valid);
    ASSERT_EQ(st.link_type, 0x2233);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(!st.partner_rules.national_dex);
    run(&b, 400);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    /* The FRLG claim in a 0x1133 block; the link types differ, so nothing follows from it */
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->version, 0x4004);
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->link_type, GEN3_LINKTYPE_TRADE_SETUP);
}

TEST(test_frlg_room_and_leave) {
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_to_room(&b));
    run(&b, 1800);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyRoom);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_EQ(st.link.checksum_errors, 0);

    ASSERT_TRUE(leave_room(&b));
    run(&b, 120);
    ASSERT_EQ(b.sent_cafe17, 1);
    ASSERT_EQ(b.sent_cafe, 1); /* nothing but that answer: the GBA never sat */
    ASSERT_EQ(b.sent_5fff, 1);
    ASSERT_EQ(b.n_g_close, 1);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_error, Gen3ClubErrorNone);
    /* The partner's rules outlive its link */
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(!st.partner_rules.national_dex);
}

TEST(test_frlg_link_test) {
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    gen3_club_init(&b.club, &club_cfg);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->version, 0x4004);
    ASSERT_EQ(fake_ruby_partner_card(&b.ruby)[0x38], 0x04);
    fake_ruby_sit(&b.ruby, 0);
    run(&b, 600);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_swap_cartridge) {
    /* Ruby, then FireRed, then Ruby again in one session: each claim follows the GBA linked now */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_to_room(&b));
    const uint8_t* card = fake_ruby_partner_card(&b.ruby);
    ASSERT_MEM_EQ(card + 0x30, flipper_name, sizeof(flipper_name)); /* Ruby mode keeps it too */
    ASSERT_EQ(card[0x38], 0);
    ASSERT_TRUE(leave_room(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    const Gen3LinkPlayer fr = bench_frlg_player(GEN3_VERSION_FIRERED);
    bench_swap_gba(&b, &fr, false);
    ASSERT_TRUE(bench_to_room(&b));
    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->version, 0x4004);
    ASSERT_EQ(p->name[8], 0x01);
    ASSERT_EQ(fake_ruby_partner_card(&b.ruby)[0x38], 0x04);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(name_is(st.trainer.name, "RED"));
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameFRLG);
    ASSERT_TRUE(!st.partner_rules.national_dex);
    ASSERT_TRUE(leave_room(&b));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    const Gen3LinkPlayer ruby = bench_ruby_player();
    bench_swap_gba(&b, &ruby, false);
    ASSERT_TRUE(bench_to_room(&b));
    p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->version, 0x4002);
    ASSERT_EQ(p->lp_field_2, 0);
    ASSERT_EQ(p->name[7], 0xFF);
    ASSERT_EQ(p->name[8], 0xFF);
    ASSERT_EQ(p->name[9], 0xFF);
    ASSERT_EQ(p->name[10], 0xFF);
    ASSERT_EQ(fake_ruby_partner_card(&b.ruby)[0x38], 0);
    st = club_status(&b);
    ASSERT_EQ(st.partner_rules.game, Gen3LinkGameRS);
    ASSERT_TRUE(st.partner_rules.national_dex);
    ASSERT_TRUE(name_is(st.trainer.name, "RUBY"));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

/* ---- The trade menu (§B2, §C3, §C4) ---- */

/* The Flipper's Pokemon in these tests: Y, the default offer CHARMANDER; T, TREECKO (#252); M, a
 * Mew without the fateful-encounter bit */
static void make_y(uint8_t out[GEN3_PARTY_SIZE]) {
    tm_make(4, 5, 21, out);
}
static void make_t(uint8_t out[GEN3_PARTY_SIZE]) {
    tm_make(252, 5, 21, out);
}
static void make_m(uint8_t out[GEN3_PARTY_SIZE]) {
    tm_make_unfateful(151, 22, out);
}

/* bench_init's GBA party with slot 1 or 2 replaced (NULL keeps it): {A, B, X} by default */
static void set_gba_party(Bench* b, const uint8_t* slot1, const uint8_t* slot2) {
    uint8_t party[3][GEN3_PARTY_SIZE];
    tm_make(1, 5, 11, party[0]);
    tm_make(4, 5, 12, party[1]);
    tm_make(7, 10, 13, party[2]);
    if(slot1) memcpy(party[1], slot1, GEN3_PARTY_SIZE);
    if(slot2) memcpy(party[2], slot2, GEN3_PARTY_SIZE);
    bench_set_party(b, party, 3);
}

/* The codes of the 20-byte blocks among blocks[0..n) are exactly want[0..count), in order */
static bool codes_are(const BenchBlock* blocks, uint8_t n, const uint16_t* want, size_t count) {
    uint16_t codes[BENCH_BLOCKS];
    const size_t got = bench_codes(blocks, n, codes, BENCH_BLOCKS);
    bool ok = got == count;
    for(size_t i = 0; ok && i < count; i++) ok = codes[i] == want[i];
    if(!ok) {
        printf("  codes:");
        for(size_t i = 0; i < got; i++) printf(" %04X", codes[i]);
        printf("\n");
    }
    return ok;
}

/* The latest 0x1122 session's five margins: margin[k] is the burst of the Flipper's CCCC k
 * (k = 0 for round 1) minus the later last_burst of the two blocks before it (both
 * LinkPlayerBlocks for round 1, both blocks of round k for round k + 1). False if that session's
 * exchange isn't complete. */
static bool exchange_margins(const Bench* b, int64_t margin[GEN3_TRADE_ROUNDS]) {
    const int fk = latest_block(b->f_blocks, b->n_f_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    const int gk = latest_block(b->g_blocks, b->n_g_blocks, GEN3_LINK_PLAYER_BLOCK_SIZE);
    if(fk < 0 || gk < 0 || b->n_f_cccc != GEN3_TRADE_ROUNDS) return false;
    if(b->n_f_blocks < fk + 1 + GEN3_TRADE_ROUNDS || b->n_g_blocks < gk + 1 + GEN3_TRADE_ROUNDS) {
        return false;
    }
    for(int k = 0; k < GEN3_TRADE_ROUNDS; k++) {
        const uint32_t f = b->f_blocks[fk + k].last_burst;
        const uint32_t g = b->g_blocks[gk + k].last_burst;
        margin[k] = (int64_t)b->f_cccc_burst[k] - (int64_t)(f > g ? f : g);
    }
    return true;
}

TEST(test_frlg_exchange) {
    /* §B2.1 against fr: trade.c: BufferTradeParties: five rounds, then FireRed's trade menu with
     * the Flipper's one Pokemon */
    static const uint16_t types[GEN3_TRADE_ROUNDS] = {1, 1, 1, 3, 4};
    Bench b;
    uint8_t y[GEN3_PARTY_SIZE];
    make_y(y);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    const Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseMenu);
    ASSERT_EQ(st.round, 5);
    ASSERT_TRUE(st.offer_usable);
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 1);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), y, GEN3_PARTY_SIZE);
    ASSERT_EQ(fake_ruby_max_enemy_species(&b.ruby), 4);
    ASSERT_EQ(b.n_f_cccc, GEN3_TRADE_ROUNDS);
    for(int r = 0; r < GEN3_TRADE_ROUNDS; r++) ASSERT_EQ(b.f_cccc[r], types[r]);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_exchange_margins) {
    /* §C4: the club mirrors an FRLG master, 16 idle bursts before round 1 and 12 before each
     * later round, against Ruby's 10 and 6 */
    Bench b;
    int64_t margin[GEN3_TRADE_ROUNDS];
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(exchange_margins(&b, margin));
    ASSERT_TRUE(margin[0] >= 17);
    for(int k = 1; k < GEN3_TRADE_ROUNDS; k++) ASSERT_TRUE(margin[k] >= 13);
}

TEST(test_frlg_round1_delay_needed) {
    /* The round-1 rule holds for FRLG too: a CCCC right after the LinkPlayerBlocks reaches
     * FireRed before its case 1, which clears the Flipper's block flag. set_round_delays fixes
     * the delays, so the GBA's 0x1122 block doesn't replace them */
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    gen3_club_set_round_delays(&b.club, 0, GEN3_CLUB_ROUND_DELAY_BURSTS_FRLG);
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 900 && fake_ruby_error(&b.ruby) == NULL; i++) step(&b);
    const char* error = fake_ruby_error(&b.ruby);
    ASSERT_TRUE(error != NULL);
    ASSERT_TRUE(strcmp(error, "stale block") == 0 || strcmp(error, "exchange stalled") == 0);
}

TEST(test_frlg_no_natdex_never_sits) {
    /* A FireRed save without the National Dex, and TREECKO on the Flipper: the club never sits,
     * so FireRed never sees it */
    Bench b;
    uint8_t t[GEN3_PARTY_SIZE];
    make_t(t);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_offer(&b, t));
    ASSERT_TRUE(bench_to_room(&b));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.has_offer);
    ASSERT_EQ(st.offer_traits, Gen3OfferNonKanto);
    ASSERT_TRUE(!st.offer_usable);
    fake_ruby_sit(&b.ruby, 1);
    run(&b, 900);
    ASSERT_EQ(b.sent_cafe16, 0);
    st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(!st.flipper_seated);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_EQ(fake_ruby_max_enemy_species(&b.ruby), 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_natdex_sits) {
    /* With the National Dex, FireRed may see TREECKO */
    Bench b;
    uint8_t t[GEN3_PARTY_SIZE];
    make_t(t);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, true);
    ASSERT_TRUE(bench_offer(&b, t));
    ASSERT_TRUE(bench_reach_menu(&b));
    const Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.offer_traits, Gen3OfferNonKanto);
    ASSERT_TRUE(st.offer_usable);
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 1);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), t, GEN3_PARTY_SIZE);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_unusable_before_round1) {
    /* An edit lands in the frame the Flipper sits: round 1 shows an empty partner column, as with
     * no offer, and the stale offer sends FireRed back to the room */
    static const uint16_t eebb[] = {GEN3_TRADE_BOTH_CANCEL};
    Bench b;
    uint8_t t[GEN3_PARTY_SIZE];
    make_t(t);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_TRUE(club_status(&b).offer_usable); /* CHARMANDER, a Kanto Pokemon */
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 300 && b.sent_cafe16 == 0; i++) step(&b);
    ASSERT_EQ(b.sent_cafe16, 1);
    ASSERT_TRUE(bench_offer(&b, t));
    ASSERT_TRUE(!club_status(&b).offer_usable);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 1200));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    ASSERT_EQ(fake_ruby_max_enemy_species(&b.ruby), 0);
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 0);
    ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, eebb, 1));
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseRoom);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_unusable_in_menu) {
    /* An edit to TREECKO in the menu: EEBB at once, and no seat in the room after */
    static const uint16_t eebb[] = {GEN3_TRADE_BOTH_CANCEL};
    Bench b;
    uint8_t t[GEN3_PARTY_SIZE];
    make_t(t);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    const uint32_t at = b.bursts;
    ASSERT_TRUE(bench_offer(&b, t));
    ASSERT_TRUE(club_status(&b).offer_stale);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 200));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, eebb, 1));
    ASSERT_TRUE(trade_block(b.f_blocks, b.n_f_blocks, 0)->first_burst - at <= 12);
    const uint32_t sat = b.sent_cafe16;
    ASSERT_EQ(sat, 1);
    fake_ruby_resit(&b.ruby);
    run(&b, 600);
    ASSERT_EQ(b.sent_cafe16, sat);
    ASSERT_TRUE(club_status(&b).gba_seated);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_EQ(fake_ruby_max_enemy_species(&b.ruby), 4);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_gba_cant_offer_non_kanto) {
    /* fr: trade.c: CanTradeSelectedMon: without its own National Dex, FireRed can't offer
     * CHIKORITA (#152); it sends nothing */
    Bench b;
    uint8_t chikorita[GEN3_PARTY_SIZE];
    tm_make(152, 5, 14, chikorita);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    set_gba_party(&b, NULL, chikorita);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    run(&b, 60);
    ASSERT_TRUE(codes_are(b.g_blocks, b.n_g_blocks, NULL, 0));
    ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, NULL, 0));
    ASSERT_EQ(fake_ruby_cant_trade(&b.ruby), FakeRubyCantTradeNational);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuMessage);
    fake_ruby_press_a(&b.ruby);
    run(&b, 5);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_gba_offers_non_kanto) {
    /* With its own National Dex FireRed offers CHIKORITA: the Flipper's progressFlags 0x01
     * (LinkPlayerBlock 0x20) say it has the National Dex too */
    static const uint16_t aabb[] = {GEN3_TRADE_READY};
    static const uint16_t dddd[] = {GEN3_TRADE_SET_MONS};
    Bench b;
    uint8_t chikorita[GEN3_PARTY_SIZE];
    tm_make(152, 5, 14, chikorita);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, true);
    set_gba_party(&b, NULL, chikorita);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 300));
    ASSERT_EQ(fake_ruby_cant_trade(&b.ruby), FakeRubyCantTradeNone);
    ASSERT_TRUE(codes_are(b.g_blocks, b.n_g_blocks, aabb, 1));
    ASSERT_EQ(trade_block(b.g_blocks, b.n_g_blocks, 0)->arg, 2);
    ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, dddd, 1));
    ASSERT_EQ(trade_block(b.f_blocks, b.n_f_blocks, 0)->arg, 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_gba_egg) {
    /* An egg: with its own National Dex FireRed offers it and the club refuses it (DDEE);
     * without, it is species 412 to FireRed, above 151, and FireRed sends nothing */
    static const uint16_t aabb[] = {GEN3_TRADE_READY};
    static const uint16_t ddee[] = {GEN3_TRADE_PLAYER_CANCEL};
    uint8_t egg[GEN3_PARTY_SIZE];
    tm_make_egg(egg);
    {
        Bench b;
        bench_init_frlg(&b, GEN3_VERSION_FIRERED, true);
        set_gba_party(&b, egg, NULL);
        ASSERT_TRUE(bench_reach_menu(&b));
        fake_ruby_offer(&b.ruby, 1);
        ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 200));
        ASSERT_EQ(fake_ruby_cant_trade(&b.ruby), FakeRubyCantTradeNone);
        ASSERT_TRUE(codes_are(b.g_blocks, b.n_g_blocks, aabb, 1));
        ASSERT_EQ(trade_block(b.g_blocks, b.n_g_blocks, 0)->arg, 1);
        ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, ddee, 1));
        ASSERT_EQ(club_status(&b).refused, Gen3RefuseEgg);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
    {
        Bench b;
        bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
        set_gba_party(&b, egg, NULL);
        ASSERT_TRUE(bench_reach_menu(&b));
        fake_ruby_offer(&b.ruby, 1);
        run(&b, 60);
        ASSERT_EQ(fake_ruby_cant_trade(&b.ruby), FakeRubyCantTradeNational);
        ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuMessage);
        ASSERT_TRUE(codes_are(b.g_blocks, b.n_g_blocks, NULL, 0));
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
}

TEST(test_frlg_unfateful_mew_refused) {
    /* §C3: FireRed refuses a Mew without the fateful-encounter bit, so the club never sits */
    Bench b;
    uint8_t m[GEN3_PARTY_SIZE];
    make_m(m);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, true);
    ASSERT_TRUE(bench_offer(&b, m));
    ASSERT_TRUE(bench_to_room(&b));
    const Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.has_offer);
    ASSERT_EQ(st.offer_traits, Gen3OfferUnfateful);
    ASSERT_TRUE(!st.offer_usable);
    fake_ruby_sit(&b.ruby, 1);
    run(&b, 900);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_unfateful_mew_bypass) {
    /* The club's rule bypassed on purpose (traits that lie), to pin FireRed's own: on YES it
     * finds the Flipper's Mew unfateful (fr: trade.c: CheckValidityOfTradeMons), sends no BBBB
     * but BBCC 180 frames later, and the club's DDEE cancels the trade */
    static const uint16_t g_codes[] = {GEN3_TRADE_READY, GEN3_TRADE_CONFIRM_NO};
    static const uint16_t f_codes[] = {GEN3_TRADE_SET_MONS, GEN3_TRADE_PLAYER_CANCEL};
    Bench b;
    uint8_t m[GEN3_PARTY_SIZE];
    make_m(m);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, true);
    ASSERT_TRUE(gen3_club_set_offer_traits(&b.club, m, 0, 0));
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), m, GEN3_PARTY_SIZE);
    fake_ruby_offer(&b.ruby, 2);
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuPrompt, 300));
    fake_ruby_answer(&b.ruby, true);
    for(int i = 0; i < 10 && fake_ruby_menu_state(&b.ruby) == FakeRubyMenuPrompt; i++) step(&b);
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuStandby);
    const uint32_t answered = b.bursts;
    ASSERT_TRUE(fake_ruby_partner_mon_invalid(&b.ruby));
    ASSERT_TRUE(run_until_menu_state(&b, FakeRubyMenuMessage, 400));
    run(&b, 60);
    ASSERT_TRUE(codes_are(b.g_blocks, b.n_g_blocks, g_codes, 2));
    const uint32_t bbcc = trade_block(b.g_blocks, b.n_g_blocks, 1)->first_burst;
    ASSERT_TRUE(bbcc - answered >= 180 && bbcc - answered <= 190);
    ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, f_codes, 2));
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuMessage);
    const Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_EQ(st.menu, Gen3ClubMenuIdle);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_swap_delays) {
    /* Ruby, then FireRed, then Ruby again in one session: the exchange delays follow the GBA
     * linked now */
    Bench b;
    int64_t margin[GEN3_TRADE_ROUNDS];
    uint8_t y[GEN3_PARTY_SIZE];
    make_y(y);
    bench_init_trade(&b);
    ASSERT_TRUE(bench_offer(&b, y));
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(exchange_margins(&b, margin));
    ASSERT_TRUE(margin[0] >= 11 && margin[0] <= 16);
    fake_ruby_power_off(&b.ruby);
    run(&b, 120);
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseWaiting);

    const Gen3LinkPlayer fr = bench_frlg_player(GEN3_VERSION_FIRERED);
    bench_swap_gba(&b, &fr, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(exchange_margins(&b, margin));
    ASSERT_TRUE(margin[0] >= 17);
    for(int k = 1; k < GEN3_TRADE_ROUNDS; k++) ASSERT_TRUE(margin[k] >= 13);
    fake_ruby_power_off(&b.ruby);
    run(&b, 120);
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseWaiting);

    const Gen3LinkPlayer ruby = bench_ruby_player();
    bench_swap_gba(&b, &ruby, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(exchange_margins(&b, margin));
    ASSERT_TRUE(margin[0] >= 11 && margin[0] <= 16);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_late_sit) {
    /* The late sit with FRLG: FireRed waits on its seat with no timeout (fr: cable_club.c:
     * Task_EnterCableClubSeat case 2), and the club sits once the offer is usable */
    Bench b;
    uint8_t t[GEN3_PARTY_SIZE];
    uint8_t y[GEN3_PARTY_SIZE];
    make_t(t);
    make_y(y);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_offer(&b, t));
    ASSERT_TRUE(bench_to_room(&b));
    fake_ruby_sit(&b.ruby, 1);
    run(&b, 300);
    ASSERT_EQ(b.sent_cafe16, 0);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.gba_seated);
    ASSERT_TRUE(!st.offer_usable);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubySeated);

    ASSERT_TRUE(bench_offer(&b, y));
    for(int i = 0; i < 200 && b.sent_cafe16 == 0; i++) step(&b);
    ASSERT_EQ(b.sent_cafe16, 1);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyMenu, 200));
    ASSERT_TRUE(run_until_browsing(&b, 600));
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), y, GEN3_PARTY_SIZE);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_set_offer_unknown_traits) {
    /* gen3_club_set_offer decrypts nothing: its offer's traits are unknown, which FRLG refuses
     * even with the National Dex, and Ruby never does */
    Bench b;
    uint8_t y[GEN3_PARTY_SIZE];
    make_y(y);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, true);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, y, 0));
    ASSERT_TRUE(bench_to_room(&b));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.offer_traits, GEN3_OFFER_TRAITS_UNKNOWN);
    ASSERT_TRUE(!st.offer_usable);
    fake_ruby_sit(&b.ruby, 1);
    run(&b, 600);
    ASSERT_EQ(b.sent_cafe16, 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);

    bench_init_trade(&b);
    ASSERT_TRUE(gen3_club_set_offer(&b.club, y, 0));
    ASSERT_TRUE(bench_to_room(&b));
    st = club_status(&b);
    ASSERT_EQ(st.offer_traits, GEN3_OFFER_TRAITS_UNKNOWN);
    ASSERT_TRUE(st.offer_usable);
}

/* ---- The trade, the link save, the relinks and leaving (§B4-§B7, §C4, §C6) ---- */

/* X, the GBA's slot 2 (bench_init's tm_make(7, 10, 13)); P, PIKACHU, the Flipper's next offer */
static void make_x(uint8_t out[GEN3_PARTY_SIZE]) {
    tm_make(7, 10, 13, out);
}
static void make_p(uint8_t out[GEN3_PARTY_SIZE]) {
    tm_make(25, 12, 31, out);
}

/* mon (game form) is tm_make(national, level, seed) as the GBA holds it after TradeMons:
 * Friendship 70, checksum refixed, every other byte as offered (§B4.4, §C4) */
static bool traded_is(const uint8_t* mon, uint16_t national, uint8_t level, uint32_t seed) {
    Gen3Pkm want;
    Gen3Pkm have;
    tm_make_pkm(national, level, seed, &want);
    gen3_pkm_set(&want, Gen3FieldFriendship, 70);
    gen3_pkm_fix_checksum(&want);
    return gen3_pkm_decrypt(mon, GEN3_PARTY_SIZE, &have) &&
           memcmp(have.raw, want.raw, GEN3_PARTY_SIZE) == 0;
}

/* The GBA's 5FFFs, word 1 of each (fr: link.c: BuildSendCmd, gReadyCloseLinkType), are want */
static bool close_types_are(const Bench* b, const uint16_t* want, uint8_t count) {
    bool ok = b->n_g_close == count;
    for(uint8_t i = 0; ok && i < count; i++) ok = b->g_close_type[i] == want[i];
    if(!ok) {
        printf("  GBA 5FFF types:");
        for(uint8_t i = 0; i < b->n_g_close; i++) printf(" %u", b->g_close_type[i]);
        printf("\n");
    }
    return ok;
}

/* One step; counts a Flipper 5FFF burst whose word 2 (the 5FFF's word 1) isn't 0 */
static void step_watch_5fff(Bench* b, uint32_t* nonzero) {
    step(b);
    const MlinkTx* tx = &b->last_tx;
    if(tx->count == MLINK_BURST_LEN && tx->word[1] == 0x5FFF && tx->word[2] != 0) (*nonzero)++;
}

TEST(test_frlg_round_trip) {
    /* §B4-§B6.1 with FireRed (§C4, §C6): it trades its slot 2 (X) for the Flipper's Y and saves;
     * the club finalizes at sync 3, and the exchange after the save shows X as the Flipper's */
    static const uint16_t f_codes[] = {GEN3_TRADE_SET_MONS, GEN3_TRADE_START, GEN3_TRADE_ANIM_ACK};
    Bench b;
    uint8_t x[GEN3_PARTY_SIZE];
    uint8_t got[GEN3_PARTY_SIZE];
    uint32_t done = 0;
    make_x(x);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_trade(&b, 2));

    /* FireRed's save: Y with Friendship 70 in slot 2, the rest as it was */
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_TRUE(!fake_ruby_save_corrupt(&b.ruby));
    ASSERT_TRUE(traded_is(fake_ruby_party(&b.ruby, 2, true), 4, 5, 21));
    for(uint8_t i = 0; i < 2; i++) {
        ASSERT_MEM_EQ(fake_ruby_party(&b.ruby, i, true), b.party[i], GEN3_PARTY_SIZE);
    }

    /* The club: done, X received and shown in the exchange after the save (§B6.1) */
    const Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseMenu);
    ASSERT_EQ(st.trades_done, 1);
    ASSERT_EQ(st.last_result, Gen3TradeResultDone);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_TRUE(!st.save_window);
    gen3_club_get_received(&b.club, got, &done);
    ASSERT_EQ(done, 1);
    ASSERT_MEM_EQ(got, x, GEN3_PARTY_SIZE);
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 1);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), x, GEN3_PARTY_SIZE);

    /* On the wire: the Flipper's trade blocks, and one 2FFE per GBA 2FFE, after it (§B5.3) */
    ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, f_codes, 3));
    const BenchBlock* dcba = trade_block(b.f_blocks, b.n_f_blocks, 2);
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
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_close_types) {
    /* §C4: FireRed's 5FFF carries gReadyCloseLinkType in word 1: 0 for the seat's close
     * (fr: cable_club.c: Task_StartWiredTrade), 32 for the trade's (fr: trade.c:
     * CB_WaitToStartTrade), 0 for the link save's (fr: trade_scene.c: CB2_SaveAndEndTrade). The
     * club answers each with {5FFF, 0} */
    static const uint16_t types[] = {0, 32, 0};
    Bench b;
    uint32_t nonzero = 0;
    bool answered = false;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_to_room(&b));
    ASSERT_EQ(b.n_g_close, 0);
    fake_ruby_sit(&b.ruby, 1);
    for(int i = 0; i < 600 && fake_ruby_menu_state(&b.ruby) != FakeRubyMenuBrowsing; i++) {
        step_watch_5fff(&b, &nonzero);
    }
    ASSERT_EQ(fake_ruby_menu_state(&b.ruby), FakeRubyMenuBrowsing);
    fake_ruby_offer(&b.ruby, 2);
    for(int i = 0; i < 4000; i++) {
        step_watch_5fff(&b, &nonzero);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        const FakeRubyMenuState menu = fake_ruby_menu_state(&b.ruby);
        if(!answered && menu == FakeRubyMenuPrompt) {
            fake_ruby_answer(&b.ruby, true);
            answered = true;
        }
        if(club_status(&b).trades_done == 1 && menu == FakeRubyMenuBrowsing) break;
    }
    ASSERT_EQ(club_status(&b).trades_done, 1);
    ASSERT_TRUE(close_types_are(&b, types, 3));
    ASSERT_EQ(b.sent_5fff, 3);
    ASSERT_EQ(nonzero, 0);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_save_window) {
    /* §C6: FireRed's link save has Ruby's three syncs; save_window from the club's sync-1 answer
     * to sync 3, and the trade counts at sync 3 */
    Bench b;
    bool answered = false;
    bool saw[GEN3_CLUB_SAVE_SYNCS + 1] = {false};
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
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

TEST(test_frlg_trade_relink_in_time) {
    /* §C4: FireRed gives the 0x1144 relink 300 frames in CB2_LinkTrade's state 4. The club
     * answers it well within that; its gap counts as linked (relinking) */
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_trade_phase(&b, 600));
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseWaiting, 10));
    Gen3ClubStatus st = club_status(&b);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_TRUE(st.relinking);
    ASSERT_TRUE(run_until_phase(&b, Gen3ClubPhaseTrade, 200)); /* the GBA's 0x1144 block is in */
    ASSERT_TRUE(!club_status(&b).relinking);
    for(int i = 0; i < 60 && fake_ruby_relink_frames(&b.ruby) == UINT32_MAX; i++) step(&b);
    ASSERT_EQ(fake_ruby_partner(&b.ruby)->link_type, GEN3_LINKTYPE_TRADE_DISCONNECTED);
    ASSERT_TRUE(fake_ruby_relink_frames(&b.ruby) < 100);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_trade_relink_timeout) {
    /* §C4: a 0x1144 relink that doesn't come: FireRed's CheckLinkTimeout (fr: trade_scene.c:
     * CB2_LinkTrade) closes the link and shows a link error. OpenLink 2 frames after its close,
     * 61 frames in state 2, one in state 3, then 301 in state 4. Nothing was written: the club's
     * pending trade waits for the GBA's next block, which cancels it (plan 3 D3) */
    Bench b;
    uint32_t frames = 0;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_trade_phase(&b, 600));
    bench_mute(&b, true);
    while(fake_ruby_error(&b.ruby) == NULL && frames < 1000) {
        step(&b);
        frames++;
    }
    ASSERT_TRUE(fake_ruby_error(&b.ruby) != NULL);
    ASSERT_STR_EQ(fake_ruby_error(&b.ruby), "link timeout");
    ASSERT_TRUE(frames >= 355 && frames <= 375);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
    ASSERT_TRUE(!fake_ruby_save_corrupt(&b.ruby));
    bench_mute(&b, false);
    run(&b, 60);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.trades_done, 0);
    ASSERT_TRUE(st.trade_pending);
    ASSERT_EQ(st.last_result, Gen3TradeResultNone);
    /* The club's own relink wait runs out, then the guide says the link was lost */
    for(int i = 0; i <= GEN3_CLUB_RELINK_WAIT_FRAMES && club_status(&b).relinking; i++) step(&b);
    st = club_status(&b);
    ASSERT_TRUE(
        guide_says(&st, "Link lost before the", "trade. On a GBA link", "error, restart it."));

    ruby_relink(&b); /* the player switches FireRed off and on, and goes back to the club */
    ASSERT_TRUE(bench_to_room(&b));
    st = club_status(&b);
    ASSERT_EQ(st.last_result, Gen3TradeResultCanceled);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_EQ(st.trades_done, 0);
    ASSERT_TRUE(saved_party_untouched(&b));
}

TEST(test_frlg_trade_relink_late_in_time) {
    /* The deadline's other side (fr: trade_scene.c: CB2_LinkTrade): after its OpenLink, FireRed
     * runs 61 frames in state 2 and one in state 3, then trips 301 frames into state 4, 363
     * frames after the OpenLink. A club that stalls 250 frames from FireRed's close, or 330 (its
     * LinkPlayer then comes more than 300 frames after the OpenLink, which only a deadline
     * counted from state 4 lets through), then answers the 0x1144 relink, is still in time: the
     * trade saves */
    static const uint32_t stalls[] = {250, 330};
    for(unsigned k = 0; k < sizeof(stalls) / sizeof(stalls[0]); k++) {
        Bench b;
        bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
        ASSERT_TRUE(bench_reach_menu(&b));
        ASSERT_TRUE(run_until_trade_phase(&b, 600));
        bench_mute(&b, true);
        run(&b, stalls[k]);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        ASSERT_EQ(fake_ruby_relink_frames(&b.ruby), UINT32_MAX);
        bench_mute(&b, false);
        for(int i = 0; i < 60 && fake_ruby_relink_frames(&b.ruby) == UINT32_MAX; i++) {
            step(&b);
            ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        }
        const uint32_t relink = fake_ruby_relink_frames(&b.ruby);
        ASSERT_TRUE(relink > stalls[k] && relink < 363);
        if(stalls[k] > 300) ASSERT_TRUE(relink > 300);
        ASSERT_EQ(fake_ruby_partner(&b.ruby)->link_type, GEN3_LINKTYPE_TRADE_DISCONNECTED);
        for(int i = 0; i < 4000 && club_status(&b).trades_done == 0; i++) {
            step(&b);
            ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        }
        const Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.trades_done, 1);
        ASSERT_EQ(st.last_result, Gen3TradeResultDone);
        ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
        ASSERT_TRUE(!fake_ruby_save_corrupt(&b.ruby));
    }
}

TEST(test_ruby_trade_relink_no_timeout) {
    /* §B4.1: Ruby's CB2_LinkTrade has no deadline in state 4: it waits on a black screen */
    Bench b;
    bench_init_trade(&b);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_trade_phase(&b, 600));
    bench_mute(&b, true);
    run(&b, 1200);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyTrade);
    ASSERT_EQ(fake_ruby_relink_frames(&b.ruby), UINT32_MAX);
}

TEST(test_frlg_national_dex_ignored) {
    /* §C1, §C4: the FRLG claim carries lp_field_2 0x8000 in every block, whatever the opt-in, and
     * FireRed's TradeMons never reads it (fr: trade_scene.c: TryEnableNationalDexFromLinkPartner
     * is commented out): its save gets no National Dex */
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    gen3_club_set_national_dex(&b.club, true);
    ASSERT_TRUE(bench_to_room(&b));
    const Gen3LinkPlayer* p = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(p->link_type, GEN3_LINKTYPE_TRADE_SETUP);
    ASSERT_EQ(p->lp_field_2, 0x8000);
    fake_ruby_sit(&b.ruby, 1);
    ASSERT_TRUE(run_until_browsing(&b, 600));
    ASSERT_EQ(p->link_type, GEN3_LINKTYPE_TRADE_CONNECTING);
    ASSERT_EQ(p->lp_field_2, 0x8000);
    ASSERT_TRUE(run_until_trade_link(&b, 600));
    ASSERT_EQ(p->lp_field_2, 0x8000);
    for(int i = 0; i < 4000 && club_status(&b).trades_done == 0; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_TRUE(!fake_ruby_national_dex(&b.ruby));
}

TEST(test_frlg_lost_in_window) {
    /* §C6, plan 3 D5: the Flipper stops bursting after its sync-1 answer; FireRed's save is
     * half-written, so it reports a corrupt save and loads the pre-trade one: RolledBack */
    Bench b;
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
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
    const Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
    ASSERT_EQ(st.last_result, Gen3TradeResultRolledBack);
    ASSERT_TRUE(!st.trade_pending);
    ASSERT_TRUE(!st.save_window);
    ASSERT_EQ(st.trades_done, 0);
    ASSERT_TRUE(saved_party_untouched(&b));
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
}

TEST(test_frlg_lost_after_sync2) {
    /* §C6, plan 3 D5: FireRed is switched off after the club's sync-2 answer, which a whole clean
     * burst carried: it has committed, so the Flipper takes its Pokemon (Assumed) */
    Bench b;
    uint8_t x[GEN3_PARTY_SIZE];
    uint8_t got[GEN3_PARTY_SIZE];
    uint32_t done = 0;
    make_x(x);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(run_until_save_syncs(&b, 2, 1000));
    run(&b, 4);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_TRUE(club_status(&b).save_window);
    fake_ruby_power_off(&b.ruby);
    run(&b, 30);
    const Gen3ClubStatus st = club_status(&b);
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

TEST(test_frlg_unusable_after_trade) {
    /* The common FireRed save without the National Dex: TREECKO becomes the offer in the frame
     * the trade is done, before the exchange after the save. That exchange's round 1 shows an
     * empty partner column, and the stale offer sends FireRed back to the room: it never sees
     * TREECKO, and no DDDD goes out after the trade */
    static const uint16_t f_codes[] = {
        GEN3_TRADE_SET_MONS, GEN3_TRADE_START, GEN3_TRADE_ANIM_ACK, GEN3_TRADE_BOTH_CANCEL};
    static const uint16_t g_codes[] = {GEN3_TRADE_READY, GEN3_TRADE_CONFIRM, GEN3_TRADE_ANIM_DONE};
    Bench b;
    uint8_t t[GEN3_PARTY_SIZE];
    bool answered = false;
    bool set = false;
    make_t(t);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    fake_ruby_offer(&b.ruby, 2);
    for(int i = 0; i < 4000 && !set; i++) {
        step(&b);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
        if(!answered && fake_ruby_menu_state(&b.ruby) == FakeRubyMenuPrompt) {
            fake_ruby_answer(&b.ruby, true);
            answered = true;
        }
        if(club_status(&b).trades_done == 1) {
            ASSERT_TRUE(bench_offer(&b, t)); /* the frame of sync 3 */
            set = true;
        }
    }
    ASSERT_TRUE(set);
    const Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseSave);
    ASSERT_TRUE(st.has_offer);
    ASSERT_TRUE(!st.offer_usable);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 1500));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 0);
    ASSERT_EQ(fake_ruby_max_enemy_species(&b.ruby), 4); /* Y, from the first exchange only */
    ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, f_codes, 4));
    ASSERT_TRUE(codes_are(b.g_blocks, b.n_g_blocks, g_codes, 3));
    ASSERT_EQ(club_status(&b).phase, Gen3ClubPhaseRoom);
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_edit_resit) {
    /* §B7 with FireRed: after a trade the offer changes to PIKACHU. One unsolicited EEBB sends
     * FireRed back to the room (its 5FFF type 12, fr: trade.c: CB_InitExitCanceledTrade); its
     * 0x1111 re-establish checks the Flipper against the linkup's copy (fr: link.c:
     * CheckLinkPlayersMatchSaved); it steps off its seat and back on, sees PIKACHU and trades
     * for it */
    static const uint16_t f_codes[] = {
        GEN3_TRADE_SET_MONS, GEN3_TRADE_START, GEN3_TRADE_ANIM_ACK, GEN3_TRADE_BOTH_CANCEL};
    Bench b;
    uint8_t p[GEN3_PARTY_SIZE];
    char name[16];
    make_p(p);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_TRUE(bench_offer(&b, p));
    ASSERT_TRUE(club_status(&b).offer_stale);
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 200));
    ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
    ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, f_codes, 4));
    ASSERT_TRUE(b.n_g_close > 0);
    ASSERT_EQ(b.g_close_type[b.n_g_close - 1], 12);
    Gen3ClubStatus st = club_status(&b);
    ASSERT_EQ(st.phase, Gen3ClubPhaseRoom);
    ASSERT_EQ(st.link_type, GEN3_LINKTYPE_TRADE);
    ASSERT_TRUE(st.resit_needed);
    const Gen3LinkPlayer* partner = fake_ruby_partner(&b.ruby);
    ASSERT_EQ(partner->link_type, GEN3_LINKTYPE_TRADE);
    ASSERT_EQ(partner->trainer_id, 22222);
    ASSERT_EQ(partner->version, 0x4004);
    gen3_text_decode(partner->name, sizeof(partner->name), name, sizeof(name));
    ASSERT_STR_EQ(name, "FLIPPER");

    fake_ruby_resit(&b.ruby);
    ASSERT_TRUE(run_until_browsing(&b, 800));
    ASSERT_EQ(fake_ruby_enemy_count(&b.ruby), 1);
    ASSERT_MEM_EQ(fake_ruby_enemy(&b.ruby, 0), p, GEN3_PARTY_SIZE);
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 2);
    ASSERT_TRUE(traded_is(fake_ruby_party(&b.ruby, 2, true), 25, 12, 31));
    st = club_status(&b);
    ASSERT_EQ(st.trades_done, 2);
    ASSERT_EQ(st.last_result, Gen3TradeResultDone);
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_natdex_trade_non_kanto) {
    /* With its own National Dex FireRed takes TREECKO (internal species 277) and saves it */
    Bench b;
    uint8_t t[GEN3_PARTY_SIZE];
    Gen3Pkm saved;
    make_t(t);
    bench_init_frlg(&b, GEN3_VERSION_FIRERED, true);
    ASSERT_TRUE(bench_offer(&b, t));
    ASSERT_TRUE(bench_reach_menu(&b));
    ASSERT_TRUE(bench_trade(&b, 2));
    ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 1);
    ASSERT_TRUE(gen3_pkm_decrypt(fake_ruby_party(&b.ruby, 2, true), GEN3_PARTY_SIZE, &saved));
    ASSERT_EQ(gen3_pkm_get(&saved, Gen3FieldSpecies), 277);
    ASSERT_TRUE(traded_is(fake_ruby_party(&b.ruby, 2, true), 252, 5, 21));
    ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
}

TEST(test_frlg_leave) {
    /* §B7, §C5 with FireRed: End trade in the room is one CAFE 0017, and FireRed walks out and
     * closes the link; from the trade menu, EEBB (5FFF type 12) and the 0x1111 room first */
    static const uint16_t eebb[] = {GEN3_TRADE_BOTH_CANCEL};
    {
        static const uint16_t types[] = {0};
        Bench b;
        bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
        ASSERT_TRUE(bench_to_room(&b));
        gen3_club_request_leave(&b.club);
        ASSERT_TRUE(club_status(&b).leaving);
        ASSERT_TRUE(run_until_left(&b, 600));
        ASSERT_EQ(b.sent_cafe17, 1);
        ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
        ASSERT_TRUE(close_types_are(&b, types, 1));
        run(&b, 200); /* FireRed stays out: no relink, and no further 0017 or 5FFF */
        const Gen3ClubStatus st = club_status(&b);
        ASSERT_EQ(st.phase, Gen3ClubPhaseWaiting);
        ASSERT_TRUE(st.left);
        ASSERT_TRUE(!st.leaving);
        ASSERT_TRUE(!st.relinking);
        ASSERT_EQ(b.sent_cafe17, 1);
        ASSERT_EQ(b.sent_5fff, 1);
        ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
    {
        static const uint16_t types[] = {0, 12, 0};
        Bench b;
        bench_init_frlg(&b, GEN3_VERSION_FIRERED, false);
        ASSERT_TRUE(bench_reach_menu(&b));
        gen3_club_request_leave(&b.club);
        ASSERT_TRUE(club_status(&b).offer_stale);
        ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyReturn, 200));
        ASSERT_TRUE(run_until_fake_phase(&b, FakeRubyRoom, 200));
        ASSERT_EQ(b.sent_cafe17, 0); /* not before FireRed's fade-in is over */
        ASSERT_TRUE(codes_are(b.f_blocks, b.n_f_blocks, eebb, 1));
        ASSERT_TRUE(run_until_left(&b, 600));
        ASSERT_EQ(b.sent_cafe17, 1);
        ASSERT_EQ(fake_ruby_phase(&b.ruby), FakeRubyClosed);
        ASSERT_TRUE(close_types_are(&b, types, 3));
        ASSERT_EQ(fake_ruby_saved_trades(&b.ruby), 0);
        ASSERT_TRUE(fake_ruby_error(&b.ruby) == NULL);
    }
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_frlg_claim_game);
    RUN(test_frlg_gba_block);
    RUN(test_frlg_claim_on_wire);
    RUN(test_frlg_leafgreen);
    RUN(test_frlg_ruby_claim_refused);
    RUN(test_frlg_fake_gate);
    RUN(test_frlg_fake_bad_card);
    RUN(test_ruby_fake_bad_card);
    RUN(test_frlg_rules_from_init);
    RUN(test_frlg_rules_kept_on_bad_block);
    RUN(test_frlg_rules_other_room);
    RUN(test_frlg_room_and_leave);
    RUN(test_frlg_link_test);
    RUN(test_frlg_swap_cartridge);
    RUN(test_frlg_exchange);
    RUN(test_frlg_exchange_margins);
    RUN(test_frlg_round1_delay_needed);
    RUN(test_frlg_no_natdex_never_sits);
    RUN(test_frlg_natdex_sits);
    RUN(test_frlg_unusable_before_round1);
    RUN(test_frlg_unusable_in_menu);
    RUN(test_frlg_gba_cant_offer_non_kanto);
    RUN(test_frlg_gba_offers_non_kanto);
    RUN(test_frlg_gba_egg);
    RUN(test_frlg_unfateful_mew_refused);
    RUN(test_frlg_unfateful_mew_bypass);
    RUN(test_frlg_swap_delays);
    RUN(test_frlg_late_sit);
    RUN(test_frlg_set_offer_unknown_traits);
    RUN(test_frlg_round_trip);
    RUN(test_frlg_close_types);
    RUN(test_frlg_save_window);
    RUN(test_frlg_trade_relink_in_time);
    RUN(test_frlg_trade_relink_timeout);
    RUN(test_frlg_trade_relink_late_in_time);
    RUN(test_ruby_trade_relink_no_timeout);
    RUN(test_frlg_national_dex_ignored);
    RUN(test_frlg_lost_in_window);
    RUN(test_frlg_lost_after_sync2);
    RUN(test_frlg_unusable_after_trade);
    RUN(test_frlg_edit_resit);
    RUN(test_frlg_natdex_trade_non_kanto);
    RUN(test_frlg_leave);
    TEST_EXIT();
}
