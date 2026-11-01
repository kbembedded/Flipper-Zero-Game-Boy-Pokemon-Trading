#include <furi.h>
#include <storage/storage.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>
#include <src/scenes/include/pokemon_gen3_cart.h>

#include <gblink/include/gblink_pinconf.h>
#include <src/gen3/gen3_bulk_furi.h>
#include <src/gen3/gen3_pins_furi.h>
#include <src/views/gen3_species_select.h>

/* Pokemon Cartridge: a second app built from the same manifest as Pokemon Trade Tool, whose Gen III
 * menu launches it. A FAP loads whole into RAM, and the Cartridge code does not fit beside the
 * trade code, so it is its own app. It shares PokemonFap and the Gen III editor scenes with the
 * pokemon app; every field it doesn't use stays NULL. Its scene table is
 * pokemon_cart_scene_config.h (POKEMON_CART_APP). */

/* The two pin files, as gblink_pinconf.c and gen3_pins_furi.c name them in APP_DATA_PATH. Pins are
 * set in the pokemon app (its Pinout screen); this app reads its own data folder, so it takes a
 * copy of them at start. */
static const char* const pokemon_cart_pin_files[] = {".gblink_pinconf", GEN3_SD_PIN_FILE_NAME};

static bool pokemon_cart_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    PokemonFap* pokemon_fap = context;

    return scene_manager_handle_custom_event(pokemon_fap->scene_manager, event);
}

static bool pokemon_cart_back_event_callback(void* context) {
    furi_assert(context);
    PokemonFap* pokemon_fap = context;

    return scene_manager_handle_back_event(pokemon_fap->scene_manager);
}

/* Each pin file of the pokemon app's data folder, copied into this app's; one the pokemon app
 * doesn't have is removed here too, so both apps always use the same pins. */
static void pokemon_cart_copy_pins(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = furi_string_alloc_set(APP_DATA_PATH(""));
    FuriString* from = furi_string_alloc();
    FuriString* to = furi_string_alloc();

    /* /ext/apps_data/pokemon_cart/, made if missing */
    storage_common_resolve_path_and_ensure_app_directory(storage, dir);
    for(size_t i = 0; i < COUNT_OF(pokemon_cart_pin_files); i++) {
        furi_string_printf(to, "%s%s", furi_string_get_cstr(dir), pokemon_cart_pin_files[i]);
        furi_string_printf(from, EXT_PATH("apps_data/pokemon/") "%s", pokemon_cart_pin_files[i]);
        storage_common_remove(storage, furi_string_get_cstr(to));
        if(storage_file_exists(storage, furi_string_get_cstr(from))) {
            storage_common_copy(storage, furi_string_get_cstr(from), furi_string_get_cstr(to));
        }
    }

    furi_string_free(to);
    furi_string_free(from);
    furi_string_free(dir);
    furi_record_close(RECORD_STORAGE);
}

static PokemonFap* pokemon_cart_alloc(void) {
    PokemonFap* pokemon_fap = malloc(sizeof(PokemonFap));
    memset(pokemon_fap, 0, sizeof(*pokemon_fap));

    // View dispatcher: no tick, as nothing here trades
    ViewDispatcher* view_dispatcher = view_dispatcher_alloc();
    pokemon_fap->view_dispatcher = view_dispatcher;
    view_dispatcher_set_event_callback_context(view_dispatcher, pokemon_fap);
    view_dispatcher_set_custom_event_callback(view_dispatcher, pokemon_cart_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        view_dispatcher, pokemon_cart_back_event_callback);
    view_dispatcher_attach_to_gui(
        view_dispatcher, (Gui*)furi_record_open(RECORD_GUI), ViewDispatcherTypeFullscreen);

    // The module views, under the pokemon app's view ids
    pokemon_fap->text_input = text_input_alloc();
    view_dispatcher_add_view(
        view_dispatcher, AppViewTextInput, text_input_get_view(pokemon_fap->text_input));
    pokemon_fap->submenu = submenu_alloc();
    view_dispatcher_add_view(
        view_dispatcher, AppViewSubmenu, submenu_get_view(pokemon_fap->submenu));
    pokemon_fap->variable_item_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        view_dispatcher,
        AppViewVariableItem,
        variable_item_list_get_view(pokemon_fap->variable_item_list));
    pokemon_fap->dialog_ex = dialog_ex_alloc();
    view_dispatcher_add_view(
        view_dispatcher, AppViewDialogEx, dialog_ex_get_view(pokemon_fap->dialog_ex));

    // Gen III species picker
    pokemon_fap->gen3_species_select =
        gen3_species_select_alloc(view_dispatcher, AppViewGen3SpeciesSelect);

    // Scene manager, with this app's scene table
    pokemon_fap->scene_manager = scene_manager_alloc(&pokemon_scene_handlers, pokemon_fap);

    // gblink, with the pokemon app's pins
    pokemon_fap->gblink_handle = gblink_alloc();
    pokemon_cart_copy_pins();
    gblink_pinconf_load(pokemon_fap->gblink_handle);
    gen3_sd_pin_load(pokemon_fap->gblink_handle);

    // The instructions; on the first entry they check the Gen III data file
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartConnect, 0);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartConnect);

    return pokemon_fap;
}

static void pokemon_cart_free(PokemonFap* pokemon_fap) {
    furi_assert(pokemon_fap);

    // The cartridge session stops its driver before gblink goes, and the progress view leaves the
    // view dispatcher before it is freed
    gen3_cart_ui_end(pokemon_fap);

    gblink_free(pokemon_fap->gblink_handle);

    view_dispatcher_remove_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
    submenu_free(pokemon_fap->submenu);
    view_dispatcher_remove_view(pokemon_fap->view_dispatcher, AppViewTextInput);
    text_input_free(pokemon_fap->text_input);
    view_dispatcher_remove_view(pokemon_fap->view_dispatcher, AppViewVariableItem);
    variable_item_list_free(pokemon_fap->variable_item_list);
    view_dispatcher_remove_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
    dialog_ex_free(pokemon_fap->dialog_ex);

    gen3_species_select_free(
        pokemon_fap->view_dispatcher, AppViewGen3SpeciesSelect, pokemon_fap->gen3_species_select);

    view_dispatcher_free(pokemon_fap->view_dispatcher);
    scene_manager_free(pokemon_fap->scene_manager);

    // Gen III bulk data file, opened by gen3_menu_editor_ready
    gen3_bulk_furi_close();

    furi_record_close(RECORD_GUI);

    free(pokemon_fap->gen3_editor);
    free(pokemon_fap);
}

int32_t pokemon_cart_app(void* p) {
    UNUSED(p);
    PokemonFap* pokemon_fap = pokemon_cart_alloc();

    view_dispatcher_run(pokemon_fap->view_dispatcher);

    pokemon_cart_free(pokemon_fap);

    return 0;
}
