#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fake_ruby.h"
#include "trade_mons.h"
#include <src/gen3/gen3_block.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_link.h>
#include <src/gen3/gen3_text.h>
#include <src/gen3/gen3_trade_sync.h>

/* Test bench shared by the club and trade tests: the Flipper (club) and fake Ruby wired back to
 * back. Every helper is static inline, so a test file that doesn't use one compiles cleanly. */

static const Gen3ClubConfig club_cfg =
    {.name = "FLIPPER", .tid = 22222, .sid = 0, .gender = 0, .trading = false};
static const Gen3ClubConfig club_cfg_trade =
    {.name = "FLIPPER", .tid = 22222, .sid = 0, .gender = 0, .trading = true};

#define BENCH_BLOCKS 96 /* f_blocks, g_blocks */
#define BENCH_CCCC   32 /* f_cccc, f_cccc_burst */
#define BENCH_SYNCS  16 /* f_2ffe_burst, g_2ffe_burst */
#define BENCH_CLOSES 16 /* g_close_type */

/* One completed block as one side put it on the wire. */
typedef struct {
    uint16_t size, code, arg; /* code, arg: the block's words 0 and 1 */
    uint16_t id; /* the header's word 2: 0x80 + the sender's multiplayer id */
    uint32_t first_burst, last_burst; /* the header's burst, the last chunk's */
} BenchBlock;

/* Flipper (club) and fake Ruby wired back to back. rx is the fake's answer to the last tx.
 * Bursts count from 1. Every log caps its writes at its size: later entries are dropped. */
typedef struct {
    Gen3Club club;
    FakeRuby ruby;
    Gen3LinkPlayer me; /* the fake's LinkPlayer, kept for fake_ruby_init after a reset */
    MlinkRx rx;
    bool started;
    /* Flipper commands by word 0; sent_cafe17 counts CAFE 0017 */
    uint32_t sent_2222, sent_cccc, sent_5fff, sent_2ffe, sent_8888, sent_cafe, sent_cafe17;

    uint32_t bursts; /* frames with tx.count == 9 so far */
    MlinkTx last_tx; /* the Flipper's tx of the latest step */
    Gen3BlockRx f_rx, g_rx; /* the bench's own parse of each side's blocks (tx / rx words 1..8) */
    uint32_t f_rx_first, g_rx_first; /* burst of the header f_rx / g_rx is parsing */
    uint16_t f_rx_id, g_rx_id; /* that header's word 2 */
    /* every completed block of each side, in order */
    BenchBlock f_blocks[BENCH_BLOCKS], g_blocks[BENCH_BLOCKS];
    uint8_t n_f_blocks, n_g_blocks;
    /* The Flipper's CCCC types and bursts since the GBA's latest LinkPlayerBlock completed */
    uint16_t f_cccc[BENCH_CCCC];
    uint32_t f_cccc_burst[BENCH_CCCC];
    uint8_t n_f_cccc;
    uint32_t g_cafe_seen; /* bit k: the GBA sent CAFE k (k < 32) */
    uint32_t sent_cafe16; /* Flipper CAFE 0016 count */
    uint32_t f_cafe16_burst, g_cafe16_burst; /* latest CAFE 0016 burst per side */
    /* every 2FFE of each side, by burst; the GBA's latest 5FFF */
    uint32_t f_2ffe_burst[BENCH_SYNCS], g_2ffe_burst[BENCH_SYNCS];
    uint8_t n_f_2ffe, n_g_2ffe;
    uint32_t g_5fff_burst;
    /* word 1 of every GBA 5FFF, in order (FRLG's gReadyCloseLinkType, fr: link.c: BuildSendCmd) */
    uint16_t g_close_type[BENCH_CLOSES];
    uint8_t n_g_close;
    bool muted; /* bench_mute */
    uint8_t party[FAKE_RUBY_PARTY][GEN3_PARTY_SIZE]; /* the GBA's party as set, game form */
    uint8_t party_count;
    /* An FRLG fake's save progress (its National Dex), applied again at each ruby_relink */
    bool natdex;
} Bench;

/* The GBA's party (game form, slots 0..count-1), for the fake now and after ruby_relink. */
static inline void
    bench_set_party(Bench* b, const uint8_t party[][GEN3_PARTY_SIZE], uint8_t count) {
    memset(b->party, 0, sizeof(b->party));
    for(uint8_t i = 0; i < count && i < FAKE_RUBY_PARTY; i++) {
        memcpy(b->party[i], party[i], GEN3_PARTY_SIZE);
    }
    b->party_count = count;
    fake_ruby_set_party(&b->ruby, b->party, count);
}

/* The Ruby GBA of bench_init_cfg: "RUBY", TID 12345, SID 54321, gender 0, link type 0x1133 */
static inline Gen3LinkPlayer bench_ruby_player(void) {
    Gen3LinkPlayer lp;
    memset(&lp, 0, sizeof(lp));
    lp.version = GEN3_VERSION_RUBY;
    lp.trainer_id = (54321u << 16) | 12345u;
    gen3_text_encode("RUBY", lp.name, sizeof(lp.name), 7);
    lp.gender = 0;
    lp.link_type = GEN3_LINKTYPE_TRADE_SETUP;
    lp.language = GEN3_LANGUAGE_ENGLISH;
    return lp;
}

/* An FRLG GBA of that version: TID 13579, SID 24680, link type 0x1133; FireRed is "RED" (gender
 * 0), LeafGreen (0x4005) "LEAF" (gender 1). The fake's FRLG mode sets its lp_field_2 and
 * name[8..10]. */
static inline Gen3LinkPlayer bench_frlg_player(uint16_t version) {
    const bool leaf = version == GEN3_VERSION_LEAFGREEN;
    Gen3LinkPlayer lp;
    memset(&lp, 0, sizeof(lp));
    lp.version = version;
    lp.trainer_id = (24680u << 16) | 13579u;
    gen3_text_encode(leaf ? "LEAF" : "RED", lp.name, sizeof(lp.name), 7);
    lp.gender = leaf ? 1 : 0;
    lp.link_type = GEN3_LINKTYPE_TRADE_SETUP;
    lp.language = GEN3_LANGUAGE_ENGLISH;
    return lp;
}

/* The GBA holds tm_make(1, 5, 11), tm_make(4, 5, 12) and tm_make(7, 10, 13). The test's main
 * must have called bulk_file_open. */
static inline void bench_init_cfg(Bench* b, uint32_t ruby_link_type, const Gen3ClubConfig* cfg) {
    uint8_t party[3][GEN3_PARTY_SIZE];
    memset(b, 0, sizeof(*b));
    b->me = bench_ruby_player();
    b->me.link_type = ruby_link_type;
    gen3_club_init(&b->club, cfg);
    fake_ruby_init(&b->ruby, &b->me);
    tm_make(1, 5, 11, party[0]);
    tm_make(4, 5, 12, party[1]);
    tm_make(7, 10, 13, party[2]);
    bench_set_party(b, party, 3);
}

/* A Link Test session (trading false). */
static inline void bench_init(Bench* b, uint32_t ruby_link_type) {
    bench_init_cfg(b, ruby_link_type, &club_cfg);
}

/* The Flipper offers mon (game form; NULL clears the offer), as the app does: its traits
 * computed outside the club (gen3_offer_traits; GEN3_OFFER_TRAITS_UNKNOWN if it doesn't
 * decrypt), then gen3_club_set_offer_traits with the club's trades_done. */
static inline bool bench_offer(Bench* b, const uint8_t mon[GEN3_PARTY_SIZE]) {
    Gen3ClubStatus st;
    Gen3Pkm p;
    uint8_t traits = 0;
    gen3_club_status(&b->club, &st);
    if(mon) {
        traits = gen3_pkm_decrypt(mon, GEN3_PARTY_SIZE, &p) ? gen3_offer_traits(&p) :
                                                              GEN3_OFFER_TRAITS_UNKNOWN;
    }
    return gen3_club_set_offer_traits(&b->club, mon, st.trades_done, traits);
}

/* bench_init(b, 0x1133) as a trade session (club_cfg_trade) offering tm_make(252, 5, 21). The
 * test's main must have called bulk_file_open. */
static inline void bench_init_trade(Bench* b) {
    uint8_t mon[GEN3_PARTY_SIZE];
    bench_init_cfg(b, GEN3_LINKTYPE_TRADE_SETUP, &club_cfg_trade);
    tm_make(252, 5, 21, mon);
    bench_offer(b, mon);
}

/* Another cartridge, or another save, in the same session: a fresh fake with LinkPlayer me (its
 * save's progress natdex, without FLAG_SYS_CAN_LINK_WITH_RS, if FRLG) and the bench's party. The
 * club and the bench's logs are untouched, and the link isn't opened. */
static inline void bench_swap_gba(Bench* b, const Gen3LinkPlayer* me, bool natdex) {
    b->me = *me;
    b->natdex = natdex;
    fake_ruby_init(&b->ruby, &b->me);
    if(fake_ruby_is_frlg(&b->ruby)) fake_ruby_set_frlg_progress(&b->ruby, natdex, false);
    fake_ruby_set_party(&b->ruby, b->party, b->party_count);
}

/* bench_init_trade with an FRLG GBA (bench_frlg_player(version), its save's National Dex natdex)
 * offering CHARMANDER, tm_make(4, 5, 21), a Kanto Pokemon. The test's main must have called
 * bulk_file_open. */
static inline void bench_init_frlg(Bench* b, uint16_t version, bool natdex) {
    uint8_t mon[GEN3_PARTY_SIZE];
    const Gen3LinkPlayer me = bench_frlg_player(version);
    bench_init_cfg(b, GEN3_LINKTYPE_TRADE_SETUP, &club_cfg_trade);
    bench_swap_gba(b, &me, natdex);
    tm_make(4, 5, 21, mon);
    bench_offer(b, mon);
}

/* The GBA restarts (or its player is back at the counter) and links again; same club and party,
 * and an FRLG save's same progress. */
static inline void ruby_relink(Bench* b) {
    fake_ruby_init(&b->ruby, &b->me);
    if(fake_ruby_is_frlg(&b->ruby)) fake_ruby_set_frlg_progress(&b->ruby, b->natdex, false);
    fake_ruby_set_party(&b->ruby, b->party, b->party_count);
    fake_ruby_open_link(&b->ruby);
}

static inline void count_sent(Bench* b, const MlinkTx* tx) {
    if(tx->count != MLINK_BURST_LEN) return;
    if(tx->word[1] == 0x2222) b->sent_2222++;
    if(tx->word[1] == 0xCCCC) b->sent_cccc++;
    if(tx->word[1] == 0x5FFF) b->sent_5fff++;
    if(tx->word[1] == 0x2FFE) b->sent_2ffe++;
    if(tx->word[1] == 0x8888) b->sent_8888++;
    if(tx->word[1] == 0xCAFE) {
        b->sent_cafe++;
        if(tx->word[2] == 0x0017) b->sent_cafe17++;
    }
}

/* Feeds one side's command words of this burst to that side's block parse (all-zero words are no
 * command). True if they completed a block, which is then logged. */
static inline bool bench_parse(
    Gen3BlockRx* rx,
    uint32_t* first,
    uint16_t* id,
    BenchBlock* blocks,
    uint8_t* n,
    const uint16_t cmd[GEN3_CMD_LEN],
    uint32_t burst) {
    bool command = false;
    for(size_t i = 0; i < GEN3_CMD_LEN; i++) command = command || cmd[i] != 0;
    if(!command) return false;
    switch(gen3_block_rx_feed(rx, cmd)) {
    case Gen3BlockRxStarted:
        *first = burst;
        *id = cmd[2];
        return false;
    case Gen3BlockRxDone:
        if(*n < BENCH_BLOCKS) {
            BenchBlock* blk = &blocks[(*n)++];
            blk->size = rx->size;
            blk->code = (uint16_t)(rx->data[0] | (rx->data[1] << 8));
            blk->arg = (uint16_t)(rx->data[2] | (rx->data[3] << 8));
            blk->id = *id;
            blk->first_burst = *first;
            blk->last_burst = burst;
        }
        return true;
    default:
        return false;
    }
}

/* What both sides put on the wire in this step's burst: the Flipper's words first, then the
 * GBA's (a Flipper CCCC in the burst that completes the GBA's LinkPlayerBlock was sent before the
 * Flipper could see that block). */
static inline void bench_record(Bench* b, const MlinkTx* tx) {
    b->last_tx = *tx;
    if(tx->count != MLINK_BURST_LEN) return;
    uint32_t burst = ++b->bursts;
    const uint16_t* f = &tx->word[1];
    const uint16_t* g = &b->rx.word[1];

    bench_parse(&b->f_rx, &b->f_rx_first, &b->f_rx_id, b->f_blocks, &b->n_f_blocks, f, burst);
    if(f[0] == 0xCCCC && b->n_f_cccc < BENCH_CCCC) {
        b->f_cccc[b->n_f_cccc] = f[1];
        b->f_cccc_burst[b->n_f_cccc] = burst;
        b->n_f_cccc++;
    }
    if(f[0] == 0xCAFE && f[1] == 0x0016) {
        b->sent_cafe16++;
        b->f_cafe16_burst = burst;
    }
    if(f[0] == 0x2FFE && b->n_f_2ffe < BENCH_SYNCS) b->f_2ffe_burst[b->n_f_2ffe++] = burst;

    if(b->rx.count != MLINK_BURST_LEN) return;
    if(bench_parse(&b->g_rx, &b->g_rx_first, &b->g_rx_id, b->g_blocks, &b->n_g_blocks, g, burst) &&
       b->g_rx.size == GEN3_LINK_PLAYER_BLOCK_SIZE) {
        b->n_f_cccc = 0; /* a new link session: its CCCCs count from here */
    }
    if(g[0] == 0xCAFE && g[1] < 32) {
        b->g_cafe_seen |= 1u << g[1];
        if(g[1] == 0x0016) b->g_cafe16_burst = burst;
    }
    if(g[0] == 0x2FFE && b->n_g_2ffe < BENCH_SYNCS) b->g_2ffe_burst[b->n_g_2ffe++] = burst;
    if(g[0] == 0x5FFF) {
        b->g_5fff_burst = burst;
        if(b->n_g_close < BENCH_CLOSES) b->g_close_type[b->n_g_close++] = g[1];
    }
}

/* While muted, step() doesn't call the club: the fake gets frames without transfers (a stalled
 * master). b->rx keeps the answer to the club's last tx for when it runs again. */
static inline void bench_mute(Bench* b, bool muted) {
    b->muted = muted;
}

static inline void step(Bench* b) {
    MlinkTx tx;
    if(b->muted) {
        MlinkRx none;
        memset(&tx, 0, sizeof(tx));
        fake_ruby_frame(&b->ruby, &tx, &none);
        return;
    }
    gen3_club_frame(&b->club, b->started ? &b->rx : NULL, &tx);
    b->started = true;
    count_sent(b, &tx);
    fake_ruby_frame(&b->ruby, &tx, &b->rx);
    bench_record(b, &tx);
}

/* The codes (words 0) of the 20-byte (§B3.1 trade) blocks among blocks[0..n), in order; returns
 * their count. */
static inline size_t bench_codes(const BenchBlock* blocks, uint8_t n, uint16_t* out, size_t max) {
    size_t count = 0;
    for(uint8_t i = 0; i < n && count < max; i++) {
        if(blocks[i].size == 20) out[count++] = blocks[i].code;
    }
    return count;
}

/* The k-th 20-byte (§B3.1) block among blocks[0..n), or NULL */
static inline const BenchBlock* trade_block(const BenchBlock* blocks, uint8_t n, size_t k) {
    for(uint8_t i = 0; i < n; i++) {
        if(blocks[i].size != GEN3_TRADE_BLOCK_SIZE) continue;
        if(k-- == 0) return &blocks[i];
    }
    return NULL;
}

/* Index of the latest block of `size` among blocks[0..n), or -1 */
static inline int latest_block(const BenchBlock* blocks, uint8_t n, uint16_t size) {
    for(int i = n - 1; i >= 0; i--) {
        if(blocks[i].size == size) return i;
    }
    return -1;
}

static inline void run(Bench* b, uint32_t frames) {
    for(uint32_t i = 0; i < frames; i++) step(b);
}

static inline Gen3ClubStatus club_status(const Bench* b) {
    Gen3ClubStatus st;
    gen3_club_status(&b->club, &st);
    return st;
}

static inline bool run_until_phase(Bench* b, Gen3ClubPhase phase, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(club_status(b).phase == phase) return true;
    }
    return false;
}

static inline bool at_cards_or_later(Gen3ClubPhase p) {
    return p == Gen3ClubPhaseCards || p == Gen3ClubPhaseRoom;
}

/* Until both sides are in the Trade Center: the club answered 2FFE, the fake got that answer. */
static inline bool run_until_room(Bench* b, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        bool club_in = club_status(b).phase == Gen3ClubPhaseRoom;
        if(club_in && fake_ruby_phase(&b->ruby) == FakeRubyRoom) return true;
    }
    return false;
}

static inline bool reach_room(Bench* b) {
    bench_init(b, GEN3_LINKTYPE_TRADE_SETUP);
    fake_ruby_open_link(&b->ruby);
    return run_until_room(b, 400) && fake_ruby_error(&b->ruby) == NULL;
}

/* As reach_room, on a bench already initialised (no re-init). */
static inline bool bench_to_room(Bench* b) {
    fake_ruby_open_link(&b->ruby);
    return run_until_room(b, 420) && fake_ruby_error(&b->ruby) == NULL;
}

/* Until the GBA's trade menu is Browsing; false on a fake error or after max_frames. */
static inline bool run_until_browsing(Bench* b, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_menu_state(&b->ruby) == FakeRubyMenuBrowsing) return true;
    }
    return false;
}

/* Until the GBA's phase is `phase`; false on a fake error or after max_frames */
static inline bool run_until_fake_phase(Bench* b, FakeRubyPhase phase, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_phase(&b->ruby) == phase) return true;
        if(fake_ruby_error(&b->ruby) != NULL) return false;
    }
    return false;
}

/* Until the GBA's trade menu is in `state`; false on a fake error or after max_frames */
static inline bool run_until_menu_state(Bench* b, FakeRubyMenuState state, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_menu_state(&b->ruby) == state) return true;
    }
    return false;
}

/* On an initialised bench (a test may set the party or the offer first): bench_to_room, the GBA
 * sits on seat 1, then until its trade menu is Browsing (<= 600 frames). False on a fake error. */
static inline bool bench_reach_menu(Bench* b) {
    if(!bench_to_room(b)) return false;
    fake_ruby_sit(&b->ruby, 1);
    return run_until_browsing(b, 600);
}

/* For `frames` frames a fresh Gen3Link is the master instead of the club: it handshakes, and on
 * Connected queues {2222, 1133} and lp's LinkPlayerBlock once, then idles. The club doesn't run,
 * so it is out of step with the wire afterwards. */
static inline void bench_raw_relink(Bench* b, const Gen3LinkPlayer* lp, uint32_t frames) {
    static const uint16_t req[GEN3_CMD_LEN] = {0x2222, GEN3_LINKTYPE_TRADE_SETUP};
    uint8_t block[GEN3_LINK_PLAYER_BLOCK_SIZE];
    Gen3Link link;
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
        gen3_link_tx(&link, &tx);
        fake_ruby_frame(&b->ruby, &tx, &b->rx);
        bench_record(b, &tx);
    }
}

/* Queues a 20-byte (§B3.1) block {code, arg, 0...} on b->club.link (sender 0) as if the club
 * sent it. False if the queue lacks room. */
static inline bool bench_inject_code(Bench* b, uint16_t code, uint16_t arg) {
    uint8_t block[GEN3_TRADE_BLOCK_SIZE];
    gen3_trade_block_build(code, arg, block);
    return gen3_block_send(&b->club.link, block, sizeof(block), 0);
}

/* Queues one raw command {w0, w1, 0, 0, 0, 0, 0, 0} on b->club.link as if the club sent it. */
static inline void bench_inject_cmd(Bench* b, uint16_t w0, uint16_t w1) {
    const uint16_t cmd[GEN3_CMD_LEN] = {w0, w1};
    gen3_link_send(&b->club.link, cmd);
}

/* From Browsing: the GBA offers its slot gba_slot (fake_ruby_offer), says YES once "Is this trade
 * okay?" is up, then the trade, the link save and the post-trade exchange run until the club's
 * trades_done has grown and the GBA's trade menu is Browsing again (<= 4000 frames). False on a
 * fake error or a timeout. */
static inline bool bench_trade(Bench* b, uint8_t gba_slot) {
    const uint32_t done = club_status(b).trades_done;
    bool answered = false;
    fake_ruby_offer(&b->ruby, gba_slot);
    for(uint32_t i = 0; i < 4000; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        const FakeRubyMenuState menu = fake_ruby_menu_state(&b->ruby);
        if(!answered && menu == FakeRubyMenuPrompt) {
            fake_ruby_answer(&b->ruby, true);
            answered = true;
        }
        if(club_status(b).trades_done > done && menu == FakeRubyMenuBrowsing) return true;
    }
    return false;
}

/* The GBA's saved party is the one the bench gave it (b->party) */
static inline bool saved_party_untouched(const Bench* b) {
    for(uint8_t i = 0; i < FAKE_RUBY_PARTY; i++) {
        if(memcmp(fake_ruby_party(&b->ruby, i, true), b->party[i], GEN3_PARTY_SIZE) != 0) {
            return false;
        }
    }
    return true;
}

/* From Browsing: the GBA offers its slot 2 and says YES once its prompt is up; then until its
 * phase is Trade: it has just closed the CCDD's 0x1122 link (its 0x1144 OpenLink is due). False
 * on a fake error or after max_frames. */
static inline bool run_until_trade_phase(Bench* b, uint32_t max_frames) {
    fake_ruby_offer(&b->ruby, 2);
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_menu_state(&b->ruby) == FakeRubyMenuPrompt) fake_ruby_answer(&b->ruby, true);
        if(fake_ruby_phase(&b->ruby) == FakeRubyTrade) return true;
    }
    return false;
}

/* From Browsing: run_until_trade_phase (<= 600 frames), then until the GBA's trade link has the
 * Flipper's 0x1144 LinkPlayer (the trade's relink, before the animation) */
static inline bool run_until_trade_link(Bench* b, uint32_t max_frames) {
    if(!run_until_trade_phase(b, 600)) return false;
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(fake_ruby_partner(&b->ruby)->link_type == GEN3_LINKTYPE_TRADE_DISCONNECTED) return true;
    }
    return false;
}

/* From Browsing: run_until_trade_link, then until the frame in which the club has answered
 * `syncs` of the GBA's link-save 2FFEs */
static inline bool run_until_save_syncs(Bench* b, uint8_t syncs, uint32_t max_frames) {
    if(!run_until_trade_link(b, 600)) return false;
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        const Gen3ClubStatus st = club_status(b);
        if(st.phase == Gen3ClubPhaseSave && st.save_syncs == syncs) return true;
    }
    return false;
}

/* Until status.left, checking the GBA has no error; false after max_frames */
static inline bool run_until_left(Bench* b, uint32_t max_frames) {
    for(uint32_t i = 0; i < max_frames; i++) {
        step(b);
        if(fake_ruby_error(&b->ruby) != NULL) return false;
        if(club_status(b).left) return true;
    }
    return false;
}

/* From raw_gba_init on, the fake isn't stepped: raw_gba_run's scripted GBA link layer answers the
 * club instead, on the same checksum chain (transfer #1 carries the previous burst's sum, §A2),
 * for club branches a Ruby-faithful fake never reaches. It sends one 20-byte block
 * {code, arg, 0...} as slot 1, then idle bursts; both sides' blocks are logged as usual. A test
 * may break the block after raw_gba_init:
 * - wire_size: the header's size word as the Flipper reads it, while this GBA sums the 0x14 it
 *   sent (a glitch at the Flipper's receiver: the Flipper alone counts a checksum error, as this
 *   GBA doesn't check the Flipper's word);
 * - chunks: how many of the block's two chunks it sends (0..2);
 * - lag: idle bursts before the second chunk (GBA lag frames; Ruby otherwise sends a block's
 *   chunks on consecutive frames). */
typedef struct {
    uint16_t cmds[3][GEN3_CMD_LEN]; /* header, chunk 1, chunk 2 */
    uint16_t wire_size;
    uint8_t chunks;
    uint8_t lag;
    uint32_t sent; /* bursts so far */
    uint16_t sum; /* this GBA's checksum of its latest burst */
} BenchRawGba;

static inline void raw_gba_init(BenchRawGba* g, const Bench* b, uint16_t code, uint16_t arg) {
    uint8_t block[GEN3_TRADE_BLOCK_SIZE];
    memset(g, 0, sizeof(*g));
    g->cmds[0][0] = 0xBBBB;
    g->cmds[0][1] = GEN3_TRADE_BLOCK_SIZE;
    g->cmds[0][2] = 0x81;
    g->cmds[1][0] = 0x8888;
    g->cmds[2][0] = 0x8888;
    gen3_trade_block_build(code, arg, block);
    for(size_t i = 0; i < GEN3_TRADE_BLOCK_SIZE / 2; i++) {
        g->cmds[1 + i / 7][1 + i % 7] = (uint16_t)(block[2 * i] | (block[2 * i + 1] << 8));
    }
    g->wire_size = GEN3_TRADE_BLOCK_SIZE;
    g->chunks = 2;
    for(size_t i = 1; i < MLINK_BURST_LEN; i++) {
        g->sum = (uint16_t)(g->sum + b->last_tx.word[i] + b->rx.word[i]);
    }
}

/* The command this GBA sends in its burst s, or NULL for an idle burst. */
static inline const uint16_t* raw_gba_cmd(const BenchRawGba* g, uint32_t s) {
    if(s == 0) return g->cmds[0];
    if(s == 1) return g->chunks >= 1 ? g->cmds[1] : NULL;
    if(s == 2u + g->lag) return g->chunks >= 2 ? g->cmds[2] : NULL;
    return NULL;
}

/* `frames` more bursts. The club takes burst s's command in the frame after it. */
static inline void raw_gba_run(BenchRawGba* g, Bench* b, uint32_t frames) {
    for(uint32_t f = 0; f < frames; f++, g->sent++) {
        MlinkTx tx;
        gen3_club_frame(&b->club, &b->rx, &tx);
        count_sent(b, &tx);
        memset(&b->rx, 0, sizeof(b->rx));
        b->rx.count = tx.count;
        b->rx.word[0] = g->sum;
        const uint16_t* own = raw_gba_cmd(g, g->sent);
        if(own) memcpy(&b->rx.word[1], own, sizeof(g->cmds[0]));
        if(g->sent == 0) b->rx.word[2] = g->wire_size;
        g->sum = 0;
        for(size_t i = 1; i < MLINK_BURST_LEN; i++) {
            g->sum = (uint16_t)(g->sum + tx.word[i] + (own ? own[i - 1] : 0));
        }
        bench_record(b, &tx);
    }
}

/* The GBA player walks out of the room (§B7); the club must go through Closing to Waiting. */
/* The guide for st with no offer refusal and no pickup failure is l1 / l2 / l3 */
static inline bool guide_says(const Gen3ClubStatus* st, const char* l1, const char* l2, const char* l3) {
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
    if(!gen3_trade_guide(st, true, true, Gen3RefuseNone, Gen3PickupOk, out)) return false;
    if(strcmp(out[0], l1) == 0 && strcmp(out[1], l2) == 0 && strcmp(out[2], l3) == 0) return true;
    printf("  guide: \"%s\" / \"%s\" / \"%s\"\n", out[0], out[1], out[2]);
    return false;
}

static inline bool leave_room(Bench* b) {
    fake_ruby_leave_room(&b->ruby);
    return run_until_phase(b, Gen3ClubPhaseClosing, 600) &&
           run_until_phase(b, Gen3ClubPhaseWaiting, 60);
}
