#include <furi_hal_light.h>
#include <pokemon_icons.h>

#include <src/include/pokemon_app.h>
#include <src/include/pokemon_data.h>
#include <src/views/trade.h>
#include <src/include/pokemon_char_encode.h>

#include <src/scenes/include/pokemon_scene.h>

#include <gblink/include/gblink_pinconf.h>
#include <src/gen3/gen3_pins_furi.h>
#include <src/views/gen3_species_select.h>
#include <gblink.h>
#include <src/gen3/gen3_bulk_furi.h>
#include <src/gen3/gen3_session_furi.h>
#include <src/views/gen3_trade.h>
#include <src/views/gen3_link_test.h>

bool pokemon_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    PokemonFap* pokemon_fap = context;

    return scene_manager_handle_custom_event(pokemon_fap->scene_manager, event);
}

bool pokemon_back_event_callback(void* context) {
    furi_assert(context);
    PokemonFap* pokemon_fap = context;
    return scene_manager_handle_back_event(pokemon_fap->scene_manager);
}

/* Every 100 ms the app thread is idle: the Gen III trade tick, which returns at once unless a
 * trade screen exists. */
static void pokemon_tick_event_callback(void* context) {
    furi_assert(context);
    PokemonFap* pokemon_fap = context;

    gen3_trade_tick(pokemon_fap);
}


PokemonFap* pokemon_alloc() {
    PokemonFap* pokemon_fap = (PokemonFap*)malloc(sizeof(PokemonFap));
    ViewDispatcher* view_dispatcher = NULL;

    // Gen III editor, allocated by the Gen III menu
    pokemon_fap->gen3_editor = NULL;
    pokemon_fap->gen3_species_select = NULL;

    // View dispatcher
    view_dispatcher = view_dispatcher_alloc();
    pokemon_fap->view_dispatcher = view_dispatcher;

    view_dispatcher_set_event_callback_context(view_dispatcher, pokemon_fap);
    view_dispatcher_set_custom_event_callback(view_dispatcher, pokemon_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(view_dispatcher, pokemon_back_event_callback);

    // Gen III link sessions and trade, allocated by the Gen III menu; the trade tick
    pokemon_fap->gen3_session = NULL;
    pokemon_fap->gen3_link_test = NULL;
    pokemon_fap->gen3_trade_view = NULL;
    pokemon_fap->gen3_trade_sync = NULL;
    pokemon_fap->gen3_national_dex = false;
    pokemon_fap->gen3_trade_ended = false;
    view_dispatcher_set_tick_event_callback(
        view_dispatcher, pokemon_tick_event_callback, furi_ms_to_ticks(100));
    view_dispatcher_attach_to_gui(
        view_dispatcher,
        (Gui*)furi_record_open(RECORD_GUI),
        ViewDispatcherTypeFullscreen);

    // Text input
    pokemon_fap->text_input = text_input_alloc();
    view_dispatcher_add_view(
        view_dispatcher, AppViewTextInput, text_input_get_view(pokemon_fap->text_input));

    // Submenu
    pokemon_fap->submenu = submenu_alloc();
    view_dispatcher_add_view(
        view_dispatcher, AppViewSubmenu, submenu_get_view(pokemon_fap->submenu));

    // Variable Item List
    pokemon_fap->variable_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        view_dispatcher, AppViewVariableItem, variable_item_list_get_view(pokemon_fap->variable_item_list));

    // DialogEx
    pokemon_fap->dialog_ex = dialog_ex_alloc();
    view_dispatcher_add_view(
        view_dispatcher, AppViewDialogEx, dialog_ex_get_view(pokemon_fap->dialog_ex));

    // Scene manager
    pokemon_fap->scene_manager = scene_manager_alloc(&pokemon_scene_handlers, pokemon_fap);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneMainMenu);

    // Allocate gblink before going to main menu
    pokemon_fap->gblink_handle = gblink_alloc();
    gblink_pinconf_load(pokemon_fap->gblink_handle);
    gen3_sd_pin_load(pokemon_fap->gblink_handle);

    return pokemon_fap;
}

void free_app(PokemonFap* pokemon_fap) {
    furi_assert(pokemon_fap);

    // Gen III session (a trade session outlives the Gen III menu), before the pins it uses
    if(pokemon_fap->gen3_session) {
        gen3_session_free(pokemon_fap->gen3_session);
        pokemon_fap->gen3_session = NULL;
    }

    // gblink
    gblink_free(pokemon_fap->gblink_handle);

    // Submenu
    view_dispatcher_remove_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
    submenu_free(pokemon_fap->submenu);

    // text input
    view_dispatcher_remove_view(pokemon_fap->view_dispatcher, AppViewTextInput);
    text_input_free(pokemon_fap->text_input);

    // Vairable item list
    view_dispatcher_remove_view(pokemon_fap->view_dispatcher, AppViewVariableItem);
    variable_item_list_free(pokemon_fap->variable_item_list);

    // Dialog ex
    view_dispatcher_remove_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
    dialog_ex_free(pokemon_fap->dialog_ex);

    // Gen III species picker
    if(pokemon_fap->gen3_species_select) {
        gen3_species_select_free(
            pokemon_fap->view_dispatcher,
            AppViewGen3SpeciesSelect,
            pokemon_fap->gen3_species_select);
    }

    // Gen III Link Test screen, if the app closes while it is up
    if(pokemon_fap->gen3_link_test) {
        gen3_link_test_free(
            pokemon_fap->view_dispatcher, AppViewGen3LinkTest, pokemon_fap->gen3_link_test);
        pokemon_fap->gen3_link_test = NULL;
    }

    // Gen III trade screen and sync
    if(pokemon_fap->gen3_trade_view) {
        gen3_trade_view_free(
            pokemon_fap->view_dispatcher, AppViewGen3Trade, pokemon_fap->gen3_trade_view);
        pokemon_fap->gen3_trade_view = NULL;
    }
    free(pokemon_fap->gen3_trade_sync);
    pokemon_fap->gen3_trade_sync = NULL;

    view_dispatcher_free(pokemon_fap->view_dispatcher);

    // Free scenes
    scene_manager_free(pokemon_fap->scene_manager);

    // Gen III bulk data file, opened by the Gen III menu
    gen3_bulk_furi_close();

    // Close records
    furi_record_close(RECORD_GUI);

    // Free rest
    free(pokemon_fap->gen3_editor);
    free(pokemon_fap);
    pokemon_fap = NULL;
}

int32_t pokemon_app(void* p) {
    UNUSED(p);
    PokemonFap* pokemon_fap = pokemon_alloc();

    furi_hal_light_set(LightRed, 0x00);
    furi_hal_light_set(LightGreen, 0x00);
    furi_hal_light_set(LightBlue, 0x00);

    //switch view and run dispatcher
    view_dispatcher_run(pokemon_fap->view_dispatcher);

    // Free resources
    free_app(pokemon_fap);

    return 0;
}
