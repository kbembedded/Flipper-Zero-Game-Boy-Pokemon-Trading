#include <gui/modules/dialog_ex.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_session_furi.h>
#include <src/gen3/gen3_trade_sync.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* "Stop link?": Back on the Link Test while the GBA is in the Cable Club, or on the trade screen
 * while a leave goes on (D1). Stop is refused while R6 forbids it (gen3_trade_stop_allowed). */

static void pokemon_scene_gen3_link_exit_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, result);
}

void pokemon_scene_gen3_link_exit_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    // Clean view
    dialog_ex_reset(dialog_ex);

    dialog_ex_set_left_button_text(dialog_ex, "Stay");
    dialog_ex_set_right_button_text(dialog_ex, "Stop");
    dialog_ex_set_header(dialog_ex, "Stop link?", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog_ex,
        "Your GBA may show a link\nerror and need a restart.\nIt saved before linking.",
        64,
        12,
        AlignCenter,
        AlignTop);
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, pokemon_scene_gen3_link_exit_dialog_callback);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

bool pokemon_scene_gen3_link_exit_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == DialogExResultLeft) {
            /* Stay: back to the Link Test or the trade screen, whose session never stopped */
            consumed = scene_manager_previous_scene(pokemon_fap->scene_manager);
        } else if(event.event == DialogExResultRight) {
            /* Decided now, from a fresh status: the link may have moved on while the dialog
             * was up. A trade session's received Pokemon is picked up first, so no stop frees
             * the club with it; a Link Test session is settled and sets neither flag R6
             * reads, so it always gets Stop. */
            const Gen3TradeEndAction action = gen3_trade_end_decide(pokemon_fap, true);
            if(action == Gen3TradeEndSaving || action == Gen3TradeEndAsk) {
                /* R6: the GBA is saving, or a trade is under way with it linked: as Stay */
                consumed = scene_manager_previous_scene(pokemon_fap->scene_manager);
            } else {
                if(action == Gen3TradeEndStop) {
                    /* The Gen III menu frees the stopped session. For a trade session, ended
                     * keeps the trade tick from starting it again meanwhile; the next Trade
                     * clears it. */
                    gen3_session_stop(pokemon_fap->gen3_session);
                    pokemon_fap->gen3_trade_ended = true;
                }
                /* Stop, Menu (the tick stopped it meanwhile) and Pickup go to the menu */
                consumed = scene_manager_search_and_switch_to_previous_scene(
                    pokemon_fap->scene_manager, PokemonSceneGen3Menu);
                /* NOTE: The above should never fail */
                furi_check(consumed);
                if(action == Gen3TradeEndPickup) {
                    /* A received Pokemon waits for the SD card: never stop silently, losing
                     * it. Retry / End anyway, over the menu. */
                    gen3_trade_end_open(pokemon_fap, action, true);
                }
            }
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* Back: same as Stay */
        consumed = scene_manager_previous_scene(pokemon_fap->scene_manager);
    }

    return consumed;
}

void pokemon_scene_gen3_link_exit_on_exit(void* context) {
    UNUSED(context);
}
