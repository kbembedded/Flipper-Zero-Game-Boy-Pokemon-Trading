#include <gui/modules/dialog_ex.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_bulk_furi.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_session_furi.h>
#include <src/gen3/gen3_trade_sync.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* End trade (D9, D10), over the Gen III menu. The scene state is what it shows (a
 * Gen3TradeEndAction: Saving, Ask or Pickup), plus GEN3_TRADE_END_STOP_LINK if it was opened
 * from a confirmed "Stop link?".
 * - Saving: the GBA is saving; OK returns.
 * - Ask: Stay returns; End ends the trade.
 * - Pickup: a received Pokemon waits for the SD card. Retry (right) tries the card again and,
 *   once it is picked up, goes on with the end; End anyway (left) ends it, losing that Pokemon.
 *   Retry sits where End and "Stop link?"'s Stop are, so pressing the right button again never
 *   loses the Pokemon.
 * Back is Stay (or OK), with the session still running.
 *
 * The session runs on behind the dialog, and the tick polls here (the dialog doesn't edit), so
 * what it shows can go stale: the GBA saves, or walks out and the tick stops the session. So
 * every button that ends decides again from a fresh status and does what that says, never what
 * the dialog showed: stop and free, ask the GBA to walk out, show the saving or pickup dialog, or,
 * once the session has stopped, just the menu (which frees it). */

#define GEN3_TRADE_END_STOP_LINK 0x100u
#define GEN3_TRADE_END_ACTION    0xFFu

static void gen3_trade_end_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, result);
}

void pokemon_scene_gen3_trade_end_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;
    const uint32_t action =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3TradeEnd) &
        GEN3_TRADE_END_ACTION;

    dialog_ex_reset(dialog_ex);
    if(action == Gen3TradeEndSaving) {
        dialog_ex_set_header(dialog_ex, "The GBA is saving", 64, 0, AlignCenter, AlignTop);
        dialog_ex_set_text(
            dialog_ex,
            "Wait until it's done,\nthen end the trade.",
            64,
            12,
            AlignCenter,
            AlignTop);
        dialog_ex_set_center_button_text(dialog_ex, "OK");
    } else if(action == Gen3TradeEndPickup) {
        dialog_ex_set_header(dialog_ex, "End trade?", 64, 0, AlignCenter, AlignTop);
        dialog_ex_set_text(
            dialog_ex,
            "A traded Pokemon\ncouldn't be loaded.\nReinsert the SD card.",
            64,
            12,
            AlignCenter,
            AlignTop);
        dialog_ex_set_left_button_text(dialog_ex, "End anyway");
        dialog_ex_set_right_button_text(dialog_ex, "Retry");
    } else {
        dialog_ex_set_header(dialog_ex, "End trade?", 64, 0, AlignCenter, AlignTop);
        dialog_ex_set_text(
            dialog_ex,
            "The GBA walks out of\nthe Trade Center by\nitself, then the link\nstops.",
            64,
            12,
            AlignCenter,
            AlignTop);
        dialog_ex_set_left_button_text(dialog_ex, "Stay");
        dialog_ex_set_right_button_text(dialog_ex, "End");
    }
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_trade_end_dialog_callback);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

void gen3_trade_end_open(PokemonFap* pokemon_fap, Gen3TradeEndAction action, bool stop_link) {
    scene_manager_set_scene_state(
        pokemon_fap->scene_manager,
        PokemonSceneGen3TradeEnd,
        (uint32_t)action | (stop_link ? GEN3_TRADE_END_STOP_LINK : 0));
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3TradeEnd);
}

/* End, Retry or End anyway: decide again now and do that. end_anyway: the user chose to lose a
 * Pokemon still waiting for the SD card. */
static void gen3_trade_end_go_on(PokemonFap* pokemon_fap, bool end_anyway) {
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    const uint32_t stop_link =
        scene_manager_get_scene_state(scene_manager, PokemonSceneGen3TradeEnd) &
        GEN3_TRADE_END_STOP_LINK;
    Gen3TradeEndAction action = gen3_trade_end_decide(pokemon_fap, stop_link != 0);

    if(end_anyway && action == Gen3TradeEndPickup) action = Gen3TradeEndStop;
    switch(action) {
    case Gen3TradeEndSaving:
    case Gen3TradeEndPickup:
        /* Not now: say why (D10), or still unreadable */
        scene_manager_set_scene_state(
            scene_manager, PokemonSceneGen3TradeEnd, (uint32_t)action | stop_link);
        pokemon_scene_gen3_trade_end_on_enter(pokemon_fap);
        break;
    case Gen3TradeEndAsk:
        /* D9: the club asks the GBA to walk out; the trade screen shows it leave, and the tick
         * stops the session once the GBA has closed the link (left) */
        gen3_session_request_leave(pokemon_fap->gen3_session);
        scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Menu);
        scene_manager_next_scene(scene_manager, PokemonSceneGen3Trade);
        break;
    case Gen3TradeEndStop:
        /* No GBA linked (or "Stop link?" confirmed), and nothing left to pick up, or End anyway.
         * The menu frees the session next; meanwhile the tick doesn't start it again. */
        gen3_session_stop(pokemon_fap->gen3_session);
        pokemon_fap->gen3_trade_ended = true;
        scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Menu);
        break;
    case Gen3TradeEndMenu:
        /* Stopped meanwhile (the GBA left; its Pokemon is in the editor): the menu frees it */
        scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Menu);
        break;
    }
}

bool pokemon_scene_gen3_trade_end_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    /* Only to tell the buttons apart: what each one means is what the dialog showed */
    const uint32_t shown = scene_manager_get_scene_state(scene_manager, PokemonSceneGen3TradeEnd) &
                           GEN3_TRADE_END_ACTION;

    /* Back: as Stay or OK, the session still running */
    if(event.type == SceneManagerEventTypeBack) return scene_manager_previous_scene(scene_manager);
    if(event.type != SceneManagerEventTypeCustom) return false;

    const bool pickup = shown == Gen3TradeEndPickup;
    if(pickup && event.event == DialogExResultRight) {
        /* Retry: open the bulk file again now; the decision polls once */
        if(!gen3_bulk_ready()) gen3_bulk_furi_open();
    } else if(pickup && event.event == DialogExResultLeft) {
        /* End anyway */
    } else if(event.event == DialogExResultLeft || event.event == DialogExResultCenter) {
        /* Stay, or OK on "The GBA is saving" */
        return scene_manager_previous_scene(scene_manager);
    } else if(event.event != DialogExResultRight) {
        return false;
    }

    /* Retry, End anyway or End */
    if(!pokemon_fap->gen3_session) {
        scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Menu);
    } else {
        gen3_trade_end_go_on(pokemon_fap, pickup && event.event == DialogExResultLeft);
    }
    return true;
}

void pokemon_scene_gen3_trade_end_on_exit(void* context) {
    UNUSED(context);
}
