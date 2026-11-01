#include <src/scenes/include/pokemon_gen3_cart.h>

#include <furi.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* The Cartridge app's first scene: how to start the GBA, then the Connect job (send the helper,
 * identify the cart, back up its save) on the progress view, then the hub. Every later Connect
 * comes back here: Retry after a Connect failed, or after a Reconnect found no helper. */

/* Scene state */
#define GEN3_CART_CONNECT_FRESH 0 /* never shown: the bulk data file is checked first */
#define GEN3_CART_CONNECT_MISSING 1 /* "Data missing" (gen3_menu_show_data_missing) */
#define GEN3_CART_CONNECT_ASK 2 /* the instructions */
#define GEN3_CART_CONNECT_BUSY 3 /* the Connect job runs */
#define GEN3_CART_CONNECT_STOPPING 4 /* Back stopped it: wait for its end, then the instructions */

static void gen3_cart_connect_set_state(PokemonFap* pokemon_fap, uint32_t state) {
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartConnect, state);
}

static uint32_t gen3_cart_connect_state(PokemonFap* pokemon_fap) {
    return scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartConnect);
}

static void gen3_cart_connect_ask(PokemonFap* pokemon_fap) {
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    gen3_cart_connect_set_state(pokemon_fap, GEN3_CART_CONNECT_ASK);
    gen3_cart_ui_dialog(pokemon_fap);
    dialog_ex_set_header(dialog_ex, "Cartridge", 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(
        dialog_ex,
        "GBA off, cart in.\nSmall plug: Flipper.\nHold START+SELECT\nand power on.",
        64,
        12,
        AlignCenter,
        AlignTop);
    dialog_ex_set_left_button_text(dialog_ex, "Back");
    dialog_ex_set_right_button_text(dialog_ex, "Start");
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

static void gen3_cart_connect_start(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    /* false only while an earlier job still ends: Start does nothing then */
    if(!gen3_cart_session_run(ui->session, Gen3CartJobConnect, NULL)) return;
    gen3_cart_connect_set_state(pokemon_fap, GEN3_CART_CONNECT_BUSY);
    gen3_cart_ui_progress(pokemon_fap, Gen3CartScreenStarting);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewGen3CartProgress);
}

/* The job ended: the hub on Ok (with the dropped edits' notice first, or the result of a write or
 * restore cut short that the Connect finished), else the failure, whose OK and Retry both come
 * back to the instructions */
static void gen3_cart_connect_done(PokemonFap* pokemon_fap, const Gen3CartSessionStatus* st) {
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    /* st is the status buffer, which the calls below read again */
    const Gen3CartResult r = st->result;
    const Gen3CartStep step = st->step;
    const bool dropped = st->dropped;
    const Gen3CartJob job = gen3_cart_screen_result_job(Gen3CartJobConnect, r, st->finished);

    if(r != Gen3CartOk && job == Gen3CartJobConnect) {
        gen3_cart_ui_set_result(
            pokemon_fap, Gen3CartJobConnect, step, r, false, PokemonSceneGen3CartConnect);
        scene_manager_next_scene(scene_manager, PokemonSceneGen3CartResult);
        return;
    }
    gen3_cart_ui_take_trainer(pokemon_fap);
    scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, 0);
    scene_manager_next_scene(scene_manager, PokemonSceneGen3Cart);
    if(dropped || job != Gen3CartJobConnect) {
        gen3_cart_ui_set_result(pokemon_fap, job, step, r, dropped, PokemonSceneGen3Cart);
        scene_manager_next_scene(scene_manager, PokemonSceneGen3CartResult);
    }
}

static void gen3_cart_connect_tick(PokemonFap* pokemon_fap) {
    const uint32_t state = gen3_cart_connect_state(pokemon_fap);
    if(state != GEN3_CART_CONNECT_BUSY && state != GEN3_CART_CONNECT_STOPPING) return;

    const Gen3CartSessionStatus* st = gen3_cart_ui_status(pokemon_fap);
    if(st->busy) {
        gen3_cart_ui_progress(
            pokemon_fap,
            state == GEN3_CART_CONNECT_STOPPING ? Gen3CartScreenStopping : Gen3CartScreenRunning);
    } else if(state == GEN3_CART_CONNECT_STOPPING) {
        gen3_cart_connect_ask(pokemon_fap);
    } else {
        gen3_cart_connect_done(pokemon_fap, st);
    }
}

/* Back on the instructions: unwritten edits (a Retry after the link was lost) are asked about */
static void gen3_cart_connect_back(PokemonFap* pokemon_fap) {
    if(gen3_cart_ui_changes(pokemon_fap) > 0) {
        gen3_cart_ui_confirm(pokemon_fap, PokemonSceneGen3CartLeave);
    } else {
        gen3_cart_ui_leave(pokemon_fap);
    }
}

/* While the job runs, Back stops it; the instructions show once it has ended */
static void gen3_cart_connect_stop(PokemonFap* pokemon_fap) {
    if(gen3_cart_connect_state(pokemon_fap) != GEN3_CART_CONNECT_BUSY) return;
    gen3_cart_session_cancel(gen3_cart_ui_get(pokemon_fap)->session);
    gen3_cart_connect_set_state(pokemon_fap, GEN3_CART_CONNECT_STOPPING);
    gen3_cart_ui_progress(pokemon_fap, Gen3CartScreenStopping);
}

void pokemon_scene_gen3_cart_connect_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;

    /* Without the bulk data file the Cartridge can't show or edit a Pokemon: say so, and its OK
     * closes the app. The editor exists from here on. */
    if(gen3_cart_connect_state(pokemon_fap) == GEN3_CART_CONNECT_FRESH &&
       !gen3_menu_editor_ready(pokemon_fap)) {
        gen3_cart_connect_set_state(pokemon_fap, GEN3_CART_CONNECT_MISSING);
        gen3_menu_show_data_missing(pokemon_fap);
        return;
    }
    gen3_cart_connect_ask(pokemon_fap);
}

bool pokemon_scene_gen3_cart_connect_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(gen3_cart_connect_state(pokemon_fap) == GEN3_CART_CONNECT_MISSING &&
       gen3_menu_dialog_up()) {
        /* "Data missing": nothing is below this scene, and nothing here works without the file */
        if((event.type == SceneManagerEventTypeCustom && event.event == GEN3_MENU_EVENT_DATA_OK) ||
           event.type == SceneManagerEventTypeBack) {
            gen3_cart_ui_leave(pokemon_fap);
            consumed = true;
        }
        return consumed;
    }

    const bool asking = gen3_cart_connect_state(pokemon_fap) == GEN3_CART_CONNECT_ASK;
    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        /* The instructions' buttons; a second press queued behind Start does nothing */
        case GEN3_CART_EVENT_RIGHT:
            if(asking) gen3_cart_connect_start(pokemon_fap);
            consumed = true;
            break;
        case GEN3_CART_EVENT_LEFT:
            if(asking) gen3_cart_connect_back(pokemon_fap);
            consumed = true;
            break;
        case GEN3_CART_EVENT_BACK:
            gen3_cart_connect_stop(pokemon_fap);
            consumed = true;
            break;
        case GEN3_CART_EVENT_TICK:
            gen3_cart_connect_tick(pokemon_fap);
            consumed = true;
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* Nothing is below this scene: Back never pops it */
        if(asking) {
            gen3_cart_connect_back(pokemon_fap);
        } else {
            gen3_cart_connect_stop(pokemon_fap);
        }
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_cart_connect_on_exit(void* context) {
    UNUSED(context);
}
