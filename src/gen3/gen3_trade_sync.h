#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_club.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_trade_block.h>

/* The app side of a trade session (D6): between UI events the app tick moves the received
 * Pokemon into the editor, hands the editor the GBA player (D7) and publishes the editor's
 * Pokemon as the offer, in that order. Also the trade screen's guide text. App thread only: this
 * reads the bulk file (gen3_edit_load) and is never called from the link ISR. Pure C, no heap. */

/* The GBA player as an OT (D7): name[0..6] raw, TID/SID from trainer_id's low/high halves,
 * gender 0 or 1 (any non-zero gender is 1). */
void gen3_trainer_from_link_player(const Gen3LinkPlayer* lp, Gen3Trainer* out);

/* Why the latest received Pokemon isn't in the editor (O2) */
typedef enum {
    Gen3PickupOk, /* no failed pickup outstanding */
    Gen3PickupNoData, /* gen3_edit_load failed with the bulk file unreadable: retried every poll */
    Gen3PickupBadData, /* the received bytes don't decrypt, or gen3_edit_load refused them: not retried */
} Gen3PickupFail;

typedef struct Gen3TradeSync {
    uint32_t trades_seen; /* trades whose Pokemon the editor has taken (or failed to, O2) */
    bool trainer_known;
    Gen3Trainer trainer; /* last trainer handed to the editor */
    bool offer_valid; /* offer holds the editor's Pokemon, game form; false: offer nothing */
    uint8_t offer[GEN3_PARTY_SIZE]; /* zeros unless offer_valid */
    /* Why the editor's Pokemon can't be offered (gen3_trade_offer_check), or, when offer_valid,
     * why the GBA's own rules refuse it (gen3_partner_refuse: an FRLG GBA); None if neither */
    Gen3Refuse offer_refuse;
    bool taken; /* the session took an offer: */
    bool taken_valid; /* the (offer_valid, offer) it holds */
    uint8_t taken_offer[GEN3_PARTY_SIZE];
    Gen3Pkm scratch; /* the received Pokemon, decrypted */
    Gen3PickupFail pickup_fail;
    /* After a failed pickup the editor still holds the Pokemon traded away (O2): its PID and OT
     * ID. It isn't offered again while the editor's PID and OT ID are these. */
    bool gone_valid;
    uint32_t gone_pid;
    uint32_t gone_ot_id;
    uint8_t offer_traits; /* gen3_offer_traits(&ed->pkm) when offer_valid, else 0 */
} Gen3TradeSync;

void gen3_trade_sync_init(Gen3TradeSync* sync);

/* Every session stop or start: as gen3_trade_sync_init, for a fresh club (trades_seen 0, no offer
 * taken, no trainer known), but the block on the traded-away Pokemon stays (O2: it is never
 * offered again, also across sessions): gone_valid, gone_pid, gone_ot_id and a BadData
 * pickup_fail. A NoData retry still waiting becomes BadData: its Pokemon went with the old club.
 * Without a block, the same bytes as gen3_trade_sync_init. */
void gen3_trade_sync_restart(Gen3TradeSync* sync);

/* gen3_trade_sync_poll's result bits */
enum {
    Gen3SyncPickedUp = 1u << 0, /* the received Pokemon is now the editor's */
    Gen3SyncPickupBad = 1u << 1, /* it couldn't be taken: pickup_fail says why */
    Gen3SyncTrainer = 1u << 2, /* the editor got a new GBA player (gen3_edit_set_trainer) */
    Gen3SyncOffer = 1u << 3, /* publish (offer_valid ? offer : no offer), then offer_taken */
};

/* App thread, between UI events. received/received_count come from one
 * gen3_session_get_received. editing (the current scene can change the Pokemon: any scene but
 * Gen3Trade, Gen3Menu and the dialogs over them that don't edit, Gen3TradeEnd, Gen3NatDex and
 * Gen3LinkExit): return 0 and do nothing (D6). Else, in order:
 * 1. Pickup, on a new trade (received_count > trades_seen) or to retry a NoData pickup.
 *    A new trade first notes the editor's PID and OT ID (gone_*) and sets trades_seen =
 *    received_count, whatever follows. Then gen3_pkm_decrypt and gen3_edit_load: PickedUp, with
 *    pickup_fail Ok; or PickupBad with the editor unchanged, pickup_fail NoData if the decrypt
 *    worked and the bulk file isn't readable after the load, else BadData.
 *    received_count < trades_seen is a club reset: trades_seen = received_count, no pickup, and a
 *    waiting NoData retry becomes BadData (its Pokemon went with the old club).
 * 2. st->trainer_valid and not the known trainer: gen3_edit_set_trainer, Trainer. While gone_valid,
 *    a set_trainer that changes the editor's PID or OT ID moves gone_* with it.
 * 3. A BadData pickup is cleared once the editor's PID or OT ID differs from gone_*. Then
 *    offer_refuse = gen3_trade_offer_check(&ed->pkm), offer_valid = offer_refuse None and no
 *    failed pickup (O2: the Pokemon traded away is never offered again), offer = the game form
 *    when valid (only a Pokemon that passes the check is ever published: the club doesn't check,
 *    as it may not decrypt in a PRIMASK section). When valid, offer_traits =
 *    gen3_offer_traits(&ed->pkm), then offer_refuse = gen3_partner_refuse(&st->partner_rules,
 *    offer_traits); else offer_traits = 0. A valid offer is published even while the GBA's rules
 *    refuse it: the club decides, with its traits and the rules of the GBA linked at that moment.
 *    Offer if nothing was taken yet, or (offer_valid, offer) differs from what was taken (the
 *    traits follow from the offer, and a new GBA's rules alone offer nothing). */
unsigned gen3_trade_sync_poll(
    Gen3TradeSync* sync,
    Gen3Editor* ed,
    const Gen3ClubStatus* st,
    const uint8_t received[GEN3_PARTY_SIZE],
    uint32_t received_count,
    bool editing);
/* The session accepted the offer (or "none"): gen3_session_set_offer_traits returned true. */
void gen3_trade_sync_offer_taken(Gen3TradeSync* sync);

/* ---- Ending a trade session (D1, D9, D10, R6) ---- */

/* The sync has every Pokemon the club received (received_count from gen3_session_get_received): it
 * is trades_seen, and no NoData pickup waits for a retry. A trade session may stop for good only
 * then, since its received Pokemon goes with the club. A BadData pickup is final. Poll first from
 * a scene that doesn't edit, so a finished trade is seen. */
bool gen3_trade_sync_settled(const Gen3TradeSync* sync, uint32_t received_count);

/* R6: the link may stop now. Not in the save window (D10), nor while a trade is pending with the
 * GBA linked. "Linked" here is a phase other than Waiting, or Waiting with relinking (a relink's
 * gap: the GBA opens its next link within about 20 frames, and a stop then is a link error on it).
 * A pending trade whose 0x1144 relink never came (the club's relink wait ran out) may stop: the
 * GBA writes nothing before sync 1 (D1). left and leaving aren't read (left is no "trade
 * resolved"). A Link Test session sets neither flag. */
bool gen3_trade_stop_allowed(const Gen3ClubStatus* st);

/* The app tick's own stop, once the GBA has walked out (left, D9): left, settled
 * (gen3_trade_sync_settled after this tick's poll), no GBA linked (Waiting, not in a relink's
 * gap) and gen3_trade_stop_allowed. left is sticky, and the session can outlive it while a trade's
 * Pokemon waits to be picked up; a GBA that walks back into the Cable Club meanwhile is linked
 * again (the club no longer sits, so it can't trade), and the tick waits until it has gone. */
bool gen3_trade_left_stop(const Gen3ClubStatus* st, bool settled);

/* What ending a trade session takes now. Decided again from a fresh status on every button that
 * ends, never from what a dialog showed when it opened: the link may have moved on since. */
typedef enum {
    Gen3TradeEndStop, /* stop and free it now: R6 allows it, and nothing is left to pick up */
    Gen3TradeEndSaving, /* the GBA is saving (D10): "The GBA is saving", OK only */
    Gen3TradeEndAsk, /* a GBA is linked (or relinking): "End trade?"; End asks it to walk out */
    Gen3TradeEndPickup, /* it may stop, but a NoData pickup waits: Retry or End anyway */
    Gen3TradeEndMenu, /* it has stopped (the GBA left): back to the menu, which frees it */
} Gen3TradeEndAction;
/* End trade, or Back on the Gen III menu. st: the session's status; running: it runs; settled:
 * gen3_trade_sync_settled after a poll. Stop only when gen3_trade_stop_allowed and settled. */
Gen3TradeEndAction gen3_trade_end_action(const Gen3ClubStatus* st, bool running, bool settled);
/* "Stop link?" confirmed (D1), or Retry / End anyway in the Pickup dialog opened from it. As
 * gen3_trade_end_action, but a linked GBA is stopped too where R6 allows it (the leave has
 * stalled); where R6 doesn't (a trade under way with the GBA linked), Ask: the leave waits for
 * the trade. */
Gen3TradeEndAction
    gen3_trade_stop_link_action(const Gen3ClubStatus* st, bool running, bool settled);

/* Back on the trade screen */
typedef enum {
    Gen3TradeBackLocked, /* the save window (D10): nothing happens; the guide says why */
    Gen3TradeBackStopLink, /* a leave asked, and the link may stop (R6): "Stop link?" (D1) */
    Gen3TradeBackLeave, /* the previous scene, with the session still running */
} Gen3TradeBackAction;
/* running: the trade session runs (a stopped one's last status locks nothing) */
Gen3TradeBackAction gen3_trade_back_action(const Gen3ClubStatus* st, bool running);

#define GEN3_GUIDE_LINES    3
#define GEN3_GUIDE_LINE_LEN 22 /* 21 chars + NUL: FontSecondary across 128 px */

/* What is happening and what to do on the GBA. started: the session runs (or ran and ended);
 * cable_ok: the cable reads right; offer_refuse and pickup_fail: the sync's. Unused lines are "".
 * The guide never reads left as "trade resolved", nor refused alone as "no offer" (an R8 DDEE
 * leaves it None). The outcome of a trade whose link was lost in the GBA's save (RolledBack,
 * Assumed) outranks left and leaving, which End trade during that trade brings too. After left,
 * "unplug" only with no GBA linked: a GBA can walk back in while a pickup waits, and is linked
 * then (the Flipper won't sit); unplugged, it would show a link error, so it walks out first. An
 * offer the GBA's own rules refuse (NationalDex, Fateful) has rows of its own, below the pickup
 * rows, shown only while a GBA is linked: the rules outlive the link, so unlinked they may be
 * another cartridge's, and the Waiting rows show instead. Waiting with relinking (a relink's
 * gap) reads as linked: "Linking...", never the Waiting rows. Closing with relinking (the club
 * answered the GBA's 5FFF, and its link hasn't closed yet) keeps "Please wait...", as every
 * Closing does. Written through gen3_trade_guide_copy: false if a text, row or reason, had to
 * be cut to fit (test_guide_line_lengths: never). */
bool gen3_trade_guide(
    const Gen3ClubStatus* st,
    bool started,
    bool cable_ok,
    Gen3Refuse offer_refuse,
    Gen3PickupFail pickup_fail,
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN]);
/* The three lines into out, each cut at GEN3_GUIDE_LINE_LEN - 1 characters and NUL-terminated.
 * false if any was cut. */
bool gen3_trade_guide_copy(
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN],
    const char* l1,
    const char* l2,
    const char* l3);
