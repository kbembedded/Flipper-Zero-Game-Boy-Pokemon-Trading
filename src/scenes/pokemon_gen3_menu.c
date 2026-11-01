#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_session_furi.h>
#include <src/gen3/gen3_trade_sync.h>
#include <src/views/gen3_link_test.h>
#include <src/views/gen3_trade.h>

#include <stdlib.h>
#include <loader/loader.h>
#include <storage/storage.h>
#include <src/gen3/gen3_bulk_furi.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/scenes/include/pokemon_gen3_menu.h>
#include <src/views/gen3_species_select.h>

/* The Cartridge item. It isn't a scene of this app: it launches the pokemon_cart app. The value is
 * clear of the scene ids and of the other Gen III events. */
#define GEN3_MENU_ITEM_CARTRIDGE PokemonSceneNum

/* The Cartridge app's file. Both apps are installed side by side, in apps/GPIO (fap_category). */
#define GEN3_MENU_CART_FAP "pokemon_cart.fap"

/* gen3_menu_status's buffer */
static Gen3ClubStatus gen3_menu_status_buf;

const Gen3ClubStatus* gen3_menu_status(PokemonFap* pokemon_fap, bool* log_ok, bool* cable_ok) {
    gen3_session_status(pokemon_fap->gen3_session, &gen3_menu_status_buf, log_ok, cable_ok);
    return &gen3_menu_status_buf;
}

static void gen3_menu_item_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* Highlight the same item when we come back to this menu */
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Menu, index);

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

/* The Link Test session and its view outlive the Link Test scene while the "Stop link?" dialog
 * is up, so that "Stay" returns to the running link. Every way out of the Link Test for good
 * (Back while not in the Cable Club, or "Stop") comes back to this menu, so it ends here. A trade
 * session and its trade screen live on behind every Gen III scene while it runs (D1), and end
 * here too once it doesn't (not started yet, or stopped: the GBA left, "Stop link?", End anyway),
 * or with End trade or Back while no GBA is linked (D9). The sync stays until the app exits,
 * restarted (it keeps the block on a traded-away Pokemon, O2). Call with neither view shown,
 * and for a running trade session only on gen3_trade_end_decide's Stop (the sync has
 * every received Pokemon). */
static void gen3_menu_end_session(PokemonFap* pokemon_fap) {
    if(pokemon_fap->gen3_session) {
        gen3_session_free(pokemon_fap->gen3_session); /* stops the link, closes the log */
        pokemon_fap->gen3_session = NULL;
    }
    if(pokemon_fap->gen3_link_test) {
        gen3_link_test_free(
            pokemon_fap->view_dispatcher, AppViewGen3LinkTest, pokemon_fap->gen3_link_test);
        pokemon_fap->gen3_link_test = NULL;
    }
    if(pokemon_fap->gen3_trade_view) {
        gen3_trade_view_free(
            pokemon_fap->view_dispatcher, AppViewGen3Trade, pokemon_fap->gen3_trade_view);
        pokemon_fap->gen3_trade_view = NULL;
    }
    if(pokemon_fap->gen3_trade_sync) gen3_trade_sync_restart(pokemon_fap->gen3_trade_sync);
}

/* The trade screen's OK (Edit). App thread (view input): Gen3Trade handles the event. */
static void gen3_menu_trade_edit_callback(void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_TRADE_EVENT_EDIT);
}

/* A new trade session with its trade screen, and the sync the first time. The tick starts it
 * once the cable reads OK. A session left from before (one that stopped while this menu was
 * shown) goes first. */
static void gen3_menu_trade_alloc(PokemonFap* pokemon_fap) {
    gen3_menu_end_session(pokemon_fap); /* restarts a sync kept from an earlier session */

    pokemon_fap->gen3_session = gen3_session_alloc(pokemon_fap->gblink_handle);
    pokemon_fap->gen3_trade_view =
        gen3_trade_view_alloc(pokemon_fap->view_dispatcher, AppViewGen3Trade);
    gen3_trade_view_set_edit_callback(
        pokemon_fap->gen3_trade_view, gen3_menu_trade_edit_callback, pokemon_fap);
    if(!pokemon_fap->gen3_trade_sync) {
        pokemon_fap->gen3_trade_sync = malloc(sizeof(Gen3TradeSync));
        gen3_trade_sync_init(pokemon_fap->gen3_trade_sync);
    }
    pokemon_fap->gen3_trade_ended = false;
}

/* The items for the session as it is now: End trade while a trade session runs (D9), Link Test
 * and Cartridge only while none does (TIM2 runs one link at a time, and Cartridge closes this
 * app). */
void gen3_menu_rebuild(PokemonFap* pokemon_fap) {
    const bool trading = gen3_session_trading(pokemon_fap->gen3_session);

    submenu_reset(pokemon_fap->submenu);
    submenu_set_header(pokemon_fap->submenu, "Gen III");

    submenu_add_item(
        pokemon_fap->submenu, "Trade", PokemonSceneGen3Trade, gen3_menu_item_callback, pokemon_fap);

    submenu_add_item(
        pokemon_fap->submenu,
        "Edit Pokemon",
        PokemonSceneGen3Edit,
        gen3_menu_item_callback,
        pokemon_fap);

    submenu_add_item(
        pokemon_fap->submenu,
        "Load .pk3",
        PokemonSceneGen3Load,
        gen3_menu_item_callback,
        pokemon_fap);

    submenu_add_item(
        pokemon_fap->submenu,
        "Save .pk3",
        PokemonSceneGen3SaveName,
        gen3_menu_item_callback,
        pokemon_fap);

    submenu_add_item(
        pokemon_fap->submenu,
        pokemon_fap->gen3_national_dex ? "National Dex: On" : "National Dex: Off",
        PokemonSceneGen3NatDex,
        gen3_menu_item_callback,
        pokemon_fap);

    if(trading) {
        submenu_add_item(
            pokemon_fap->submenu,
            "End trade",
            PokemonSceneGen3TradeEnd,
            gen3_menu_item_callback,
            pokemon_fap);
    } else {
        submenu_add_item(
            pokemon_fap->submenu,
            "Link Test",
            PokemonSceneGen3LinkTest,
            gen3_menu_item_callback,
            pokemon_fap);
        submenu_add_item(
            pokemon_fap->submenu,
            "Cartridge",
            GEN3_MENU_ITEM_CARTRIDGE,
            gen3_menu_item_callback,
            pokemon_fap);
    }

    submenu_set_selected_item(
        pokemon_fap->submenu,
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Menu));
}

void pokemon_scene_gen3_menu_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* The bulk data file is opened once, then kept open until the app exits (free_app closes
     * it). Until it opens, each visit to this menu tries again. A read that fails later (the SD
     * card pulled and put back) uninstalls it, so the next visit opens it again. */
    if(!gen3_bulk_ready()) gen3_bulk_furi_open();
    if(!pokemon_fap->gen3_species_select) {
        pokemon_fap->gen3_species_select =
            gen3_species_select_alloc(pokemon_fap->view_dispatcher, AppViewGen3SpeciesSelect);
    }

    /* A trade session runs on behind this menu (D1) */
    const bool trading = gen3_session_trading(pokemon_fap->gen3_session);

    gen3_menu_rebuild(pokemon_fap);

    /* Switch away from the Link Test or trade view before freeing it */
    gen3_menu_show_menu(pokemon_fap);
    if(!trading) gen3_menu_end_session(pokemon_fap);
}

/* Edit Pokemon or Load .pk3 (scene): with a trade session running, the club holds the offer
 * first (O1); if it can't (a trade is pending, or a pickup is being retried), the trade screen
 * says why instead. begin_edit and the scene switch share this handler, so no tick runs between
 * them. */
static void gen3_menu_open_editor(PokemonFap* pokemon_fap, uint32_t scene) {
    if(!gen3_menu_editor_ready(pokemon_fap)) {
        gen3_menu_show_data_missing(pokemon_fap);
    } else if(gen3_trade_begin_edit(pokemon_fap)) {
        scene_manager_next_scene(pokemon_fap->scene_manager, scene);
    } else {
        scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3Trade);
    }
}

/* End trade, or Back, while a trade session may run (D9, D10). With the GBA linked or saving, or a
 * received Pokemon waiting for the SD card, Gen3TradeEnd opens and this returns true. false: stop
 * and free the session now (Stop: no GBA is linked and the editor has every received Pokemon), or
 * it has stopped already (Menu). */
static bool gen3_menu_end_dialog(PokemonFap* pokemon_fap) {
    if(!pokemon_fap->gen3_session) return false;
    const Gen3TradeEndAction action = gen3_trade_end_decide(pokemon_fap, false);
    if(action == Gen3TradeEndStop || action == Gen3TradeEndMenu) return false;

    gen3_trade_end_open(pokemon_fap, action, false);
    return true;
}

/* The Cartridge app's file into path: beside this app's own .fap, else in apps/GPIO, whichever
 * holds it first. false if neither does. */
static bool gen3_menu_cart_path(Loader* loader, Storage* storage, FuriString* path) {
    if(loader_get_application_launch_path(loader, path)) {
        const size_t slash = furi_string_search_rchar(path, '/', 0);
        if(slash != FURI_STRING_FAILURE) {
            furi_string_left(path, slash + 1);
            furi_string_cat_str(path, GEN3_MENU_CART_FAP);
            if(storage_file_exists(storage, furi_string_get_cstr(path))) return true;
        }
    }
    furi_string_set(path, EXT_PATH("apps/GPIO/" GEN3_MENU_CART_FAP));
    return storage_file_exists(storage, furi_string_get_cstr(path));
}

/* Cartridge is another app, pokemon_cart: a FAP loads whole into RAM, and the Cartridge code does
 * not fit beside the trade code. The loader starts it once this app has exited, and reports a
 * failed start in its own dialog. Call only while no trade session runs: one that stopped goes
 * now, with the Link Test and trade views, as for Link Test. Without the app's file this app
 * stays, and a dialog says where to copy it. */
static void gen3_menu_launch_cart(PokemonFap* pokemon_fap) {
    Loader* loader = furi_record_open(RECORD_LOADER);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc();

    if(gen3_menu_cart_path(loader, storage, path)) {
        gen3_menu_end_session(pokemon_fap);
        loader_enqueue_launch(
            loader, furi_string_get_cstr(path), NULL, LoaderDeferredLaunchFlagGui);
        scene_manager_stop(pokemon_fap->scene_manager);
        view_dispatcher_stop(pokemon_fap->view_dispatcher);
    } else {
        gen3_menu_show_dialog(
            pokemon_fap,
            "App missing",
            "Copy pokemon_cart.fap\nnext to pokemon.fap,\nin apps/GPIO.");
    }

    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_LOADER);
}

bool pokemon_scene_gen3_menu_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    /* Only menu items and the dialog's OK. A Link Test refresh tick still queued when the Link
     * Test closed may arrive here too; ignore it. */
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == PokemonSceneGen3LinkTest) {
            /* A trade session the tick stopped while this menu was shown is still here: the
             * Link Test makes its own */
            if(!gen3_session_trading(pokemon_fap->gen3_session)) {
                gen3_menu_end_session(pokemon_fap);
                scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            }
            consumed = true;
        } else if(event.event == GEN3_MENU_ITEM_CARTRIDGE) {
            /* Never while a trade session runs: closing this app would stop it */
            if(!gen3_session_trading(pokemon_fap->gen3_session)) {
                gen3_menu_launch_cart(pokemon_fap);
            }
            consumed = true;
        } else if(event.event == PokemonSceneGen3Trade) {
            if(gen3_session_trading(pokemon_fap->gen3_session)) {
                /* The running session's trade screen, whatever the SD card does: its editor
                 * exists, and the guide says why a pickup waits for the data file */
                scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            } else if(!gen3_menu_editor_ready(pokemon_fap)) {
                /* A new session's tick needs the editor: it hands the GBA player to it and
                 * offers its Pokemon */
                gen3_menu_show_data_missing(pokemon_fap);
            } else {
                gen3_menu_trade_alloc(pokemon_fap);
                scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            }
            consumed = true;
        } else if(event.event == PokemonSceneGen3Edit || event.event == PokemonSceneGen3Load) {
            gen3_menu_open_editor(pokemon_fap, event.event);
            consumed = true;
        } else if(event.event == PokemonSceneGen3SaveName) {
            if(gen3_menu_editor_ready(pokemon_fap)) {
                gen3_pk3_default_name(
                    &pokemon_fap->gen3_editor->pkm,
                    pokemon_fap->gen3_file_name,
                    sizeof(pokemon_fap->gen3_file_name));
                /* The text scene's mode, as the Gen III editor sets it for a nickname */
                scene_manager_set_scene_state(
                    pokemon_fap->scene_manager, PokemonSceneGen3Nickname, event.event);
                scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            } else {
                gen3_menu_show_data_missing(pokemon_fap);
            }
            consumed = true;
        } else if(event.event == PokemonSceneGen3NatDex) {
            if(pokemon_fap->gen3_national_dex) {
                /* Off at once: only turning it on needs a confirm (D10) */
                pokemon_fap->gen3_national_dex = false;
                if(pokemon_fap->gen3_session) {
                    gen3_session_set_national_dex(pokemon_fap->gen3_session, false);
                }
                gen3_menu_rebuild(pokemon_fap);
            } else {
                scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
            }
            consumed = true;
        } else if(event.event == PokemonSceneGen3TradeEnd) {
            if(!gen3_menu_end_dialog(pokemon_fap)) {
                /* No GBA linked: the session, its trade view and the sync's club state go now */
                gen3_menu_end_session(pokemon_fap);
                gen3_menu_rebuild(pokemon_fap);
            }
            consumed = true;
        } else if(event.event == GEN3_MENU_EVENT_DATA_OK && gen3_menu_dialog_up()) {
            gen3_menu_show_menu(pokemon_fap);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(gen3_menu_dialog_up()) {
            /* Back on the dialog returns to this menu, not to the main menu */
            gen3_menu_show_menu(pokemon_fap);
            consumed = true;
        } else if(gen3_session_trading(pokemon_fap->gen3_session)) {
            /* With the GBA linked, Back asks first, as End trade does (D9, D10) */
            consumed = gen3_menu_end_dialog(pokemon_fap);
        }
        /* Not consumed: to the main menu, without a session (D1) */
        if(!consumed) gen3_menu_end_session(pokemon_fap);
    }

    return consumed;
}

void pokemon_scene_gen3_menu_on_exit(void* context) {
    UNUSED(context);
}
