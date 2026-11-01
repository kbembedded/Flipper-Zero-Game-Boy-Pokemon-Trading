#include <src/gen3/gen3_trade_sync.h>

#include <string.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_partner.h>

void gen3_trainer_from_link_player(const Gen3LinkPlayer* lp, Gen3Trainer* out) {
    memset(out, 0, sizeof(*out));
    memcpy(out->name, lp->name, GEN3_OT_NAME_LEN);
    out->tid = (uint16_t)(lp->trainer_id & 0xFFFF);
    out->sid = (uint16_t)(lp->trainer_id >> 16);
    out->gender = lp->gender != 0;
}

/* Field by field: no padding compared */
static bool trainer_eq(const Gen3Trainer* a, const Gen3Trainer* b) {
    return memcmp(a->name, b->name, GEN3_OT_NAME_LEN) == 0 && a->tid == b->tid &&
           a->sid == b->sid && a->gender == b->gender;
}

void gen3_trade_sync_init(Gen3TradeSync* sync) {
    memset(sync, 0, sizeof(*sync));
    sync->offer_refuse = Gen3RefuseNone;
    sync->pickup_fail = Gen3PickupOk;
}

void gen3_trade_sync_restart(Gen3TradeSync* sync) {
    /* The block: a failed pickup, which always comes with gone_valid (only a successful pickup
     * or the user's change clears either) */
    const bool blocked = sync->pickup_fail != Gen3PickupOk && sync->gone_valid;
    const uint32_t gone_pid = sync->gone_pid;
    const uint32_t gone_ot_id = sync->gone_ot_id;
    gen3_trade_sync_init(sync);
    if(blocked) {
        sync->pickup_fail = Gen3PickupBadData; /* a NoData retry's Pokemon went with the club */
        sync->gone_valid = true;
        sync->gone_pid = gone_pid;
        sync->gone_ot_id = gone_ot_id;
    }
}

/* Step 1 (O2): take the latest trade's Pokemon into the editor, or retry a NoData pickup.
 * The club keeps the received bytes, and no new trade can happen while the offer is unpublished,
 * so a retry reads the same Pokemon. */
static unsigned sync_pickup(
    Gen3TradeSync* sync,
    Gen3Editor* ed,
    const uint8_t received[GEN3_PARTY_SIZE],
    uint32_t received_count) {
    if(received_count < sync->trades_seen) {
        /* The club was reset (trades_done back to 0): no trade to pick up. A NoData retry's
         * Pokemon went with the old club, so nothing is retried and the block stays (BadData):
         * the editor still holds the Pokemon traded away. */
        sync->trades_seen = received_count;
        if(sync->pickup_fail == Gen3PickupNoData) sync->pickup_fail = Gen3PickupBadData;
        return 0;
    }
    if(received_count != sync->trades_seen) {
        /* A new trade: the editor's Pokemon is the GBA's now */
        sync->gone_pid = gen3_pkm_get(&ed->pkm, Gen3FieldPid);
        sync->gone_ot_id = gen3_pkm_get(&ed->pkm, Gen3FieldOtId);
        sync->gone_valid = true;
        sync->trades_seen = received_count;
    } else if(sync->pickup_fail != Gen3PickupNoData) {
        return 0;
    }
    /* gen3_edit_load normalises. That recalculates the party stats, which Ruby leaves behind the
     * EVs after a battle (MonGainEVs doesn't recalculate them), so the Pokemon published below
     * can differ from the received bytes the club offers after the trade. The poll publishes it
     * at once, on the first tick after trades_done advances, normally before the post-save
     * exchange's round 1; if round 1 comes first, the club sees a changed offer: one EEBB and
     * one re-sit. Normalisation is never skipped to avoid that. */
    bool decrypted = gen3_pkm_decrypt(received, GEN3_PARTY_SIZE, &sync->scratch);
    if(decrypted && gen3_edit_load(ed, &sync->scratch)) {
        sync->pickup_fail = Gen3PickupOk;
        sync->gone_valid = false;
        return Gen3SyncPickedUp;
    }
    sync->pickup_fail = decrypted && !gen3_bulk_ready() ? Gen3PickupNoData : Gen3PickupBadData;
    return Gen3SyncPickupBad;
}

/* Step 2 (D7) */
static unsigned sync_trainer(Gen3TradeSync* sync, Gen3Editor* ed, const Gen3ClubStatus* st) {
    if(!st->trainer_valid) return 0;
    Gen3Trainer t;
    gen3_trainer_from_link_player(&st->trainer, &t);
    if(sync->trainer_known && trainer_eq(&t, &sync->trainer)) return 0;
    const uint32_t pid = gen3_pkm_get(&ed->pkm, Gen3FieldPid);
    const uint32_t ot_id = gen3_pkm_get(&ed->pkm, Gen3FieldOtId);
    gen3_edit_set_trainer(ed, &t);
    /* O2: the block follows a trainer swap. Only a pristine editor changes here, and while
     * gone_valid that is the default traded away, now under this player's OT: still blocked, to
     * this player and to the one it went to. */
    if(sync->gone_valid && (gen3_pkm_get(&ed->pkm, Gen3FieldPid) != pid ||
                            gen3_pkm_get(&ed->pkm, Gen3FieldOtId) != ot_id)) {
        sync->gone_pid = gen3_pkm_get(&ed->pkm, Gen3FieldPid);
        sync->gone_ot_id = gen3_pkm_get(&ed->pkm, Gen3FieldOtId);
    }
    sync->trainer = t;
    sync->trainer_known = true;
    return Gen3SyncTrainer;
}

/* Step 3 (D8, O2): only a Pokemon that passes gen3_trade_offer_check is ever offered. A valid
 * one goes out with its traits even while the GBA's own rules refuse it (st->partner_rules): the
 * club applies the rules of the GBA linked when it shows the offer, which may be a later one. */
static unsigned sync_offer(Gen3TradeSync* sync, const Gen3Editor* ed, const Gen3ClubStatus* st) {
    /* The user loaded or made another Pokemon: the traded-away one is gone from the editor */
    if(sync->pickup_fail == Gen3PickupBadData && sync->gone_valid &&
       (gen3_pkm_get(&ed->pkm, Gen3FieldPid) != sync->gone_pid ||
        gen3_pkm_get(&ed->pkm, Gen3FieldOtId) != sync->gone_ot_id)) {
        sync->pickup_fail = Gen3PickupOk;
        sync->gone_valid = false;
    }
    sync->offer_refuse = gen3_trade_offer_check(&ed->pkm);
    sync->offer_valid = sync->offer_refuse == Gen3RefuseNone && sync->pickup_fail == Gen3PickupOk;
    sync->offer_traits = 0;
    if(sync->offer_valid) {
        sync->offer_traits = gen3_offer_traits(&ed->pkm);
        sync->offer_refuse = gen3_partner_refuse(&st->partner_rules, sync->offer_traits);
        gen3_pkm_encrypt(&ed->pkm, sync->offer);
    } else {
        memset(sync->offer, 0, sizeof(sync->offer));
    }
    /* offer is zeros without a valid one, so one compare covers both */
    if(sync->taken && sync->taken_valid == sync->offer_valid &&
       memcmp(sync->taken_offer, sync->offer, sizeof(sync->offer)) == 0)
        return 0;
    return Gen3SyncOffer;
}

unsigned gen3_trade_sync_poll(
    Gen3TradeSync* sync,
    Gen3Editor* ed,
    const Gen3ClubStatus* st,
    const uint8_t received[GEN3_PARTY_SIZE],
    uint32_t received_count,
    bool editing) {
    if(editing) return 0;
    unsigned result = sync_pickup(sync, ed, received, received_count);
    result |= sync_trainer(sync, ed, st);
    result |= sync_offer(sync, ed, st);
    return result;
}

void gen3_trade_sync_offer_taken(Gen3TradeSync* sync) {
    sync->taken = true;
    sync->taken_valid = sync->offer_valid;
    memcpy(sync->taken_offer, sync->offer, sizeof(sync->taken_offer));
}

bool gen3_trade_sync_settled(const Gen3TradeSync* sync, uint32_t received_count) {
    return received_count == sync->trades_seen && sync->pickup_fail != Gen3PickupNoData;
}

/* A GBA is linked, or in a relink's gap: stopping now would drop it mid-relink, a link error on
 * the GBA. Waiting without relinking is "no GBA linked" (D1). */
static bool gba_linked(const Gen3ClubStatus* st) {
    return st->phase != Gen3ClubPhaseWaiting || st->relinking;
}

bool gen3_trade_stop_allowed(const Gen3ClubStatus* st) {
    if(st->save_window) return false;
    return !(st->trade_pending && gba_linked(st));
}

bool gen3_trade_left_stop(const Gen3ClubStatus* st, bool settled) {
    return st->left && settled && !gba_linked(st) && gen3_trade_stop_allowed(st);
}

Gen3TradeEndAction gen3_trade_end_action(const Gen3ClubStatus* st, bool running, bool settled) {
    if(!running) return Gen3TradeEndMenu;
    if(st->save_window) return Gen3TradeEndSaving;
    if(gba_linked(st)) return Gen3TradeEndAsk;
    /* No GBA linked, so R6 allows the stop (a pending trade here lost its relink before sync 1) */
    return settled ? Gen3TradeEndStop : Gen3TradeEndPickup;
}

Gen3TradeEndAction
    gen3_trade_stop_link_action(const Gen3ClubStatus* st, bool running, bool settled) {
    if(!running) return Gen3TradeEndMenu;
    if(st->save_window) return Gen3TradeEndSaving;
    if(!gen3_trade_stop_allowed(st)) return Gen3TradeEndAsk;
    return settled ? Gen3TradeEndStop : Gen3TradeEndPickup;
}

Gen3TradeBackAction gen3_trade_back_action(const Gen3ClubStatus* st, bool running) {
    if(!running) return Gen3TradeBackLeave;
    if(st->save_window) return Gen3TradeBackLocked;
    if(st->leaving && gen3_trade_stop_allowed(st)) return Gen3TradeBackStopLink;
    return Gen3TradeBackLeave;
}

/* The reason line of a refusal (the Flipper's offer, or the GBA's pick) */
static const char* guide_reason(Gen3Refuse r) {
    switch(r) {
    case Gen3RefuseEmpty:
        return "that slot is empty.";
    case Gen3RefuseBadEgg:
        return "it's a Bad Egg.";
    case Gen3RefuseEgg:
        return "it's an Egg.";
    case Gen3RefuseMail:
        return "it holds Mail.";
    case Gen3RefuseItem:
        return "it holds a bad item.";
    case Gen3RefuseNationalDex:
        return "National Dex needed.";
    case Gen3RefuseFateful:
        return "it's not fateful.";
    case Gen3RefuseNone: /* never asked for */
    case Gen3RefuseBadData:
        break;
    }
    return "its data is invalid.";
}

/* Which Pokemon Center 2F counter is the Trade Center, for the game whose LinkPlayerBlock parsed:
 * - Ruby and Sapphire: the middle one. Every pokeruby <Town>_PokemonCenter_2F map has three
 *   attendants: x = 4 the COLOSSEUM (data/scripts/cable_club.inc: <Town>_..._EventScript_1A3EA6),
 *   x = 7 the TRADE CENTER (EventScript_1A40CC, which links with sub_80834E4 [TryTradeLinkup])
 *   and x = 10 the RECORD CORNER (EventScript_1A41FB).
 * - FireRed and LeafGreen: the right-hand attendant (x = 10, fr: data/scripts/cable_club.inc:
 *   CableClub_EventScript_DirectCornerAttendant), which with no wireless adapter offers TRADE
 *   CENTER and COLOSSEUM (CableClub_EventScript_WelcomeToCableClub); x = 2 and x = 6 are the
 *   wireless club and the Union Room.
 * - Emerald: as FireRed. pokeemerald's PokemonCenter_2F maps place the same three attendants,
 *   and its CableClub_EventScript_DirectCornerAttendant falls back to a menu that also adds
 *   RECORD CORNER after Mauville, with TRADE CENTER still on the right.
 * - Unknown: as R/S. */
static bool trade_center_right(Gen3LinkGame game) {
    switch(game) {
    case Gen3LinkGameE:
    case Gen3LinkGameFRLG:
        return true;
    case Gen3LinkGameRS:
    case Gen3LinkGameUnknown:
        break;
    }
    return false;
}

/* SAY sets the three lines and returns: the first row that matches wins */
#define SAY(a, b, c) \
    do { \
        l[0] = (a); \
        l[1] = (b); \
        l[2] = (c); \
        return; \
    } while(0)

static void guide_pick(
    const Gen3ClubStatus* st,
    bool started,
    bool cable_ok,
    Gen3Refuse offer_refuse,
    Gen3PickupFail pickup_fail,
    const char* l[GEN3_GUIDE_LINES]) {
    if(!cable_ok) SAY("Plug in the cable", "(or swap its ends).", "");
    if(!started) SAY("Starting...", "", "");
    if(st->save_window) SAY("The GBA is saving.", "Don't unplug or turn", "off. Back is locked.");
    /* The latest trade reached the GBA's save and lost its link there (D5): its outcome, in the
     * Waiting rows below, outranks the leave's rows, which End trade during that trade brings
     * too. Not an older trade's: none pending since, and no GBA linked again (not even a relink's
     * gap) */
    const bool lost_in_save = st->phase == Gen3ClubPhaseWaiting && !st->relinking &&
                              !st->trade_pending &&
                              (st->last_result == Gen3TradeResultRolledBack ||
                               st->last_result == Gen3TradeResultAssumed);
    /* left can come with a trade still pending (a 0x1144 relink that never came, before sync 1):
     * it says nothing about the trade. A GBA linked after it walked back in while a pickup waits:
     * the Flipper won't sit, and unplugging it now would be a link error on it */
    if(st->left && !lost_in_save) {
        if(gba_linked(st)) SAY("Walk out on the GBA,", "then unplug.", "");
        SAY("Link ended. You can", "unplug the GBA.", "");
    }
    /* Ahead of the menu rows: no "offer changed" hint while leaving */
    if(st->leaving && !lost_in_save)
        SAY("Ending: the GBA walks", "out of the Trade", "Center by itself.");
    if(st->trade_pending) {
        /* No GBA linked, not even in a relink's gap: the trade's 0x1144 link didn't come within
         * the relink wait (a GBA turned off in the gap), or closed before the GBA's block parsed
         * (FRLG's own 300-frame deadline on it, fr: trade_scene.c: CB2_LinkTrade, then a link
         * error on the GBA). No sync was answered, so nothing reached the GBA's save. The trade
         * stays pending until the GBA's next block: a late 0x1144 relink still trades, any other
         * link cancels it */
        if(!gba_linked(st))
            SAY("Link lost before the", "trade. On a GBA link", "error, restart it.");
        SAY("Trading...", "Watch the GBA.", "");
    }
    if(st->phase == Gen3ClubPhaseError) {
        /* The GBA's block parsed, so its rules (partner_rules) are this GBA's: name its game's
         * counter, not the one the player just used */
        if(st->last_error == Gen3ClubErrorOtherRoom) {
            if(trade_center_right(st->partner_rules.game))
                SAY("The GBA chose another", "room. Right counter,", "then TRADE CENTER.");
            SAY("The GBA chose another", "room. Use the middle", "counter: Trade Center");
        }
        SAY("Link error. Restart", "the GBA, then go to", "the Cable Club.");
    }
    /* No valid offer (R5, O2): the club doesn't sit, and in the menu it sends EEBB */
    if(pickup_fail == Gen3PickupNoData)
        SAY("Can't take the traded", "Pokemon: data file", "unreadable. Retrying.");
    if(pickup_fail == Gen3PickupBadData)
        SAY("Can't take the traded", "Pokemon: bad data.", "Load or pick another.");
    /* The GBA's own rules refuse the offer (an FRLG GBA): the club neither sits nor shows it. Only
     * while a GBA is linked: the rules outlive the link, so unlinked they may be the last
     * cartridge's, while the user walks another one to the counter (the Waiting rows instead) */
    if(offer_refuse == Gen3RefuseNationalDex || offer_refuse == Gen3RefuseFateful) {
        if(gba_linked(st)) {
            if(offer_refuse == Gen3RefuseNationalDex)
                SAY("Can't offer it: GBA", "has no National Dex.", "Get it, or pick 1-151");
            SAY("Can't offer it: FRLG", "needs Fateful: Yes on", "Mew/Deoxys: edit it.");
        }
    } else if(offer_refuse != Gen3RefuseNone) {
        /* R5: the Flipper sits as soon as there is a valid offer */
        if(st->phase == Gen3ClubPhaseRoom && st->gba_seated)
            SAY("Can't offer it:", guide_reason(offer_refuse), "Edit it; stay seated.");
        SAY("Can't offer it:", guide_reason(offer_refuse), "Edit it first.");
    }
    switch(st->phase) {
    case Gen3ClubPhaseWaiting:
        /* A relink's gap (about 20 frames) is linked, as for ending (gba_linked): the GBA opens
         * its next link at once, so no Waiting row flashes up */
        if(st->relinking) SAY("Linking...", "", "");
        if(st->last_result == Gen3TradeResultRolledBack)
            SAY("Link lost while the", "GBA saved. It keeps", "its old save.");
        if(st->last_result == Gen3TradeResultAssumed)
            SAY("Link lost at the end.", "Check the GBA party:", "it likely traded.");
        if(st->last_result == Gen3TradeResultCanceled)
            SAY("Link lost. Nothing", "was traded.", "");
        /* No GBA linked: the rules are the last GBA's, and the next GBA may be another
         * cartridge, swapped meanwhile. So both games' counters */
        SAY("Go to the Cable Club:", "PC 2F, Trade Center:", "RS middle, FRLG right");
    case Gen3ClubPhaseLinked:
    case Gen3ClubPhasePlayers:
    case Gen3ClubPhaseCards:
        SAY("Linking...", "", "");
    case Gen3ClubPhaseRoom:
        /* The hint only while the GBA isn't seated: a seated GBA is past it */
        if(st->resit_needed && !st->gba_seated)
            SAY("Sit at the table", "again (step off and", "back on if seated).");
        if(st->gba_seated) SAY("Opening the trade", "menu...", "");
        SAY("In the Trade Center:", "sit at the table on", "the GBA.");
    case Gen3ClubPhaseExchange:
        SAY("Opening the trade", "menu...", "");
    case Gen3ClubPhaseMenu:
        if(st->offer_stale || st->menu == Gen3ClubMenuLeaving)
            SAY("Updating the trade", "menu: the GBA goes", "back to the room.");
        /* An R8 DDEE leaves refused at None: no reason is shown for it */
        if(st->refused != Gen3RefuseNone)
            SAY("Can't take that one:", guide_reason(st->refused), "Pick another one.");
        if(st->menu == Gen3ClubMenuOffered) SAY("Confirm the trade", "on the GBA.", "");
        if(st->trades_done > 0) SAY("Edit it (OK), or pick", "the next trade on", "the GBA.");
        SAY("Pick a Pokemon to", "trade on the GBA.", "");
    case Gen3ClubPhaseClosing:
    case Gen3ClubPhaseError:
    case Gen3ClubPhaseTrade:
    case Gen3ClubPhaseSave:
        break;
    }
    SAY("Please wait...", "", "");
}

#undef SAY

bool gen3_trade_guide_copy(
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN],
    const char* l1,
    const char* l2,
    const char* l3) {
    const char* const l[GEN3_GUIDE_LINES] = {l1, l2, l3};
    bool whole = true;
    for(int i = 0; i < GEN3_GUIDE_LINES; i++) {
        size_t n = 0;
        for(; l[i][n] != '\0' && n < GEN3_GUIDE_LINE_LEN - 1; n++) out[i][n] = l[i][n];
        out[i][n] = '\0';
        if(l[i][n] != '\0') whole = false; /* cut, never overrun */
    }
    return whole;
}

bool gen3_trade_guide(
    const Gen3ClubStatus* st,
    bool started,
    bool cable_ok,
    Gen3Refuse offer_refuse,
    Gen3PickupFail pickup_fail,
    char out[GEN3_GUIDE_LINES][GEN3_GUIDE_LINE_LEN]) {
    const char* l[GEN3_GUIDE_LINES];
    guide_pick(st, started, cable_ok, offer_refuse, pickup_fail, l);
    return gen3_trade_guide_copy(out, l[0], l[1], l[2]);
}
