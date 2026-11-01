#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <stdio.h>
#include <string.h>

#include <src/gen3/gen3_bulk_furi.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_session_furi.h>
#include <src/gen3/gen3_trade_sync.h>
#include <src/scenes/include/pokemon_gen3_menu.h>
#include <src/views/gen3_trade.h>

/* The Gen III trade screen (Gen3Trade) and the app tick that runs a trade session behind every
 * Gen III scene (D1, D6). The session and the trade view are allocated together by Gen III ->
 * Trade, and freed together by the Gen III menu once no trade session runs. The sync is allocated
 * by the first Trade and kept until the app exits, so the block on a traded-away Pokemon (O2)
 * outlives the session. */

/* Ticks between two tries to reopen the bulk file for a NoData pickup retry (O2): 1 s. A corrupt
 * file costs a full CRC pass per try. */
#define GEN3_TRADE_BULK_RETRY_TICKS 10
/* Ticks between two tries to start the session while the cable reads OK but the start fails
 * (TIM2 busy): 1 s */
#define GEN3_TRADE_START_RETRY_TICKS 10

/* No app stack local over 128 bytes, and a Gen3ClubStatus never is one. These are the tick's
 * buffers. */
static uint8_t gen3_trade_received[GEN3_PARTY_SIZE];
static Gen3TradeViewState gen3_trade_view_state;
/* The status the guide is given: the session's, with last_result latched (below) */
static Gen3ClubStatus gen3_trade_guide_status;

/* A "Link lost ..." row is about the trade that just ended. Once the GBA has linked again after
 * it, the row is stale: the guide gets last_result None from then on, until the next trade is
 * pending, the club's last_result changes, or the session starts again. */
static bool gen3_trade_result_latched;
static Gen3TradeResult gen3_trade_result_latched_value; /* the last_result latched */
static uint8_t gen3_trade_bulk_wait;
static uint8_t gen3_trade_start_wait;

/* The current scene can change the editor's Pokemon: an editor scene, or Load. The sync holds the
 * offer meanwhile (D6), and the club's editing flag stays set (O1). The trade screen, the menu
 * and the dialogs over them that neither change nor show the Pokemon don't edit, so the trade
 * screen stays up to date behind them; Save .pk3 does edit (it holds the sync). */
static bool gen3_trade_editing(PokemonFap* pokemon_fap) {
    switch(scene_manager_get_current_scene(pokemon_fap->scene_manager)) {
    case PokemonSceneGen3Trade:
    case PokemonSceneGen3Menu:
    case PokemonSceneGen3TradeEnd:
    case PokemonSceneGen3NatDex:
    case PokemonSceneGen3LinkExit:
        return false;
    default:
        return true;
    }
}

/* A new club (a start) or none (a stop): the sync and the latch begin again. A restarted club
 * has no offer and trades_done 0, which the sync's reset rule alone doesn't make it publish
 * again. The block on a traded-away Pokemon stays (O2). */
static void gen3_trade_reset(PokemonFap* pokemon_fap) {
    gen3_trade_sync_restart(pokemon_fap->gen3_trade_sync);
    gen3_trade_result_latched = false;
    gen3_trade_bulk_wait = 0;
    gen3_trade_start_wait = 0;
}

static const Gen3ClubStatus* gen3_trade_latch_result(const Gen3ClubStatus* st) {
    /* A new result is never hidden by an older one latched */
    if(st->trade_pending || st->last_result != gen3_trade_result_latched_value) {
        gen3_trade_result_latched = false;
    }
    if(!gen3_trade_result_latched && !st->trade_pending &&
       st->last_result != Gen3TradeResultNone && st->phase != Gen3ClubPhaseWaiting) {
        gen3_trade_result_latched = true;
        gen3_trade_result_latched_value = st->last_result;
    }
    gen3_trade_guide_status = *st;
    if(gen3_trade_result_latched) gen3_trade_guide_status.last_result = Gen3TradeResultNone;
    return &gen3_trade_guide_status;
}

void gen3_trade_tick(PokemonFap* pokemon_fap) {
    Gen3Session* s = pokemon_fap->gen3_session;
    Gen3TradeSync* sync = pokemon_fap->gen3_trade_sync;
    Gen3Editor* ed = pokemon_fap->gen3_editor;

    if(!pokemon_fap->gen3_trade_view || !sync || !ed || !s) return;

    /* 1. Start, once the cable reads OK; after a failed start, at most once a second */
    bool trading = gen3_session_trading(s);
    bool cable_ok = true;
    if(!trading && !pokemon_fap->gen3_trade_ended) {
        cable_ok = gen3_session_check_cable(s) == MlinkCableOk;
        if(cable_ok && gen3_trade_start_wait == 0) {
            if(gen3_session_start_trade(s, pokemon_fap->gen3_national_dex)) {
                gen3_trade_reset(pokemon_fap);
                trading = true;
            } else {
                gen3_trade_start_wait = GEN3_TRADE_START_RETRY_TICKS;
            }
        }
        if(gen3_trade_start_wait > 0) gen3_trade_start_wait--;
    }

    /* 2. Read: cable_ok is the driver's live guard while the link runs */
    const Gen3ClubStatus* st = gen3_menu_status(pokemon_fap, NULL, trading ? &cable_ok : NULL);
    uint32_t received_count = 0;
    gen3_session_get_received(s, gen3_trade_received, &received_count);

    /* 3. Sync (D6): only between edits */
    if(trading) {
        const bool editing = gen3_trade_editing(pokemon_fap);

        /* O2: a NoData pickup can only succeed once the bulk file reads again */
        if(!editing && sync->pickup_fail == Gen3PickupNoData && !gen3_bulk_ready()) {
            if(gen3_trade_bulk_wait == 0) {
                gen3_bulk_furi_open();
                gen3_trade_bulk_wait = GEN3_TRADE_BULK_RETRY_TICKS;
            }
            gen3_trade_bulk_wait--;
        }

        const unsigned r =
            gen3_trade_sync_poll(sync, ed, st, gen3_trade_received, received_count, editing);
        /* The offer goes with the traits the poll computed from the editor's decrypted
         * Pokemon: the club may not decrypt in the hand-off's PRIMASK section */
        if((r & Gen3SyncOffer) &&
           gen3_session_set_offer_traits(
               s, sync->offer_valid ? sync->offer : NULL, sync->trades_seen, sync->offer_traits)) {
            gen3_trade_sync_offer_taken(sync);
        }
        /* O1: back from editing. After the offer above, so the club sees the new offer before
         * it may sit again. */
        if(!editing && st->editing) gen3_session_set_editing(s, false, 0);
    }

    /* 4. End: the GBA has walked out and closed the link (D9). left is sticky and says nothing
     * about a trade: it only ends the session, and only once the sync has every trade's Pokemon
     * and no GBA is linked (gen3_trade_left_stop). Save .pk3, Gen3Result and the editor's
     * dialogs hold the sync without the club's editing flag, so a trade can finish there;
     * stopping now would free the club with its received Pokemon, which the GBA has saved as
     * traded. That Pokemon is picked up on the first tick back on a scene that doesn't edit,
     * and the session stops then. Meanwhile the guide shows "Link ended" (or the outcome of a
     * trade lost in the GBA's save). A NoData pickup keeps it up: End trade offers Retry / End
     * anyway. A GBA that walks back in meanwhile can't trade (the club no longer sits), and
     * keeps it up until it has gone again: a stop in its link save would break R6. */
    if(trading && gen3_trade_left_stop(st, gen3_trade_sync_settled(sync, received_count))) {
        gen3_session_stop(s); /* the last status stays readable */
        gen3_trade_reset(pokemon_fap);
        pokemon_fap->gen3_trade_ended = true;
        trading = false;
        /* End trade goes from the menu shown, and Link Test comes back */
        if(scene_manager_get_current_scene(pokemon_fap->scene_manager) == PokemonSceneGen3Menu) {
            gen3_menu_rebuild(pokemon_fap);
        }
    }

    /* 5. Show */
    const bool ended = pokemon_fap->gen3_trade_ended;
    Gen3TradeViewState* view_state = &gen3_trade_view_state;
    char nickname[GEN3_NICKNAME_LEN + 1];

    if(ended) cable_ok = true;
    gen3_trade_guide(
        gen3_trade_latch_result(st),
        trading || ended,
        cable_ok,
        sync->offer_refuse,
        sync->pickup_fail,
        view_state->guide);
    gen3_pkm_nickname(&ed->pkm, nickname, sizeof(nickname));
    snprintf(view_state->flipper_line, sizeof(view_state->flipper_line), "Flipper: %s", nickname);
    view_state->errors = st->link.checksum_errors;
    view_state->can_edit = trading && !st->trade_pending && !st->save_window && !st->leaving &&
                           !st->left;
    gen3_trade_view_update(pokemon_fap->gen3_trade_view, view_state);
}

bool gen3_trade_begin_edit(PokemonFap* pokemon_fap) {
    Gen3Session* s = pokemon_fap->gen3_session;
    Gen3TradeSync* sync = pokemon_fap->gen3_trade_sync;

    if(!gen3_session_trading(s) || !sync) return true;

    /* A trade that just finished is picked up first, so the edit starts from its Pokemon */
    gen3_trade_tick(pokemon_fap);
    if(sync->pickup_fail == Gen3PickupNoData) return false;
    /* Not running any more (the tick ended it): true */
    return gen3_session_set_editing(s, true, sync->trades_seen);
}

/* A running trade session gets one tick first. From a scene that doesn't edit, its poll picks up a
 * finished trade's Pokemon (and the tick may end the session itself, once the GBA has left). Then
 * *st gets the status, and only after it the received count is read: a trade that ends after that
 * status (its link lost after the sync-2 answer: Assumed, at Waiting) is in the count, so the
 * status never shows Waiting with its Pokemon unseen. Returns gen3_trade_sync_settled, or true
 * without a running trade session. */
static bool gen3_trade_settled(PokemonFap* pokemon_fap, const Gen3ClubStatus** st) {
    Gen3Session* s = pokemon_fap->gen3_session;
    Gen3TradeSync* sync = pokemon_fap->gen3_trade_sync;
    uint32_t received_count = 0;

    if(gen3_session_trading(s) && sync) gen3_trade_tick(pokemon_fap);
    *st = gen3_menu_status(pokemon_fap, NULL, NULL);
    if(!gen3_session_trading(s) || !sync) return true; /* the tick may have ended it, all seen */
    gen3_session_get_received(s, gen3_trade_received, &received_count);
    return gen3_trade_sync_settled(sync, received_count);
}

Gen3TradeEndAction gen3_trade_end_decide(PokemonFap* pokemon_fap, bool stop_link) {
    const Gen3ClubStatus* st;
    const bool settled = gen3_trade_settled(pokemon_fap, &st);
    /* After the tick: a session it stopped reads as not running (Menu) */
    const bool running = gen3_session_running(pokemon_fap->gen3_session);

    return stop_link ? gen3_trade_stop_link_action(st, running, settled) :
                       gen3_trade_end_action(st, running, settled);
}

void pokemon_scene_gen3_trade_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;

    /* Never switch to a trade view that is gone (the menu freed it with its session): a crash.
     * The menu instead */
    if(!pokemon_fap->gen3_trade_view || !pokemon_fap->gen3_session) {
        const bool found = scene_manager_search_and_switch_to_previous_scene(
            pokemon_fap->scene_manager, PokemonSceneGen3Menu);
        /* NOTE: Gen3Trade is only ever opened over the Gen III menu */
        furi_check(found);
        return;
    }

    gen3_trade_tick(pokemon_fap);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewGen3Trade);
}

bool pokemon_scene_gen3_trade_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    if(event.type == SceneManagerEventTypeBack && pokemon_fap->gen3_session) {
        const Gen3ClubStatus* st = gen3_menu_status(pokemon_fap, NULL, NULL);
        switch(gen3_trade_back_action(st, gen3_session_trading(pokemon_fap->gen3_session))) {
        case Gen3TradeBackLocked:
            /* D10: the GBA is saving; the guide already says Back is locked */
            return true;
        case Gen3TradeBackStopLink:
            /* D1: a leave that may have stalled can still be stopped (plan 1's "Stop link?") */
            scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3LinkExit);
            return true;
        case Gen3TradeBackLeave:
            break;
        }
    }

    /* Back otherwise is left to the scene manager: the previous scene, with the session still
     * running */
    if(event.type != SceneManagerEventTypeCustom || event.event != GEN3_TRADE_EVENT_EDIT) {
        return false;
    }

    /* The view offers Edit only when it may be used, but the status may have moved on since */
    const Gen3ClubStatus* st = gen3_menu_status(pokemon_fap, NULL, NULL);
    if(st->save_window || st->leaving || st->left) return true;

    if(!gen3_menu_editor_ready(pokemon_fap)) {
        gen3_menu_show_data_missing(pokemon_fap);
    } else if(gen3_trade_begin_edit(pokemon_fap)) {
        /* Same handler as begin_edit: no tick runs before the editor is the current scene */
        scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3Edit);
    }
    /* else: stay; the guide shows why (a pending trade, or a pickup being retried) */
    return true;
}

void pokemon_scene_gen3_trade_on_exit(void* context) {
    UNUSED(context);
}
