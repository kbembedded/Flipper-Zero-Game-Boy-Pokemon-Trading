#include <gui/modules/dialog_ex.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_session_furi.h>

/* "Unlock National Dex?" (D10): the confirm to turn the per-run setting on. Turning it off needs
 * none (the Gen III menu does it at once). The next 0x1144 block the Flipper sends carries the
 * flag, and a Ruby or Sapphire GBA keeps its National Dex for good. FireRed/LeafGreen never read
 * it (fr: trade_scene.c: TryEnableNationalDexFromLinkPartner has Ruby's check commented out),
 * and the dialog says so. */

static void gen3_natdex_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, result);
}

void pokemon_scene_gen3_natdex_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_header(dialog_ex, "Unlock National Dex?", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog_ex,
        "Ruby/Sapphire only.\nThe next trade unlocks\nit for good.",
        64,
        12,
        AlignCenter,
        AlignTop);
    dialog_ex_set_left_button_text(dialog_ex, "Cancel");
    dialog_ex_set_right_button_text(dialog_ex, "Turn on");
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_natdex_dialog_callback);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

bool pokemon_scene_gen3_natdex_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    if(event.type == SceneManagerEventTypeCustom && event.event == DialogExResultRight) {
        /* Turn on: the next session start takes it too (gen3_session_start_trade) */
        pokemon_fap->gen3_national_dex = true;
        if(pokemon_fap->gen3_session) {
            gen3_session_set_national_dex(pokemon_fap->gen3_session, true);
        }
        return scene_manager_previous_scene(pokemon_fap->scene_manager);
    }
    if((event.type == SceneManagerEventTypeCustom && event.event == DialogExResultLeft) ||
       event.type == SceneManagerEventTypeBack) {
        /* Cancel, or Back */
        return scene_manager_previous_scene(pokemon_fap->scene_manager);
    }
    return false;
}

void pokemon_scene_gen3_natdex_on_exit(void* context) {
    UNUSED(context);
}
