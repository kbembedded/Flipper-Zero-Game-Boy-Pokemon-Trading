#include "test.h"
#include "bulk_file.h"
#include "gen3_golden.h"
#include <stdlib.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_partner.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_rules.h>
#include <src/gen3/gen3_trade_block.h>
#include <src/gen3/gen3_trade_sync.h>

#define G(f) gen3_pkm_get(&ed.pkm, Gen3Field##f)

/* The GBA player: RUBY, TID 12345, SID 54321 (OT ID 0xD4313039), female */
static const uint8_t RUBY_NAME[GEN3_OT_NAME_LEN] = {0xCC, 0xCF, 0xBC, 0xD3, 0xFF, 0xFF, 0xFF};
#define RUBY_OT_ID 0xD4313039u

#define ENIGMA_FIXTURE "../docs/gen3/fixtures/enigma-berry.pk3"

static void status_reset(Gen3ClubStatus* st, Gen3ClubPhase phase) {
    memset(st, 0, sizeof(*st));
    st->phase = phase;
}

/* A status whose trainer (the GBA's LinkPlayer from its 0x1133 linkup) is RUBY */
static void status_ruby(Gen3ClubStatus* st) {
    status_reset(st, Gen3ClubPhaseRoom);
    st->trainer_valid = true;
    st->trainer.version = GEN3_VERSION_RUBY;
    st->trainer.trainer_id = RUBY_OT_ID;
    memset(st->trainer.name, 0xFF, sizeof(st->trainer.name));
    memcpy(st->trainer.name, RUBY_NAME, GEN3_OT_NAME_LEN);
    st->trainer.gender = 1;
    st->trainer.link_type = GEN3_LINKTYPE_TRADE_SETUP;
    st->trainer.language = GEN3_LANGUAGE_ENGLISH;
}

static unsigned poll(
    Gen3TradeSync* sync,
    Gen3Editor* ed,
    const Gen3ClubStatus* st,
    const uint8_t received[GEN3_PARTY_SIZE],
    uint32_t count) {
    return gen3_trade_sync_poll(sync, ed, st, received, count, false);
}

/* The offer is the editor's Pokemon, game form */
static bool offer_is_editor(const Gen3TradeSync* sync, const Gen3Editor* ed) {
    uint8_t want[GEN3_PARTY_SIZE];
    gen3_pkm_encrypt(&ed->pkm, want);
    return sync->offer_valid && memcmp(sync->offer, want, GEN3_PARTY_SIZE) == 0;
}

/* GOLDEN with no move, game form: loading it needs the default moveset from the bulk file */
static void moveless_golden_game(uint8_t out[GEN3_PARTY_SIZE]) {
    Gen3Pkm p;
    golden_build(&p);
    for(int s = 0; s < 4; s++) {
        gen3_pkm_set(&p, Gen3FieldMove1 + s, 0);
        gen3_pkm_set(&p, Gen3FieldPp1 + s, 0);
    }
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, out);
}

TEST(test_trainer_from_link_player) {
    Gen3LinkPlayer lp;
    memset(&lp, 0, sizeof(lp));
    for(int i = 0; i < 11; i++) lp.name[i] = (uint8_t)(0xA0 + i);
    lp.trainer_id = 0xD4313039;
    lp.gender = 5;
    Gen3Trainer t;
    memset(&t, 0x5A, sizeof(t));
    gen3_trainer_from_link_player(&lp, &t);
    ASSERT_MEM_EQ(t.name, lp.name, GEN3_OT_NAME_LEN);
    ASSERT_EQ(t.tid, 12345);
    ASSERT_EQ(t.sid, 54321);
    ASSERT_EQ(t.gender, 1);

    /* Bytes 7..10 of the LinkPlayer name aren't part of the OT name */
    for(int i = 7; i < 11; i++) lp.name[i] = 0x00;
    lp.gender = 0;
    Gen3Trainer u;
    gen3_trainer_from_link_player(&lp, &u);
    ASSERT_MEM_EQ(u.name, t.name, GEN3_OT_NAME_LEN);
    ASSERT_EQ(u.tid, 12345);
    ASSERT_EQ(u.sid, 54321);
    ASSERT_EQ(u.gender, 0);
}

TEST(test_sync_init) {
    Gen3TradeSync sync;
    memset(&sync, 0xA5, sizeof(sync));
    gen3_trade_sync_init(&sync);
    ASSERT_EQ(sync.trades_seen, 0);
    ASSERT_TRUE(!sync.trainer_known);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    ASSERT_TRUE(!sync.taken);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);
}

TEST(test_sync_first_offer) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseWaiting);

    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    ASSERT_EQ(sync.trades_seen, 0);
    ASSERT_TRUE(ed.pristine); /* the sync itself changes nothing */
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);

    /* An edit is offered on the next poll */
    gen3_edit_set_level(&ed, 6);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    /* Not taken (the session refused it): offered again */
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    /* An edit undone before the next poll: the offer taken still holds */
    gen3_edit_set_level(&ed, 7);
    gen3_edit_set_level(&ed, 6);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
}

TEST(test_sync_trainer) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_ruby(&st);

    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer | Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw + 0x14, RUBY_NAME, GEN3_OT_NAME_LEN);
    ASSERT_EQ(G(OtId), RUBY_OT_ID);
    ASSERT_EQ(G(OtGender), 1);
    ASSERT_TRUE(ed.pristine);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    ASSERT_TRUE(sync.trainer_known);
    ASSERT_MEM_EQ(sync.trainer.name, RUBY_NAME, GEN3_OT_NAME_LEN);
    ASSERT_EQ(sync.trainer.tid, 12345);
    ASSERT_EQ(sync.trainer.sid, 54321);
    ASSERT_EQ(sync.trainer.gender, 1);
    ASSERT_TRUE(offer_is_editor(&sync, &ed)); /* with the player's OT */
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);

    /* Another player: the still-default Pokemon becomes theirs */
    st.trainer.trainer_id = 0x00020001;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer | Gen3SyncOffer);
    ASSERT_EQ(G(OtId), 0x00020001);
    ASSERT_TRUE(ed.pristine);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    gen3_trade_sync_offer_taken(&sync);

    /* No trainer in the status: the editor keeps the last one */
    st.trainer_valid = false;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    ASSERT_EQ(G(OtId), 0x00020001);

    /* An edited Pokemon keeps its OT: the player is handed over, the offer stays */
    gen3_edit_set_level(&ed, 10);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    status_ruby(&st);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer);
    ASSERT_EQ(G(OtId), 0x00020001);
    ASSERT_EQ(gen3_edit_trainer(&ed)->tid, 12345);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
}

TEST(test_sync_pickup) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);

    golden_game(rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);
    ASSERT_TRUE(sync.offer_valid);
    ASSERT_MEM_EQ(sync.offer, rx, GEN3_PARTY_SIZE);
    gen3_trade_sync_offer_taken(&sync);
    /* Taken once: the same count is no new trade */
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);

    /* The next trade */
    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldExp, gen3_exp_for_level(gen3_species(252)->growth, 10));
    gen3_recalc_party(&p);
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_EQ(sync.trades_seen, 2);
    ASSERT_EQ(G(Level), 10);
    ASSERT_MEM_EQ(sync.offer, rx, GEN3_PARTY_SIZE);
}

/* Ruby's MonGainEVs raises EVs without recalculating the party stats, and the pickup's
 * gen3_normalise recalculates them. The normalised Pokemon is published in the same poll as the
 * pickup (the first tick after trades_done advances), so its game form differs from the received
 * bytes the club offers after the trade. */
TEST(test_sync_pickup_stats_lag_evs) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);

    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldEvHp, 252);
    gen3_pkm_set(&p, Gen3FieldEvAtk, 252);
    gen3_pkm_fix_checksum(&p);
    uint16_t calc[Gen3StatCount];
    gen3_calc_stats(&p, calc);
    ASSERT_TRUE(calc[Gen3StatHp] != gen3_pkm_get(&p, Gen3FieldMaxHp)); /* the stats lag */
    ASSERT_TRUE(calc[Gen3StatAtk] != gen3_pkm_get(&p, Gen3FieldAtk));
    gen3_pkm_encrypt(&p, rx);

    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_EQ(G(EvHp), 252);
    ASSERT_EQ(G(EvAtk), 252);
    ASSERT_EQ(G(MaxHp), calc[Gen3StatHp]);
    ASSERT_EQ(G(Atk), calc[Gen3StatAtk]);
    ASSERT_EQ(G(Hp), 20); /* the current HP is kept */
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_TRUE(memcmp(sync.offer, rx, GEN3_PARTY_SIZE) != 0);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
}

/* D6: a received Pokemon replaces an edit the session never took */
TEST(test_sync_pickup_before_offer) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);

    gen3_edit_set_level(&ed, 30);
    golden_game(rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_TRUE(sync.offer_valid);
    ASSERT_MEM_EQ(sync.offer, rx, GEN3_PARTY_SIZE);
}

/* O2: the received bytes don't decrypt. The traded-away Pokemon is never offered again, its level
 * edited or not, until the editor holds another one (another PID or OT ID). */
TEST(test_sync_bad_received) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    Gen3Pkm before = ed.pkm;

    golden_game(rx);
    rx[0x30] ^= 1;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone); /* the Pokemon itself is fine */
    gen3_trade_sync_offer_taken(&sync);

    /* Not retried */
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);

    /* The same Pokemon edited: still not offered */
    gen3_edit_set_level(&ed, 30);
    ASSERT_EQ(G(Pid), gen3_pkm_get(&before, Gen3FieldPid));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
    ASSERT_TRUE(!sync.offer_valid);

    /* Another Pokemon (a new PID): offered */
    ASSERT_TRUE(gen3_edit_set_species(&ed, 4));
    ASSERT_TRUE(G(Pid) != gen3_pkm_get(&before, Gen3FieldPid));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);

    /* A load refused by gen3_edit_load (an egg) is BadData too; another OT ID frees the editor */
    gen3_trade_sync_offer_taken(&sync);
    before = ed.pkm;
    Gen3Pkm egg;
    golden_build(&egg);
    gen3_pkm_set(&egg, Gen3FieldIsEgg, 1);
    gen3_pkm_fix_checksum(&egg);
    gen3_pkm_encrypt(&egg, rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 1, 2), Gen3OtIdOk);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
}

/* O2: the block follows a trainer swap. Player A trades the default away and the pickup fails; B
 * and then A link again. The default takes each one's OT, and the Pokemon A traded away is never
 * published, to B or back to A. */
TEST(test_sync_block_follows_trainer) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_ruby(&st);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer | Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    Gen3Pkm traded = ed.pkm; /* what A gets */

    golden_game(rx);
    rx[0x30] ^= 1;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    gen3_trade_sync_offer_taken(&sync);

    /* B: the still-default Pokemon takes B's OT, and stays blocked */
    st.trainer.trainer_id = 0x00020001;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncTrainer);
    ASSERT_EQ(G(OtId), 0x00020001);
    ASSERT_TRUE(ed.pristine);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_TRUE(sync.gone_valid);
    ASSERT_EQ(sync.gone_pid, G(Pid));
    ASSERT_EQ(sync.gone_ot_id, 0x00020001);

    /* A again: the very Pokemon A traded away, still not offered */
    status_ruby(&st);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncTrainer);
    ASSERT_MEM_EQ(ed.pkm.raw, traded.raw, GEN3_PARTY_SIZE);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);

    /* The user's own new Pokemon frees it */
    ASSERT_TRUE(gen3_edit_set_species(&ed, 4));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
}

/* received_count below trades_seen is a club reset (trades_done back to 0, the received Pokemon
 * zeros): trades_seen follows it, with no pickup */
TEST(test_sync_club_reset) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseWaiting);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    golden_game(rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);

    memset(rx, 0, sizeof(rx));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    ASSERT_EQ(sync.trades_seen, 0);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_MEM_EQ(ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);

    /* The new club's first trade is picked up as usual */
    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldExp, gen3_exp_for_level(gen3_species(252)->growth, 10));
    gen3_recalc_party(&p);
    gen3_pkm_fix_checksum(&p);
    gen3_pkm_encrypt(&p, rx);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_EQ(G(Level), 10);
    gen3_trade_sync_offer_taken(&sync);

    /* A reset while a NoData pickup waits: that Pokemon went with the old club, so nothing is
     * retried, and the editor's Pokemon (traded away) stays blocked until the user changes it */
    moveless_golden_game(rx);
    bulk_file_failing = true;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_TRUE(bulk_file_open());
    Gen3Pkm held = ed.pkm;
    memset(rx, 0, sizeof(rx));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    ASSERT_EQ(sync.trades_seen, 0);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    ASSERT_MEM_EQ(ed.pkm.raw, held.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_TRUE(gen3_edit_set_species(&ed, 4));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
}

/* O2: the bulk file is unreadable at the pickup: retried on every poll, with the offer unpublished
 * meanwhile, until it works */
TEST(test_sync_pickup_retry) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    Gen3Pkm before = ed.pkm;

    moveless_golden_game(rx);
    bulk_file_failing = true;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);
    gen3_trade_sync_offer_taken(&sync);

    /* Still unreadable: tried again, nothing else changes */
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    ASSERT_MEM_EQ(ed.pkm.raw, before.raw, GEN3_PARTY_SIZE);

    ASSERT_TRUE(bulk_file_open());
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);
    ASSERT_EQ(gen3_edit_national(&ed), 252);
    ASSERT_EQ(G(Move1), 1); /* Treecko's level-5 moves: POUND, LEER */
    ASSERT_EQ(G(Move2), 43);
    ASSERT_EQ(G(Move3), 0);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
}

/* O2 across sessions: a restart (the session stopped, or started a new club) keeps the block on
 * the traded-away Pokemon, which is never offered again. Everything else begins again: no trade
 * seen, no offer taken, no trainer known. */
TEST(test_sync_restart_keeps_block) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    const uint8_t zeros[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_ruby(&st);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer | Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    const Gen3Pkm traded = ed.pkm;

    golden_game(rx);
    rx[0x30] ^= 1;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    gen3_trade_sync_offer_taken(&sync);

    gen3_trade_sync_restart(&sync);
    ASSERT_EQ(sync.trades_seen, 0);
    ASSERT_TRUE(!sync.trainer_known);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_MEM_EQ(sync.offer, zeros, GEN3_PARTY_SIZE);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    ASSERT_TRUE(!sync.taken);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_TRUE(sync.gone_valid);
    ASSERT_EQ(sync.gone_pid, gen3_pkm_get(&traded, Gen3FieldPid));
    ASSERT_EQ(sync.gone_ot_id, gen3_pkm_get(&traded, Gen3FieldOtId));

    /* The new club: nothing received, the same player. "No offer" is published, once. */
    memset(rx, 0, sizeof(rx));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer | Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw, traded.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_MEM_EQ(sync.offer, zeros, GEN3_PARTY_SIZE);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);

    /* The same Pokemon edited: still not offered */
    gen3_edit_set_level(&ed, 30);
    ASSERT_EQ(G(Pid), gen3_pkm_get(&traded, Gen3FieldPid));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    ASSERT_TRUE(!sync.offer_valid);

    /* Two restarts in a row change nothing more */
    gen3_trade_sync_restart(&sync);
    gen3_trade_sync_restart(&sync);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_TRUE(sync.gone_valid);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncTrainer | Gen3SyncOffer);
    ASSERT_TRUE(!sync.offer_valid);
}

/* After a restart the user's own change still frees the editor; a NoData retry pending at the
 * restart becomes BadData (its Pokemon went with the old club), with the block kept */
TEST(test_sync_restart_edit_clears_block) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseSave);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    const Gen3Pkm traded = ed.pkm;

    moveless_golden_game(rx);
    bulk_file_failing = true;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_TRUE(bulk_file_open());

    gen3_trade_sync_restart(&sync);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_TRUE(sync.gone_valid);
    ASSERT_EQ(sync.trades_seen, 0);

    /* The new club: no retry, the traded-away Pokemon stays unpublished */
    memset(rx, 0, sizeof(rx));
    status_reset(&st, Gen3ClubPhaseWaiting);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_MEM_EQ(ed.pkm.raw, traded.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!sync.offer_valid);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);

    /* Another OT ID: offered, and the block is gone for good */
    ASSERT_EQ(gen3_edit_set_ot_id(&ed, 1, 2), Gen3OtIdOk);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);
    gen3_trade_sync_restart(&sync);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(!sync.gone_valid);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
}

/* Without a block, a restart is exactly an init: the same bytes, and the same polls after it */
TEST(test_sync_restart_like_init) {
    Gen3Editor ed;
    Gen3TradeSync used, fresh;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&used);
    status_ruby(&st);
    ASSERT_EQ(poll(&used, &ed, &st, rx, 0), Gen3SyncTrainer | Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&used);
    golden_game(rx);
    ASSERT_EQ(poll(&used, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&used);
    ASSERT_EQ(used.trades_seen, 1);
    ASSERT_TRUE(used.trainer_known && used.taken && used.offer_valid);

    memset(&fresh, 0xA5, sizeof(fresh));
    gen3_trade_sync_init(&fresh);
    gen3_trade_sync_restart(&used);
    ASSERT_MEM_EQ(&used, &fresh, sizeof(fresh));

    memset(rx, 0, sizeof(rx));
    const unsigned r = poll(&fresh, &ed, &st, rx, 0);
    ASSERT_EQ(r, Gen3SyncTrainer | Gen3SyncOffer);
    ASSERT_EQ(poll(&used, &ed, &st, rx, 0), r);
    ASSERT_MEM_EQ(&used, &fresh, sizeof(fresh));
    ASSERT_TRUE(offer_is_editor(&used, &ed));
}

/* D6: while editing, nothing moves either way; afterwards all of it, in order */
TEST(test_sync_holds_while_editing) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseWaiting);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    uint8_t taken[GEN3_PARTY_SIZE];
    memcpy(taken, sync.offer, sizeof(taken));

    status_ruby(&st);
    golden_game(rx);
    gen3_edit_set_level(&ed, 30);
    Gen3Pkm edited = ed.pkm;
    ASSERT_EQ(gen3_trade_sync_poll(&sync, &ed, &st, rx, 1, true), 0);
    ASSERT_MEM_EQ(ed.pkm.raw, edited.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!ed.has_trainer);
    ASSERT_EQ(sync.trades_seen, 0);
    ASSERT_TRUE(!sync.trainer_known);
    ASSERT_MEM_EQ(sync.offer, taken, GEN3_PARTY_SIZE);
    ASSERT_MEM_EQ(sync.taken_offer, taken, GEN3_PARTY_SIZE);

    ASSERT_EQ(
        gen3_trade_sync_poll(&sync, &ed, &st, rx, 1, false),
        Gen3SyncPickedUp | Gen3SyncTrainer | Gen3SyncOffer);
    ASSERT_MEM_EQ(
        ed.pkm.raw, GOLDEN_PK3, GEN3_PARTY_SIZE); /* loaded: the player only remembered */
    ASSERT_TRUE(ed.has_trainer);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_TRUE(sync.trainer_known);
    ASSERT_MEM_EQ(sync.offer, rx, GEN3_PARTY_SIZE);
    gen3_trade_sync_offer_taken(&sync);

    /* A NoData retry waits for the editing to end too */
    moveless_golden_game(rx);
    bulk_file_failing = true;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_TRUE(bulk_file_open());
    Gen3Pkm held = ed.pkm;
    ASSERT_EQ(gen3_trade_sync_poll(&sync, &ed, &st, rx, 2, true), 0);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    ASSERT_MEM_EQ(ed.pkm.raw, held.raw, GEN3_PARTY_SIZE);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(
        gen3_trade_sync_poll(&sync, &ed, &st, rx, 2, false), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupOk);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
}

/* D8, R7: a Pokemon the trade can't take is never published (the club doesn't check) */
TEST(test_sync_bad_offer_unpublished) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseRoom);

    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldHeldItem, GEN3_ITEM_ENIGMA_BERRY);
    gen3_pkm_fix_checksum(&p);
    ASSERT_TRUE(gen3_edit_load(&ed, &p));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseItem);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);

    gen3_edit_set_item(&ed, 0);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    gen3_trade_sync_offer_taken(&sync);

    /* Defences: the editor can't hold Mail, so poke it */
    gen3_pkm_set(&ed.pkm, Gen3FieldHeldItem, GEN3_ITEM_MAIL_FIRST);
    gen3_pkm_fix_checksum(&ed.pkm);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseMail);
    gen3_pkm_set(&ed.pkm, Gen3FieldHeldItem, 0);
    gen3_pkm_set(&ed.pkm, Gen3FieldMail, 0);
    gen3_pkm_fix_checksum(&ed.pkm);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseMail);
    gen3_pkm_set(&ed.pkm, Gen3FieldMail, GEN3_MAIL_NONE);
    gen3_pkm_set(&ed.pkm, Gen3FieldHeldItem, GEN3_RUBY_ITEMS_COUNT);
    gen3_pkm_fix_checksum(&ed.pkm);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseItem);
    ASSERT_TRUE(!sync.offer_valid);
}

/* ---- The GBA's own rules (FireRed/LeafGreen): the offer's traits, and why the GBA linked last
 * won't take it. Such an offer is still published: the club decides, with the rules of the GBA
 * linked at that moment. ---- */

/* A status in Room whose GBA has these rules; refused None */
static void status_rules(Gen3ClubStatus* st, Gen3LinkGame game, bool national_dex) {
    status_reset(st, Gen3ClubPhaseRoom);
    st->partner_rules.game = game;
    st->partner_rules.national_dex = national_dex;
}

/* A fresh editor holding that species: 252 TREECKO (FRLG takes it only with the National Dex),
 * 151 MEW, 386 DEOXYS */
static bool editor_at(Gen3Editor* ed, uint16_t national) {
    return gen3_edit_init(ed, 7) && gen3_edit_set_species(ed, national);
}

TEST(test_sync_frlg_no_natdex) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(editor_at(&ed, 252));
    gen3_trade_sync_init(&sync);
    status_rules(&st, Gen3LinkGameFRLG, false);

    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed)); /* published: the club won't show it */
    ASSERT_EQ(sync.offer_traits, Gen3OfferNonKanto);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNationalDex);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNationalDex);

    /* A Kanto Pokemon: nothing refused */
    ASSERT_TRUE(gen3_edit_set_species(&ed, 4));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_traits, 0);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);

    /* DEOXYS: both traits, and the National Dex comes first */
    ASSERT_TRUE(editor_at(&ed, 386));
    gen3_edit_set_fateful(&ed, false);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_traits, Gen3OfferNonKanto | Gen3OfferUnfateful);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNationalDex);
}

TEST(test_sync_frlg_natdex) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(editor_at(&ed, 252));
    gen3_trade_sync_init(&sync);
    status_rules(&st, Gen3LinkGameFRLG, true);

    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_traits, Gen3OfferNonKanto);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
}

/* Only an FRLG partner refuses; the traits are the offer's whatever the partner */
TEST(test_sync_ruby_ignores_rules) {
    static const Gen3LinkGame games[] = {Gen3LinkGameUnknown, Gen3LinkGameRS, Gen3LinkGameE};
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    for(unsigned g = 0; g < sizeof(games) / sizeof(games[0]); g++) {
        for(int natdex = 0; natdex <= 1; natdex++) {
            status_rules(&st, games[g], natdex);
            gen3_trade_sync_init(&sync);
            ASSERT_TRUE(editor_at(&ed, 252));
            ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
            ASSERT_TRUE(offer_is_editor(&sync, &ed));
            ASSERT_EQ(sync.offer_traits, Gen3OfferNonKanto);
            ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);

            ASSERT_TRUE(editor_at(&ed, 151));
            gen3_edit_set_fateful(&ed, false);
            ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
            ASSERT_TRUE(offer_is_editor(&sync, &ed));
            ASSERT_EQ(sync.offer_traits, Gen3OfferUnfateful);
            ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
        }
    }
}

TEST(test_sync_unfateful_mew) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(editor_at(&ed, 151));
    gen3_trade_sync_init(&sync);
    status_rules(&st, Gen3LinkGameFRLG, true);

    gen3_edit_set_fateful(&ed, false);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_traits, Gen3OfferUnfateful);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseFateful);
    gen3_trade_sync_offer_taken(&sync);

    gen3_edit_set_fateful(&ed, true);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_traits, 0);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
}

/* Another GBA with other rules: the reason follows it, and the offer taken still holds (the club
 * applies the new rules itself) */
TEST(test_sync_partner_change) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(editor_at(&ed, 252));
    gen3_trade_sync_init(&sync);
    status_rules(&st, Gen3LinkGameRS, true);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    gen3_trade_sync_offer_taken(&sync);

    status_rules(&st, Gen3LinkGameFRLG, false);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_traits, Gen3OfferNonKanto);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNationalDex);

    status_rules(&st, Gen3LinkGameRS, true);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), 0);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
}

/* gen3_trade_offer_check first: a Pokemon no game may take has no traits and no partner reason.
 * The editor can't hold Mail, so poke it. */
TEST(test_sync_check_before_partner) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(editor_at(&ed, 252));
    gen3_trade_sync_init(&sync);
    status_rules(&st, Gen3LinkGameFRLG, false);
    gen3_pkm_set(&ed.pkm, Gen3FieldHeldItem, 121);
    gen3_pkm_fix_checksum(&ed.pkm);

    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseMail);
    ASSERT_EQ(sync.offer_traits, 0);
}

/* O2 under the GBA's rules: the Pokemon traded away is never offered again, whichever GBA is
 * linked, and a blocked offer has no traits and no partner reason */
TEST(test_sync_frlg_keeps_pickup_block) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(editor_at(&ed, 252));
    gen3_trade_sync_init(&sync);
    status_rules(&st, Gen3LinkGameFRLG, true);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    gen3_trade_sync_offer_taken(&sync);

    golden_game(rx);
    rx[0x30] ^= 1; /* doesn't decrypt: BadData */
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_traits, 0);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    gen3_trade_sync_offer_taken(&sync);

    /* Another GBA, without the National Dex: still blocked, and no reason of its own */
    status_rules(&st, Gen3LinkGameFRLG, false);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), 0);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
    gen3_trade_sync_restart(&sync);
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(!sync.offer_valid);
    ASSERT_EQ(sync.offer_traits, 0);

    /* Another Pokemon frees the editor */
    ASSERT_TRUE(gen3_edit_set_species(&ed, 4));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    ASSERT_TRUE(offer_is_editor(&sync, &ed));
    ASSERT_EQ(sync.offer_refuse, Gen3RefuseNone);
}

/* R7: the ENIGMA BERRY holder for the device check (checklist H.3) */
TEST(test_enigma_fixture) {
    Gen3Pkm p;
    golden_build(&p);
    gen3_pkm_set(&p, Gen3FieldHeldItem, GEN3_ITEM_ENIGMA_BERRY);
    uint8_t want[GEN3_BOX_SIZE];
    gen3_pk3_export(&p, want);

    if(getenv("GEN3_WRITE_FIXTURES")) {
        FILE* w = fopen(ENIGMA_FIXTURE, "wb");
        ASSERT_TRUE(w != NULL);
        size_t n = fwrite(want, 1, sizeof(want), w);
        ASSERT_EQ(fclose(w), 0);
        ASSERT_EQ(n, sizeof(want));
    }

    uint8_t got[GEN3_BOX_SIZE + 1];
    FILE* f = fopen(ENIGMA_FIXTURE, "rb");
    ASSERT_TRUE(f != NULL);
    size_t n = fread(got, 1, sizeof(got), f);
    fclose(f);
    ASSERT_EQ(n, GEN3_BOX_SIZE);
    ASSERT_MEM_EQ(got, want, GEN3_BOX_SIZE);

    Gen3Pkm q;
    ASSERT_EQ(gen3_pk3_import(got, GEN3_BOX_SIZE, &q, NULL), Gen3CheckOk);
    ASSERT_EQ(gen3_pkm_get(&q, Gen3FieldHeldItem), GEN3_ITEM_ENIGMA_BERRY);
    ASSERT_EQ(gen3_trade_offer_check(&q), Gen3RefuseItem);
}

/* A trade session may stop for good only once the sync has every Pokemon the club received. A
 * NoData pickup waits for a retry; a BadData one is final. */
TEST(test_sync_settled) {
    Gen3Editor ed;
    Gen3TradeSync sync;
    Gen3ClubStatus st;
    uint8_t rx[GEN3_PARTY_SIZE] = {0};
    ASSERT_TRUE(gen3_edit_init(&ed, 7));
    gen3_trade_sync_init(&sync);
    status_reset(&st, Gen3ClubPhaseRoom);
    ASSERT_TRUE(gen3_trade_sync_settled(&sync, 0));
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 0), Gen3SyncOffer);
    gen3_trade_sync_offer_taken(&sync);
    ASSERT_TRUE(gen3_trade_sync_settled(&sync, 0));

    /* A trade the sync hasn't polled yet (an editing scene was shown) */
    moveless_golden_game(rx);
    ASSERT_TRUE(!gen3_trade_sync_settled(&sync, 1));

    /* Polled, but the bulk file can't be read: the Pokemon is still only in the club */
    bulk_file_failing = true;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.trades_seen, 1);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupNoData);
    ASSERT_TRUE(!gen3_trade_sync_settled(&sync, 1));

    ASSERT_TRUE(bulk_file_open());
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 1), Gen3SyncPickedUp | Gen3SyncOffer);
    ASSERT_TRUE(gen3_trade_sync_settled(&sync, 1));

    /* A received Pokemon that will never load: final, so the session may stop */
    golden_game(rx);
    rx[0x30] ^= 1;
    ASSERT_EQ(poll(&sync, &ed, &st, rx, 2), Gen3SyncPickupBad | Gen3SyncOffer);
    ASSERT_EQ(sync.pickup_fail, Gen3PickupBadData);
    ASSERT_TRUE(gen3_trade_sync_settled(&sync, 2));
    ASSERT_TRUE(!gen3_trade_sync_settled(&sync, 3));
}

/* The sweeps below cover every phase with every combination of the flags the end decisions
 * read: END_BITS bits 0-3 and 5 set these five, and bit 4 is the settled argument. */
#define END_BITS 64u
static void end_status(Gen3ClubStatus* st, int phase, unsigned bits) {
    status_reset(st, (Gen3ClubPhase)phase);
    st->save_window = bits & 1;
    st->trade_pending = (bits >> 1) & 1;
    st->leaving = (bits >> 2) & 1;
    st->left = (bits >> 3) & 1;
    st->relinking = (bits >> 5) & 1;
}

/* A GBA is linked, or in a relink's gap: Waiting is "no GBA linked" only without relinking */
static bool end_linked(const Gen3ClubStatus* st) {
    return st->phase != Gen3ClubPhaseWaiting || st->relinking;
}

/* R6: "Stop link?" may stop the link outside the save window, and outside a pending trade with the
 * GBA linked, a relink's gap included. Once the relink has timed out (Waiting, not relinking), a
 * pending trade may stop (D1): the GBA wrote nothing before sync 1. left and leaving don't matter:
 * left is no "trade resolved". */
TEST(test_trade_stop_allowed) {
    Gen3ClubStatus st;

    status_reset(&st, Gen3ClubPhaseRoom);
    ASSERT_TRUE(gen3_trade_stop_allowed(&st));
    st.save_window = true;
    ASSERT_TRUE(!gen3_trade_stop_allowed(&st));

    status_reset(&st, Gen3ClubPhaseTrade);
    st.trade_pending = true;
    ASSERT_TRUE(!gen3_trade_stop_allowed(&st));
    st.left = true; /* the 0x1144 timeout can set left with the trade still pending */
    ASSERT_TRUE(!gen3_trade_stop_allowed(&st));
    /* The 0x1144 gap (Waiting, the relink due) is still linked */
    st.left = false;
    st.phase = Gen3ClubPhaseWaiting;
    st.relinking = true;
    ASSERT_TRUE(!gen3_trade_stop_allowed(&st));
    /* The relink timed out: the GBA is gone, and may be stopped again */
    st.relinking = false;
    st.left = true;
    ASSERT_TRUE(gen3_trade_stop_allowed(&st));

    /* Independent invariants over every phase and flag, not the formula again */
    for(int ph = Gen3ClubPhaseWaiting; ph <= Gen3ClubPhaseSave; ph++) {
        for(unsigned bits = 0; bits < END_BITS; bits++) {
            end_status(&st, ph, bits);
            const bool allowed = gen3_trade_stop_allowed(&st);
            const bool linked = end_linked(&st);
            if(st.save_window) ASSERT_TRUE(!allowed); /* D10: never in the save window */
            if(!linked && !st.save_window) ASSERT_TRUE(allowed); /* D1: no GBA linked */
            if(linked && st.trade_pending) ASSERT_TRUE(!allowed); /* R6 */
            if(linked && !st.trade_pending && !st.save_window) ASSERT_TRUE(allowed);
            /* leaving and left never change it */
            Gen3ClubStatus plain = st;
            plain.leaving = false;
            plain.left = false;
            ASSERT_EQ(gen3_trade_stop_allowed(&plain), allowed);
        }
    }
}

/* End trade, or Back on the Gen III menu (running: the session runs) */
TEST(test_trade_end_action) {
    Gen3ClubStatus st;

    /* D9: a linked GBA is asked to walk out first, settled or not */
    status_reset(&st, Gen3ClubPhaseRoom);
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndAsk);
    ASSERT_EQ(gen3_trade_end_action(&st, true, false), Gen3TradeEndAsk);
    st.phase = Gen3ClubPhaseMenu;
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndAsk);
    st.phase = Gen3ClubPhaseTrade; /* G.3: End during the trade animation */
    st.trade_pending = true;
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndAsk);

    /* D10: the save window has no End, whatever was shown before (a stale "End trade?") */
    st.phase = Gen3ClubPhaseSave;
    st.save_window = true;
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndSaving);
    ASSERT_EQ(gen3_trade_end_action(&st, true, false), Gen3TradeEndSaving);

    /* No GBA linked (D1): stop now, unless a received Pokemon still waits for the SD card */
    status_reset(&st, Gen3ClubPhaseWaiting);
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndStop);
    ASSERT_EQ(gen3_trade_end_action(&st, true, false), Gen3TradeEndPickup);
    st.left = true;
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndStop);
    ASSERT_EQ(gen3_trade_end_action(&st, true, false), Gen3TradeEndPickup);
    st.left = false;
    st.trade_pending = true; /* the 0x1144 relink timed out: nothing written on the GBA yet */
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndStop);

    /* A relink's gap (the 0x1144 one after CCDD, the 0x1122 one after the save) is no "no GBA
     * linked": ask it to walk out once it relinks, never drop it mid-relink */
    st.relinking = true;
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndAsk);
    ASSERT_EQ(gen3_trade_end_action(&st, true, false), Gen3TradeEndAsk);
    st.trade_pending = false; /* the post-save 0x1122 gap */
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndAsk);
    /* The relink timed out (the club cleared it): stop again */
    st.relinking = false;
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndStop);

    /* A stale dialog. The tick stopped the session (the GBA left, its Pokemon picked up): back to
     * the menu, never End on it. */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.left = true;
    ASSERT_EQ(gen3_trade_end_action(&st, false, true), Gen3TradeEndMenu);
    /* A Pickup becomes a Stop once a retry has picked the Pokemon up */
    st.left = false;
    ASSERT_EQ(gen3_trade_end_action(&st, true, false), Gen3TradeEndPickup);
    ASSERT_EQ(gen3_trade_end_action(&st, true, true), Gen3TradeEndStop);

    /* Never a stop R6 forbids, nor one with a Pokemon still to pick up; never asks a GBA that
     * isn't linked to walk out; a stopped session only goes back to the menu */
    for(int ph = Gen3ClubPhaseWaiting; ph <= Gen3ClubPhaseSave; ph++) {
        for(unsigned bits = 0; bits < END_BITS; bits++) {
            end_status(&st, ph, bits);
            const bool settled = (bits >> 4) & 1;
            ASSERT_EQ(gen3_trade_end_action(&st, false, settled), Gen3TradeEndMenu);
            const Gen3TradeEndAction a = gen3_trade_end_action(&st, true, settled);
            ASSERT_TRUE(a != Gen3TradeEndMenu);
            ASSERT_EQ(a == Gen3TradeEndSaving, st.save_window);
            if(a == Gen3TradeEndStop) {
                ASSERT_TRUE(gen3_trade_stop_allowed(&st));
                ASSERT_TRUE(settled);
            }
            if(a == Gen3TradeEndAsk) ASSERT_TRUE(end_linked(&st));
            /* A stop or a pickup only with no GBA linked, a relink's gap included */
            if(a == Gen3TradeEndStop || a == Gen3TradeEndPickup) ASSERT_TRUE(!end_linked(&st));
            if(a == Gen3TradeEndPickup) {
                ASSERT_TRUE(!settled);
                ASSERT_TRUE(gen3_trade_stop_allowed(&st));
            }
        }
    }
}

/* "Stop link?" confirmed (D1), and the Pickup dialog opened from it: a linked GBA may be stopped
 * too, where R6 allows */
TEST(test_trade_stop_link_action) {
    Gen3ClubStatus st;

    /* A leave that stalled in the room: stop, or Pickup first */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.leaving = true;
    ASSERT_EQ(gen3_trade_stop_link_action(&st, true, true), Gen3TradeEndStop);
    ASSERT_EQ(gen3_trade_stop_link_action(&st, true, false), Gen3TradeEndPickup);

    /* R6: the GBA is saving, or a trade is under way with it linked: no stop */
    st.phase = Gen3ClubPhaseSave;
    st.trade_pending = true;
    st.save_window = true;
    ASSERT_EQ(gen3_trade_stop_link_action(&st, true, true), Gen3TradeEndSaving);
    st.save_window = false;
    st.phase = Gen3ClubPhaseTrade;
    ASSERT_EQ(gen3_trade_stop_link_action(&st, true, true), Gen3TradeEndAsk);
    st.phase = Gen3ClubPhaseWaiting; /* the 0x1144 gap is still linked */
    st.relinking = true;
    ASSERT_EQ(gen3_trade_stop_link_action(&st, true, true), Gen3TradeEndAsk);
    st.relinking = false; /* the relink timed out: may stop (D1) */
    ASSERT_EQ(gen3_trade_stop_link_action(&st, true, true), Gen3TradeEndStop);
    /* A relink's gap without a trade pending: a confirmed "Stop link?" may stop it */
    st.trade_pending = false;
    st.relinking = true;
    ASSERT_EQ(gen3_trade_stop_link_action(&st, true, true), Gen3TradeEndStop);
    st.relinking = false;

    /* Stopped meanwhile: back to the menu */
    ASSERT_EQ(gen3_trade_stop_link_action(&st, false, true), Gen3TradeEndMenu);

    for(int ph = Gen3ClubPhaseWaiting; ph <= Gen3ClubPhaseSave; ph++) {
        for(unsigned bits = 0; bits < END_BITS; bits++) {
            end_status(&st, ph, bits);
            const bool settled = (bits >> 4) & 1;
            ASSERT_EQ(gen3_trade_stop_link_action(&st, false, settled), Gen3TradeEndMenu);
            const Gen3TradeEndAction a = gen3_trade_stop_link_action(&st, true, settled);
            ASSERT_EQ(a == Gen3TradeEndSaving, st.save_window);
            /* Stop or Pickup exactly where R6 allows a stop; Stop only once settled */
            const bool stops = a == Gen3TradeEndStop || a == Gen3TradeEndPickup;
            ASSERT_EQ(stops, gen3_trade_stop_allowed(&st));
            if(a == Gen3TradeEndStop) ASSERT_TRUE(settled);
            if(a == Gen3TradeEndPickup) ASSERT_TRUE(!settled);
            /* Where End trade would stop, so does "Stop link?" */
            if(gen3_trade_end_action(&st, true, settled) == Gen3TradeEndStop) {
                ASSERT_EQ(a, Gen3TradeEndStop);
            }
        }
    }
}

/* Back on the trade screen */
TEST(test_trade_back_action) {
    Gen3ClubStatus st;

    status_reset(&st, Gen3ClubPhaseRoom);
    ASSERT_EQ(gen3_trade_back_action(&st, true), Gen3TradeBackLeave);
    st.leaving = true; /* a leave that may stall: plan 1's "Stop link?" (D1) */
    ASSERT_EQ(gen3_trade_back_action(&st, true), Gen3TradeBackStopLink);

    /* D10: locked in the save window, leaving or not */
    st.phase = Gen3ClubPhaseSave;
    st.trade_pending = true;
    st.save_window = true;
    ASSERT_EQ(gen3_trade_back_action(&st, true), Gen3TradeBackLocked);
    st.leaving = false;
    ASSERT_EQ(gen3_trade_back_action(&st, true), Gen3TradeBackLocked);

    /* R6: End trade during the animation: back to the menu, never "Stop link?" */
    st.save_window = false;
    st.phase = Gen3ClubPhaseTrade;
    st.leaving = true;
    ASSERT_EQ(gen3_trade_back_action(&st, true), Gen3TradeBackLeave);
    st.phase = Gen3ClubPhaseWaiting; /* the 0x1144 gap: still no "Stop link?" */
    st.relinking = true;
    ASSERT_EQ(gen3_trade_back_action(&st, true), Gen3TradeBackLeave);
    st.relinking = false; /* the relink timed out: may stop (D1) */
    ASSERT_EQ(gen3_trade_back_action(&st, true), Gen3TradeBackStopLink);

    /* A stopped session: a stale flag locks nothing */
    status_reset(&st, Gen3ClubPhaseSave);
    st.save_window = true;
    st.leaving = true;
    ASSERT_EQ(gen3_trade_back_action(&st, false), Gen3TradeBackLeave);

    for(int ph = Gen3ClubPhaseWaiting; ph <= Gen3ClubPhaseSave; ph++) {
        for(unsigned bits = 0; bits < END_BITS; bits++) {
            end_status(&st, ph, bits);
            for(int running = 0; running < 2; running++) {
                const Gen3TradeBackAction a = gen3_trade_back_action(&st, running);
                ASSERT_EQ(a == Gen3TradeBackLocked, running && st.save_window);
                if(a == Gen3TradeBackStopLink) {
                    ASSERT_TRUE(running && st.leaving);
                    ASSERT_TRUE(gen3_trade_stop_allowed(&st));
                }
            }
        }
    }
}

/* The app tick's own stop once the GBA has walked out (left, D9). left is sticky, so the GBA may
 * be linked again by then (it walked back into the Cable Club while the session waited for a
 * pickup): the tick stops only with no GBA linked, which R6 then allows too, and settled */
TEST(test_trade_left_stop) {
    Gen3ClubStatus st;

    status_reset(&st, Gen3ClubPhaseWaiting);
    ASSERT_TRUE(!gen3_trade_left_stop(&st, true)); /* no leave asked */
    st.left = true;
    ASSERT_TRUE(gen3_trade_left_stop(&st, true));
    ASSERT_TRUE(!gen3_trade_left_stop(&st, false)); /* a received Pokemon not picked up yet */
    st.trade_pending = true; /* its 0x1144 relink never came: nothing written on the GBA (D1) */
    ASSERT_TRUE(gen3_trade_left_stop(&st, true));
    st.relinking = true; /* a relink's gap is linked */
    ASSERT_TRUE(!gen3_trade_left_stop(&st, true));

    /* The GBA walked back in after left: in the room, and in a trade's link save (D10) */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.left = true;
    ASSERT_TRUE(!gen3_trade_left_stop(&st, true));
    status_reset(&st, Gen3ClubPhaseSave);
    st.left = true;
    st.trade_pending = true;
    st.save_window = true;
    ASSERT_TRUE(!gen3_trade_left_stop(&st, true));
    ASSERT_TRUE(!gen3_trade_stop_allowed(&st));

    for(int ph = Gen3ClubPhaseWaiting; ph <= Gen3ClubPhaseSave; ph++) {
        for(unsigned bits = 0; bits < END_BITS; bits++) {
            end_status(&st, ph, bits);
            const bool settled = (bits >> 4) & 1;
            const bool stop = gen3_trade_left_stop(&st, settled);
            if(stop) {
                ASSERT_TRUE(st.left && settled);
                ASSERT_TRUE(gen3_trade_stop_allowed(&st)); /* R6, D10 */
                ASSERT_TRUE(!end_linked(&st));
            }
            /* And it does stop once the GBA is gone with everything picked up */
            if(st.left && settled && !end_linked(&st) && !st.save_window) ASSERT_TRUE(stop);
        }
    }
}

/* The guide as three strings; prints what it got on a mismatch */
static bool guide_is(
    const Gen3ClubStatus* st,
    bool started,
    bool cable_ok,
    Gen3Refuse offer_refuse,
    Gen3PickupFail pickup_fail,
    const char* l1,
    const char* l2,
    const char* l3) {
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
    memset(out, 'X', sizeof(out));
    if(!gen3_trade_guide(st, started, cable_ok, offer_refuse, pickup_fail, out)) {
        printf("  guide: a text was cut: \"%s\" / \"%s\" / \"%s\"\n", out[0], out[1], out[2]);
        return false;
    }
    for(int i = 0; i < GEN3_GUIDE_LINES; i++) {
        if(memchr(out[i], '\0', GEN3_GUIDE_LINE_LEN) == NULL) {
            printf("  guide line %d has no NUL\n", i + 1);
            return false;
        }
    }
    if(strcmp(out[0], l1) != 0 || strcmp(out[1], l2) != 0 || strcmp(out[2], l3) != 0) {
        printf("  guide: \"%s\" / \"%s\" / \"%s\"\n", out[0], out[1], out[2]);
        printf("  want:  \"%s\" / \"%s\" / \"%s\"\n", l1, l2, l3);
        return false;
    }
    return true;
}

#define GUIDE(st, r, pf, l1, l2, l3) ASSERT_TRUE(guide_is((st), true, true, (r), (pf), l1, l2, l3))
#define GUIDE_ROW(st, r, pf, row)    GUIDE((st), (r), (pf), (row)[0], (row)[1], (row)[2])

/* The Trade Center's counter on Pokemon Center 2F: the middle one in Ruby and Sapphire, the
 * right-hand attendant (then TRADE CENTER) in FireRed, LeafGreen and Emerald. With no GBA linked
 * the next GBA's game is unknown, so the Waiting row names both. */
static const char* const ROW_GO_CLUB[GEN3_GUIDE_LINES] = {
    "Go to the Cable Club:",
    "PC 2F, Trade Center:",
    "RS middle, FRLG right",
};
static const char* const ROW_OTHER_ROOM_MIDDLE[GEN3_GUIDE_LINES] = {
    "The GBA chose another",
    "room. Use the middle",
    "counter: Trade Center",
};
static const char* const ROW_OTHER_ROOM_RIGHT[GEN3_GUIDE_LINES] = {
    "The GBA chose another",
    "room. Right counter,",
    "then TRADE CENTER.",
};
/* A trade pending with no GBA linked: its 0x1144 link never came, or closed before the trade */
static const char* const ROW_LOST_BEFORE_TRADE[GEN3_GUIDE_LINES] = {
    "Link lost before the",
    "trade. On a GBA link",
    "error, restart it.",
};

/* The reason line of each refusal */
static const char* const REASON[] = {
    [Gen3RefuseEmpty] = "that slot is empty.",
    [Gen3RefuseBadEgg] = "it's a Bad Egg.",
    [Gen3RefuseEgg] = "it's an Egg.",
    [Gen3RefuseMail] = "it holds Mail.",
    [Gen3RefuseItem] = "it holds a bad item.",
    [Gen3RefuseBadData] = "its data is invalid.",
    [Gen3RefuseNationalDex] = "National Dex needed.",
    [Gen3RefuseFateful] = "it's not fateful.",
};

TEST(test_guide_texts) {
    Gen3ClubStatus st;
    const Gen3Refuse no = Gen3RefuseNone;
    const Gen3PickupFail ok = Gen3PickupOk;

    /* Cable and start, ahead of everything */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.save_window = true;
    ASSERT_TRUE(
        guide_is(&st, true, false, no, ok, "Plug in the cable", "(or swap its ends).", ""));
    ASSERT_TRUE(guide_is(&st, false, true, no, ok, "Starting...", "", ""));

    /* The save window (D10) */
    status_reset(&st, Gen3ClubPhaseSave);
    st.save_window = true;
    st.trade_pending = true;
    st.leaving = true;
    GUIDE(&st, no, ok, "The GBA is saving.", "Don't unplug or turn", "off. Back is locked.");

    /* left (D9) says nothing about a trade still pending: left isn't "trade resolved" */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.left = true;
    st.trade_pending = true;
    st.last_result = Gen3TradeResultCanceled;
    GUIDE(&st, Gen3RefuseItem, Gen3PickupBadData, "Link ended. You can", "unplug the GBA.", "");

    /* End trade during a trade whose link was then lost in the GBA's save: the leave is done
     * (left), and the save's outcome (D5) outranks "Link ended" */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.left = true;
    st.last_result = Gen3TradeResultRolledBack;
    GUIDE(&st, no, ok, "Link lost while the", "GBA saved. It keeps", "its old save.");
    st.last_result = Gen3TradeResultAssumed;
    GUIDE(&st, no, ok, "Link lost at the end.", "Check the GBA party:", "it likely traded.");
    st.left = false;
    st.leaving = true;
    GUIDE(&st, no, ok, "Link lost at the end.", "Check the GBA party:", "it likely traded.");
    /* A pickup still waiting comes first, as without the leave */
    st.leaving = false;
    st.left = true;
    GUIDE(
        &st,
        no,
        Gen3PickupNoData,
        "Can't take the traded",
        "Pokemon: data file",
        "unreadable. Retrying.");
    /* Nothing written on the GBA, or nothing lost: "Link ended" */
    st.last_result = Gen3TradeResultCanceled;
    GUIDE(&st, no, ok, "Link ended. You can", "unplug the GBA.", "");
    st.last_result = Gen3TradeResultDone;
    GUIDE(&st, no, ok, "Link ended. You can", "unplug the GBA.", "");
    /* An older trade's outcome: a later trade pending, a relink's gap, the GBA linked again. A
     * GBA linked after the leave (it walked back in while a pickup waits) is walked out first:
     * unplugged now, it would show a link error */
    st.last_result = Gen3TradeResultAssumed;
    st.trade_pending = true;
    GUIDE(&st, no, ok, "Link ended. You can", "unplug the GBA.", "");
    st.trade_pending = false;
    st.relinking = true;
    GUIDE(&st, no, ok, "Walk out on the GBA,", "then unplug.", "");
    st.relinking = false;
    st.phase = Gen3ClubPhaseRoom;
    GUIDE(&st, no, ok, "Walk out on the GBA,", "then unplug.", "");

    /* left with a GBA linked, whatever else is set below the save's outcome */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.left = true;
    st.gba_seated = true;
    GUIDE(&st, no, ok, "Walk out on the GBA,", "then unplug.", "");
    GUIDE(
        &st, Gen3RefuseNationalDex, Gen3PickupNoData, "Walk out on the GBA,", "then unplug.", "");
    status_reset(&st, Gen3ClubPhaseMenu);
    st.left = true;
    st.offer_stale = true;
    GUIDE(&st, no, ok, "Walk out on the GBA,", "then unplug.", "");
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.left = true;
    st.relinking = true;
    GUIDE(&st, no, ok, "Walk out on the GBA,", "then unplug.", "");
    st.relinking = false;
    GUIDE(&st, no, ok, "Link ended. You can", "unplug the GBA.", "");

    /* leaving: no "offer changed" hint in the menu */
    status_reset(&st, Gen3ClubPhaseMenu);
    st.leaving = true;
    st.offer_stale = true;
    st.menu = Gen3ClubMenuLeaving;
    GUIDE(&st, no, ok, "Ending: the GBA walks", "out of the Trade", "Center by itself.");
    st.trade_pending = true;
    GUIDE(&st, no, ok, "Ending: the GBA walks", "out of the Trade", "Center by itself.");

    status_reset(&st, Gen3ClubPhaseMenu);
    st.trade_pending = true;
    st.menu = Gen3ClubMenuStarting;
    GUIDE(&st, Gen3RefuseItem, Gen3PickupNoData, "Trading...", "Watch the GBA.", "");
    status_reset(&st, Gen3ClubPhaseTrade);
    st.trade_pending = true;
    GUIDE(&st, no, ok, "Trading...", "Watch the GBA.", "");
    /* The trade still pending with no GBA linked, not even in a relink's gap: its 0x1144 link
     * didn't come within the club's relink wait (a GBA turned off in the gap), or FRLG's own
     * deadline on that link closed it before the trade (a link error on the GBA). Not
     * "Trading...": nothing is under way */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.trade_pending = true;
    GUIDE_ROW(&st, no, ok, ROW_LOST_BEFORE_TRADE);
    GUIDE_ROW(&st, Gen3RefuseNationalDex, Gen3PickupNoData, ROW_LOST_BEFORE_TRADE);
    st.last_result = Gen3TradeResultDone; /* an older trade's */
    GUIDE_ROW(&st, no, ok, ROW_LOST_BEFORE_TRADE);
    st.relinking = true; /* the gap before the relink wait runs out: linked */
    GUIDE(&st, no, ok, "Trading...", "Watch the GBA.", "");
    /* A late relink: a GBA is linked again, and the trade may go on */
    status_reset(&st, Gen3ClubPhaseLinked);
    st.trade_pending = true;
    GUIDE(&st, no, ok, "Trading...", "Watch the GBA.", "");

    /* Errors */
    status_reset(&st, Gen3ClubPhaseError);
    st.last_error = Gen3ClubErrorOtherRoom;
    GUIDE_ROW(&st, Gen3RefuseItem, ok, ROW_OTHER_ROOM_MIDDLE); /* game Unknown: as R/S */
    st.last_error = Gen3ClubErrorBadPlayerBlock;
    GUIDE(&st, no, ok, "Link error. Restart", "the GBA, then go to", "the Cable Club.");
    st.last_error = Gen3ClubErrorUnexpected;
    GUIDE(
        &st,
        no,
        Gen3PickupBadData,
        "Link error. Restart",
        "the GBA, then go to",
        "the Cable Club.");

    /* O2: a failed pickup, above any offer refusal */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.gba_seated = true;
    GUIDE(
        &st,
        Gen3RefuseItem,
        Gen3PickupNoData,
        "Can't take the traded",
        "Pokemon: data file",
        "unreadable. Retrying.");
    status_reset(&st, Gen3ClubPhaseWaiting);
    GUIDE(
        &st,
        no,
        Gen3PickupBadData,
        "Can't take the traded",
        "Pokemon: bad data.",
        "Load or pick another.");

    /* R5: no valid offer while the GBA sits: the Flipper joins once there is one */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.gba_seated = true;
    GUIDE(
        &st,
        Gen3RefuseItem,
        ok,
        "Can't offer it:",
        REASON[Gen3RefuseItem],
        "Edit it; stay seated.");

    /* No valid offer, each reason of gen3_trade_offer_check (the GBA's own rules have their own
     * rows: test_guide_frlg_rows) */
    for(Gen3Refuse r = Gen3RefuseEmpty; r <= Gen3RefuseBadData; r++) {
        status_reset(&st, Gen3ClubPhaseWaiting);
        st.last_result = Gen3TradeResultRolledBack;
        GUIDE(&st, r, ok, "Can't offer it:", REASON[r], "Edit it first.");
        status_reset(&st, Gen3ClubPhaseMenu);
        st.offer_stale = true;
        GUIDE(&st, r, ok, "Can't offer it:", REASON[r], "Edit it first.");
    }

    /* Waiting: how the latest trade ended */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.last_result = Gen3TradeResultRolledBack;
    GUIDE(&st, no, ok, "Link lost while the", "GBA saved. It keeps", "its old save.");
    st.last_result = Gen3TradeResultAssumed;
    GUIDE(&st, no, ok, "Link lost at the end.", "Check the GBA party:", "it likely traded.");
    st.last_result = Gen3TradeResultCanceled;
    GUIDE(&st, no, ok, "Link lost. Nothing", "was traded.", "");
    st.last_result = Gen3TradeResultDone;
    GUIDE_ROW(&st, no, ok, ROW_GO_CLUB);
    st.last_result = Gen3TradeResultNone;
    GUIDE_ROW(&st, no, ok, ROW_GO_CLUB);
    /* After a Disconnected, the link state, not the re-sit hint */
    st.resit_needed = true;
    GUIDE_ROW(&st, no, ok, ROW_GO_CLUB);

    /* A relink's gap: Waiting with relinking is the GBA opening its next link, about 20 frames. It
     * counts as linked, so never the Waiting rows */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.relinking = true; /* the sit's 0x1122, to the trade menu */
    GUIDE(&st, no, ok, "Linking...", "", "");
    st.resit_needed = true; /* the 0x1111 back to the room, after an EEBB */
    GUIDE(&st, no, ok, "Linking...", "", "");
    st.resit_needed = false;
    st.trades_done = 1; /* the 0x1122 back to the menu, after the save */
    st.last_result = Gen3TradeResultDone;
    GUIDE(&st, no, ok, "Linking...", "", "");
    st.trade_pending = true; /* the trade's 0x1144, after CCDD */
    GUIDE(&st, no, ok, "Trading...", "Watch the GBA.", "");
    st.leaving = true;
    GUIDE(&st, no, ok, "Ending: the GBA walks", "out of the Trade", "Center by itself.");
    /* The rows above the phase rows still come first */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.relinking = true;
    GUIDE(&st, Gen3RefuseItem, ok, "Can't offer it:", REASON[Gen3RefuseItem], "Edit it first.");
    /* The relink wait ran out: the GBA is gone, plain Waiting again */
    st.relinking = false;
    GUIDE_ROW(&st, no, ok, ROW_GO_CLUB);

    status_reset(&st, Gen3ClubPhaseLinked);
    GUIDE(&st, no, ok, "Linking...", "", "");
    status_reset(&st, Gen3ClubPhasePlayers);
    GUIDE(&st, no, ok, "Linking...", "", "");
    status_reset(&st, Gen3ClubPhaseCards);
    st.resit_needed = true;
    GUIDE(&st, no, ok, "Linking...", "", "");

    /* Room */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.resit_needed = true;
    GUIDE(&st, no, ok, "Sit at the table", "again (step off and", "back on if seated).");
    st.gba_seated = true; /* the hint only while the GBA isn't seated */
    GUIDE(&st, no, ok, "Opening the trade", "menu...", "");
    st.resit_needed = false;
    GUIDE(&st, no, ok, "Opening the trade", "menu...", "");
    status_reset(&st, Gen3ClubPhaseExchange);
    GUIDE(&st, no, ok, "Opening the trade", "menu...", "");
    status_reset(&st, Gen3ClubPhaseRoom);
    st.flipper_seated = true;
    GUIDE(&st, no, ok, "In the Trade Center:", "sit at the table on", "the GBA.");

    /* Menu */
    status_reset(&st, Gen3ClubPhaseMenu);
    st.offer_stale = true;
    st.refused = Gen3RefuseEgg;
    GUIDE(&st, no, ok, "Updating the trade", "menu: the GBA goes", "back to the room.");
    status_reset(&st, Gen3ClubPhaseMenu);
    st.menu = Gen3ClubMenuLeaving;
    GUIDE(&st, no, ok, "Updating the trade", "menu: the GBA goes", "back to the room.");
    for(Gen3Refuse r = Gen3RefuseEmpty; r <= Gen3RefuseFateful; r++) {
        status_reset(&st, Gen3ClubPhaseMenu);
        st.refused = r;
        st.trades_done = 1;
        GUIDE(&st, no, ok, "Can't take that one:", REASON[r], "Pick another one.");
    }
    status_reset(&st, Gen3ClubPhaseMenu);
    st.menu = Gen3ClubMenuOffered;
    st.trades_done = 2;
    GUIDE(&st, no, ok, "Confirm the trade", "on the GBA.", "");
    status_reset(&st, Gen3ClubPhaseMenu);
    st.trades_done = 1;
    GUIDE(&st, no, ok, "Edit it (OK), or pick", "the next trade on", "the GBA.");
    status_reset(&st, Gen3ClubPhaseMenu);
    GUIDE(&st, no, ok, "Pick a Pokemon to", "trade on the GBA.", "");
    /* An R8 DDEE leaves refused at None: no reason is claimed */
    st.menu = Gen3ClubMenuIdle;
    st.refused = Gen3RefuseNone;
    GUIDE(&st, no, ok, "Pick a Pokemon to", "trade on the GBA.", "");

    /* Otherwise */
    status_reset(&st, Gen3ClubPhaseClosing);
    GUIDE(&st, no, ok, "Please wait...", "", "");
    st.relinking = true; /* the club answered the GBA's 5FFF; its link not closed yet */
    GUIDE(&st, no, ok, "Please wait...", "", "");
    status_reset(&st, Gen3ClubPhaseSave);
    st.last_result = Gen3TradeResultDone;
    st.trades_done = 1;
    GUIDE(&st, no, ok, "Please wait...", "", "");
    status_reset(&st, Gen3ClubPhaseTrade);
    GUIDE(&st, no, ok, "Please wait...", "", "");
}

/* The GBA's own rules refuse the offer (FireRed/LeafGreen): rows of their own, after the pickup
 * rows, only while a GBA is linked. The rules outlive the link: unlinked, they may be another
 * cartridge's, so the Waiting rows show instead. */
static const char* const ROW_NATDEX[GEN3_GUIDE_LINES] = {
    "Can't offer it: GBA",
    "has no National Dex.",
    "Get it, or pick 1-151",
};
static const char* const ROW_FATEFUL[GEN3_GUIDE_LINES] = {
    "Can't offer it: FRLG",
    "needs Fateful: Yes on",
    "Mew/Deoxys: edit it.",
};

TEST(test_guide_frlg_rows) {
    Gen3ClubStatus st;
    const Gen3Refuse nd = Gen3RefuseNationalDex;
    const Gen3Refuse fa = Gen3RefuseFateful;
    const Gen3PickupFail ok = Gen3PickupOk;

    /* Linked: the trade menu's way in, the room, the menu, a close */
    for(int phase = Gen3ClubPhaseLinked; phase <= Gen3ClubPhaseSave; phase++) {
        if(phase == Gen3ClubPhaseError) continue;
        status_reset(&st, (Gen3ClubPhase)phase);
        GUIDE_ROW(&st, nd, ok, ROW_NATDEX);
        GUIDE_ROW(&st, fa, ok, ROW_FATEFUL);
    }
    /* Seated: no "stay seated" variant */
    status_reset(&st, Gen3ClubPhaseRoom);
    st.gba_seated = true;
    GUIDE_ROW(&st, nd, ok, ROW_NATDEX);
    GUIDE_ROW(&st, fa, ok, ROW_FATEFUL);
    /* Ahead of the menu's "Updating the trade menu" */
    status_reset(&st, Gen3ClubPhaseMenu);
    st.offer_stale = true;
    st.trades_done = 1;
    GUIDE_ROW(&st, nd, ok, ROW_NATDEX);
    GUIDE_ROW(&st, fa, ok, ROW_FATEFUL);
    /* A relink's gap is linked */
    status_reset(&st, Gen3ClubPhaseWaiting);
    st.relinking = true;
    GUIDE_ROW(&st, nd, ok, ROW_NATDEX);
    GUIDE_ROW(&st, fa, ok, ROW_FATEFUL);

    /* No GBA linked: the Waiting rows, as with no refusal */
    for(Gen3Refuse r = nd; r <= fa; r++) {
        status_reset(&st, Gen3ClubPhaseWaiting);
        GUIDE_ROW(&st, r, ok, ROW_GO_CLUB);
        st.last_result = Gen3TradeResultRolledBack;
        GUIDE(&st, r, ok, "Link lost while the", "GBA saved. It keeps", "its old save.");
    }

    /* The rows above them still win */
    status_reset(&st, Gen3ClubPhaseRoom);
    ASSERT_TRUE(
        guide_is(&st, true, false, nd, ok, "Plug in the cable", "(or swap its ends).", ""));
    st.save_window = true;
    GUIDE(&st, nd, ok, "The GBA is saving.", "Don't unplug or turn", "off. Back is locked.");
    status_reset(&st, Gen3ClubPhaseMenu);
    st.trade_pending = true;
    GUIDE(&st, fa, ok, "Trading...", "Watch the GBA.", "");
    status_reset(&st, Gen3ClubPhaseRoom);
    st.gba_seated = true;
    GUIDE(
        &st,
        nd,
        Gen3PickupNoData,
        "Can't take the traded",
        "Pokemon: data file",
        "unreadable. Retrying.");
}

/* The GBA linked as another room (Ruby's Record Corner at the right counter, FireRed's
 * COLOSSEUM): its LinkPlayerBlock parsed, so the guide names that game's Trade Center counter,
 * never the one the player just used. With no GBA linked, the rules are the last GBA's, and the
 * next one may be another cartridge: the Waiting row names both counters. */
TEST(test_guide_counter_by_game) {
    Gen3ClubStatus st;
    const Gen3Refuse no = Gen3RefuseNone;
    const Gen3PickupFail ok = Gen3PickupOk;
    static const struct {
        Gen3LinkGame game;
        const char* const* other_room;
    } games[] = {
        {Gen3LinkGameUnknown, ROW_OTHER_ROOM_MIDDLE}, /* as R/S */
        {Gen3LinkGameRS, ROW_OTHER_ROOM_MIDDLE},
        {Gen3LinkGameE, ROW_OTHER_ROOM_RIGHT}, /* Emerald's 2F is laid out as FRLG's */
        {Gen3LinkGameFRLG, ROW_OTHER_ROOM_RIGHT},
    };
    for(size_t i = 0; i < sizeof(games) / sizeof(games[0]); i++) {
        status_reset(&st, Gen3ClubPhaseError);
        st.last_error = Gen3ClubErrorOtherRoom;
        st.partner_rules.game = games[i].game;
        st.partner_rules.national_dex = games[i].game != Gen3LinkGameFRLG;
        GUIDE_ROW(&st, no, ok, games[i].other_room);
        GUIDE_ROW(&st, Gen3RefuseItem, Gen3PickupBadData, games[i].other_room);
        /* The other errors name no counter */
        st.last_error = Gen3ClubErrorBadPlayerBlock;
        GUIDE(&st, no, ok, "Link error. Restart", "the GBA, then go to", "the Cable Club.");
        /* The GBA's 5FFF answered, its link closed: Waiting, last_error kept */
        status_reset(&st, Gen3ClubPhaseWaiting);
        st.last_error = Gen3ClubErrorOtherRoom;
        st.partner_rules.game = games[i].game;
        GUIDE_ROW(&st, no, ok, ROW_GO_CLUB);
        st.last_error = Gen3ClubErrorNone;
        GUIDE_ROW(&st, no, ok, ROW_GO_CLUB);
        GUIDE_ROW(&st, Gen3RefuseNationalDex, ok, ROW_GO_CLUB);
    }
}

/* Every line fits FontSecondary across 128 px (21 characters) for every combination */
static unsigned long guide_calls;
static bool guide_saw_confirm, guide_saw_next;
/* A "Link lost ..." row about a lost trade shown with a partner refusal (one only the unlinked
 * sweep reaches) */
static bool guide_saw_partner_lost;
/* A line naming one game's counter seen (the other-room rows): middle, right */
static bool guide_saw_middle, guide_saw_right;

/* A known game's player is never sent to another game's counter: "middle" only for R/S (and an
 * unknown game, as R/S), "right" only for FRLG and Emerald, except in the row that names both */
static bool guide_counter_ok(const Gen3ClubStatus* st, const char out[][GEN3_GUIDE_LINE_LEN]) {
    bool right_game = false;
    switch(st->partner_rules.game) {
    case Gen3LinkGameE:
    case Gen3LinkGameFRLG:
        right_game = true;
        break;
    case Gen3LinkGameRS:
    case Gen3LinkGameUnknown:
        break;
    }
    for(int i = 0; i < GEN3_GUIDE_LINES; i++) {
        if(strcmp(out[i], ROW_GO_CLUB[2]) == 0) continue;
        const bool middle = strstr(out[i], "middle") != NULL;
        const bool right = strstr(out[i], "right") != NULL || strstr(out[i], "Right") != NULL;
        guide_saw_middle |= middle;
        guide_saw_right |= right;
        if(right_game ? middle : right) {
            printf("  guide, game %d: \"%s\"\n", st->partner_rules.game, out[i]);
            return false;
        }
    }
    return true;
}

/* One sweep call: gen3_trade_guide cut no text, and line 1 says something */
static bool guide_fits(const Gen3ClubStatus* st, Gen3Refuse offer, Gen3PickupFail pf) {
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
    guide_calls++;
    if(!gen3_trade_guide(st, true, true, offer, pf, out) || out[0][0] == '\0') {
        printf(
            "  guide: phase %d error %d game %d menu %d trades_done %u refused %d result %d "
            "offer %d pickup %d: \"%s\" / \"%s\" / \"%s\"\n",
            st->phase,
            st->last_error,
            st->partner_rules.game,
            st->menu,
            (unsigned)st->trades_done,
            st->refused,
            st->last_result,
            offer,
            pf,
            out[0],
            out[1],
            out[2]);
        return false;
    }
    /* A relink's gap is linked: never the Waiting rows, whatever else is set */
    if(st->relinking && (strcmp(out[0], ROW_GO_CLUB[0]) == 0 ||
                         strncmp(out[0], "Link lost", strlen("Link lost")) == 0)) {
        printf(
            "  guide in a relink gap: phase %d result %d: \"%s\"\n",
            st->phase,
            st->last_result,
            out[0]);
        return false;
    }
    /* "unplug" only with no GBA linked; the GBA's own rules only with one linked (unlinked, they
     * may be another cartridge's); "Trading..." only with one linked, and a trade lost before it
     * began only with none */
    const bool linked = st->phase != Gen3ClubPhaseWaiting || st->relinking;
    const bool partner = offer == Gen3RefuseNationalDex || offer == Gen3RefuseFateful;
    const bool lost_before = strcmp(out[0], ROW_LOST_BEFORE_TRADE[0]) == 0;
    if((linked && strcmp(out[0], "Link ended. You can") == 0) ||
       (!linked && strcmp(out[0], "Walk out on the GBA,") == 0) ||
       (!linked && partner && strncmp(out[0], "Can't offer it", strlen("Can't offer it")) == 0) ||
       (!linked && strcmp(out[0], "Trading...") == 0) || (linked && lost_before)) {
        printf(
            "  guide, linked %d: phase %d relinking %d offer %d: \"%s\"\n",
            linked,
            st->phase,
            st->relinking,
            offer,
            out[0]);
        return false;
    }
    if(!guide_counter_ok(st, out)) return false;
    guide_saw_confirm |= strcmp(out[0], "Confirm the trade") == 0;
    guide_saw_next |= strcmp(out[0], "Edit it (OK), or pick") == 0;
    guide_saw_partner_lost |= partner && !lost_before &&
                              strncmp(out[0], "Link lost", strlen("Link lost")) == 0;
    return true;
}

/* The sweep's status: phase, the GBA's game (partner_rules), last_error, the eight flags of
 * test_guide_line_lengths by bit, menu and trades_done; refused and last_result None */
static void sweep_status(
    Gen3ClubStatus* st,
    int phase,
    int game,
    int err,
    unsigned flags,
    int menu,
    uint32_t done) {
    status_reset(st, (Gen3ClubPhase)phase);
    st->partner_rules.game = (Gen3LinkGame)game;
    st->partner_rules.national_dex = game != Gen3LinkGameFRLG;
    st->last_error = (Gen3ClubError)err;
    st->save_window = flags & 1;
    st->left = flags & 2;
    st->leaving = flags & 4;
    st->trade_pending = flags & 8;
    st->offer_stale = flags & 16;
    st->resit_needed = flags & 32;
    st->gba_seated = flags & 64;
    st->relinking = flags & 128;
    st->menu = (Gen3ClubMenu)menu;
    st->trades_done = done;
}

/* (a), then (b), of test_guide_line_lengths on st */
static bool guide_fits_linked(Gen3ClubStatus* st) {
    for(int r = Gen3RefuseNone; r <= Gen3RefuseFateful; r++) {
        for(int res = Gen3TradeResultNone; res <= Gen3TradeResultAssumed; res++) {
            st->refused = (Gen3Refuse)r;
            st->last_result = (Gen3TradeResult)res;
            if(!guide_fits(st, Gen3RefuseNone, Gen3PickupOk)) return false;
        }
    }
    st->refused = Gen3RefuseNone;
    st->last_result = Gen3TradeResultNone;
    for(int offer = Gen3RefuseNone; offer <= Gen3RefuseFateful; offer++) {
        for(int pf = Gen3PickupOk; pf <= Gen3PickupBadData; pf++) {
            if(!guide_fits(st, (Gen3Refuse)offer, (Gen3PickupFail)pf)) return false;
        }
    }
    return true;
}

/* (c) of test_guide_line_lengths on st */
static bool guide_fits_unlinked(Gen3ClubStatus* st) {
    for(int offer = Gen3RefuseNone; offer <= Gen3RefuseFateful; offer++) {
        for(int pf = Gen3PickupOk; pf <= Gen3PickupBadData; pf++) {
            for(int res = Gen3TradeResultNone; res <= Gen3TradeResultAssumed; res++) {
                st->last_result = (Gen3TradeResult)res;
                if(!guide_fits(st, (Gen3Refuse)offer, (Gen3PickupFail)pf)) return false;
            }
        }
    }
    return true;
}

/* Every text fits FontSecondary across 128 px (21 characters): gen3_trade_guide reports any text
 * it had to cut. Every phase x last_error x {save_window, left, leaving, trade_pending,
 * offer_stale, resit_needed, gba_seated, relinking} x menu x trades_done (0, 1), in the Error
 * phase x partner_rules.game too, then
 * (a) refused x last_result with no pickup failure and no offer refusal, and
 * (b) offer_refuse x pickup_fail with refused and last_result None.
 * The game: only the Error phase's other-room row reads it (it names that game's Trade Center
 * counter), so there every game, and elsewhere Unknown. The Waiting row names both counters
 * whatever the game (test_guide_counter_by_game).
 * While a GBA is linked (a phase other than Waiting, or relinking), a pickup failure or an offer
 * refusal decides the guide before any row that reads refused or last_result, so there (a) and
 * (b) stand for the full cross product. With no GBA linked they don't: last_result decides
 * whether the latest trade was lost in the GBA's save, ahead of the pickup and offer rows, and a
 * partner refusal (NationalDex, Fateful) falls through to the Waiting rows, which read
 * last_result. So a second sweep, counted on its own, covers every unlinked status:
 * (c) Waiting, not relinking, x last_error x the other seven flags x menu x trades_done, then
 * offer_refuse x pickup_fail x last_result, with refused None (no unlinked row reads it).
 * Every call also checks that a known game's player is never sent to another game's counter
 * (guide_counter_ok). */
TEST(test_guide_line_lengths) {
    Gen3ClubStatus st;
    guide_calls = 0;
    guide_saw_confirm = false;
    guide_saw_next = false;
    guide_saw_middle = false;
    guide_saw_right = false;
    for(int phase = Gen3ClubPhaseWaiting; phase <= Gen3ClubPhaseSave; phase++) {
        const int last_game = phase == Gen3ClubPhaseError ? Gen3LinkGameFRLG : Gen3LinkGameUnknown;
        for(int game = Gen3LinkGameUnknown; game <= last_game; game++) {
            for(int err = Gen3ClubErrorNone; err <= Gen3ClubErrorUnexpected; err++) {
                for(unsigned flags = 0; flags < (1u << 8); flags++) {
                    for(int menu = Gen3ClubMenuIdle; menu <= Gen3ClubMenuLeaving; menu++) {
                        for(uint32_t done = 0; done <= 1; done++) {
                            sweep_status(&st, phase, game, err, flags, menu, done);
                            ASSERT_TRUE(guide_fits_linked(&st));
                        }
                    }
                }
            }
        }
    }
    ASSERT_EQ(GEN3_GUIDE_LINE_LEN, 22);
    /* 10 phases with game Unknown, and the Error phase with each of the 4 games */
    ASSERT_EQ(guide_calls, (10ul + 4) * 5 * 256 * 4 * 2 * (9 * 5 + 9 * 3));
    ASSERT_TRUE(guide_saw_confirm);
    ASSERT_TRUE(guide_saw_next);
    ASSERT_TRUE(guide_saw_middle);
    ASSERT_TRUE(guide_saw_right);

    /* (c) No GBA linked */
    guide_calls = 0;
    guide_saw_partner_lost = false;
    for(int err = Gen3ClubErrorNone; err <= Gen3ClubErrorUnexpected; err++) {
        for(unsigned flags = 0; flags < (1u << 7); flags++) {
            for(int menu = Gen3ClubMenuIdle; menu <= Gen3ClubMenuLeaving; menu++) {
                for(uint32_t done = 0; done <= 1; done++) {
                    sweep_status(
                        &st, Gen3ClubPhaseWaiting, Gen3LinkGameUnknown, err, flags, menu, done);
                    ASSERT_TRUE(guide_fits_unlinked(&st));
                }
            }
        }
    }
    ASSERT_EQ(guide_calls, 5ul * 128 * 4 * 2 * (9 * 3 * 5));
    /* With no GBA linked, a partner refusal leaves the Waiting rows to say how a trade was lost */
    ASSERT_TRUE(guide_saw_partner_lost);
}

/* The sweep's check can fail: a text over 21 characters is cut to 21, and reported */
TEST(test_guide_copy_reports_cut) {
    static const char fit[] = "123456789012345678901"; /* 21 */
    static const char over[] = "1234567890123456789012"; /* 22 */
    static const char row34[] = "Pick a Pokemon to trade on the GBA"; /* 34 */
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN];
    ASSERT_EQ(strlen(fit), 21);
    ASSERT_EQ(strlen(over), 22);
    ASSERT_EQ(strlen(row34), 34);

    ASSERT_TRUE(gen3_trade_guide_copy(out, fit, fit, ""));
    ASSERT_STR_EQ(out[0], fit);
    ASSERT_STR_EQ(out[1], fit);
    ASSERT_STR_EQ(out[2], "");
    for(int i = 0; i < GEN3_GUIDE_LINES; i++) {
        const char* l[GEN3_GUIDE_LINES] = {"a", "b", "c"};
        l[i] = over;
        memset(out, 'X', sizeof(out));
        ASSERT_TRUE(!gen3_trade_guide_copy(out, l[0], l[1], l[2]));
        for(int j = 0; j < GEN3_GUIDE_LINES; j++) ASSERT_STR_EQ(out[j], j == i ? fit : l[j]);
    }
    ASSERT_TRUE(!gen3_trade_guide_copy(out, "Pick a Pokemon to", row34, ""));
    ASSERT_STR_EQ(out[1], "Pick a Pokemon to tra");
}

/* Each test starts with the bulk file readable, whatever a test that failed with
 * bulk_file_failing set left behind */
#define RUN_BULK(name) \
    do { \
        if(!bulk_file_open()) return 1; \
        RUN(name); \
    } while(0)

int main(void) {
    RUN_BULK(test_trainer_from_link_player);
    RUN_BULK(test_sync_init);
    RUN_BULK(test_sync_first_offer);
    RUN_BULK(test_sync_trainer);
    RUN_BULK(test_sync_pickup);
    RUN_BULK(test_sync_pickup_stats_lag_evs);
    RUN_BULK(test_sync_pickup_before_offer);
    RUN_BULK(test_sync_bad_received);
    RUN_BULK(test_sync_block_follows_trainer);
    RUN_BULK(test_sync_club_reset);
    RUN_BULK(test_sync_pickup_retry);
    RUN_BULK(test_sync_restart_keeps_block);
    RUN_BULK(test_sync_restart_edit_clears_block);
    RUN_BULK(test_sync_restart_like_init);
    RUN_BULK(test_sync_holds_while_editing);
    RUN_BULK(test_sync_bad_offer_unpublished);
    RUN_BULK(test_sync_frlg_no_natdex);
    RUN_BULK(test_sync_frlg_natdex);
    RUN_BULK(test_sync_ruby_ignores_rules);
    RUN_BULK(test_sync_unfateful_mew);
    RUN_BULK(test_sync_partner_change);
    RUN_BULK(test_sync_check_before_partner);
    RUN_BULK(test_sync_frlg_keeps_pickup_block);
    RUN_BULK(test_enigma_fixture);
    RUN_BULK(test_sync_settled);
    RUN_BULK(test_trade_stop_allowed);
    RUN_BULK(test_trade_end_action);
    RUN_BULK(test_trade_stop_link_action);
    RUN_BULK(test_trade_back_action);
    RUN_BULK(test_trade_left_stop);
    RUN_BULK(test_guide_texts);
    RUN_BULK(test_guide_frlg_rows);
    RUN_BULK(test_guide_counter_by_game);
    RUN_BULK(test_guide_line_lengths);
    RUN_BULK(test_guide_copy_reports_cut);
    TEST_EXIT();
}
