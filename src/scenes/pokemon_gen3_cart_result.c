#include <src/scenes/include/pokemon_gen3_cart.h>

#include <furi.h>

/* What a job, or a step of the Cartridge scenes, came to: the header and text the ui holds, OK,
 * and Retry when the ui's retry_mode offers one. OK (or Back) returns to the scene result_back
 * names. */

void pokemon_scene_gen3_cart_result_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    gen3_cart_ui_dialog(pokemon_fap);
    dialog_ex_set_header(dialog_ex, ui->result_header, 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog_ex,
        ui->result_text ? ui->result_text : gen3_cart_result_text(ui->result),
        64,
        14,
        AlignCenter,
        AlignTop);
    dialog_ex_set_center_button_text(dialog_ex, "OK");
    if(ui->retry_mode != Gen3CartRetryNone) dialog_ex_set_right_button_text(dialog_ex, "Retry");
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

/* OK: back to result_back, and the next result returns to the hub unless it says otherwise */
static void gen3_cart_result_ok(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const uint32_t back = ui->result_back;

    ui->result_back = PokemonSceneGen3Cart;
    ui->result_text = NULL;
    ui->retry_mode = Gen3CartRetryNone;
    if(!scene_manager_search_and_switch_to_previous_scene(pokemon_fap->scene_manager, back)) {
        scene_manager_previous_scene(pokemon_fap->scene_manager);
    }
}

static void gen3_cart_result_retry(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const Gen3CartRetry retry = ui->retry_mode;

    ui->retry_mode = Gen3CartRetryNone;
    ui->result_text = NULL;
    switch(retry) {
    case Gen3CartRetryConnect:
        /* The instructions, to start the GBA and send the helper again. The cart's backup and
         * edits stay in the session: attaching keeps them if the save hasn't changed. */
        ui->result_back = PokemonSceneGen3Cart;
        scene_manager_search_and_switch_to_previous_scene(
            pokemon_fap->scene_manager, PokemonSceneGen3CartConnect);
        break;
    case Gen3CartRetryReconnect:
        gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobReconnect, NULL);
        break;
    case Gen3CartRetryNone:
        break;
    }
}

bool pokemon_scene_gen3_cart_result_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GEN3_CART_EVENT_CENTER) {
            gen3_cart_result_ok(pokemon_fap);
            consumed = true;
        } else if(event.event == GEN3_CART_EVENT_RIGHT) {
            gen3_cart_result_retry(pokemon_fap);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* Never back into a job that has ended */
        gen3_cart_result_ok(pokemon_fap);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_cart_result_on_exit(void* context) {
    UNUSED(context);
}
