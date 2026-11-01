#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <furi_hal_random.h>
#include <stdlib.h>
#include <src/gen3/gen3_bulk_furi.h>
#include <src/gen3/gen3_edit.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* The Gen III helpers both apps share: the pokemon app's Gen III menu and the pokemon_cart app's
 * hub, each registered as Gen3Menu, and the Gen III editor scenes above them. */

/* Deleting the app's asset folder makes the Flipper unpack the app's assets again on the next
 * start; a reinstall alone doesn't if their signature still matches. Each app has its own. */
#ifdef POKEMON_CART_APP
#define GEN3_DATA_MISSING_TEXT \
    "Gen III data missing.\nDelete apps_assets/\npokemon_cart on the\nSD card, then reopen."
#else
#define GEN3_DATA_MISSING_TEXT \
    "Gen III data file missing.\nDelete apps_assets/pokemon\non the SD card, then reopen."
#endif

/* A dialog gen3_menu_show_dialog showed is up instead of the menu */
static bool gen3_menu_dialog_shown = false;

static void gen3_menu_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    if(result == DialogExResultCenter) {
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_MENU_EVENT_DATA_OK);
    }
}

void gen3_menu_show_dialog(PokemonFap* pokemon_fap, const char* header, const char* text) {
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_header(dialog_ex, header, 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog_ex, text, 64, 13, AlignCenter, AlignTop);
    dialog_ex_set_center_button_text(dialog_ex, "OK");
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_menu_dialog_callback);

    gen3_menu_dialog_shown = true;
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

bool gen3_menu_dialog_up(void) {
    return gen3_menu_dialog_shown;
}

/* Everything but the Link Test needs gen3_bulk.bin (format v2): the species, move and item names
 * and the learnsets. */
void gen3_menu_show_data_missing(PokemonFap* pokemon_fap) {
    /* From an editor scene, back to the menu first. From the menu itself this does nothing. */
    scene_manager_search_and_switch_to_previous_scene(
        pokemon_fap->scene_manager, PokemonSceneGen3Menu);

    gen3_menu_show_dialog(pokemon_fap, "Data missing", GEN3_DATA_MISSING_TEXT);
}

static void gen3_notice_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    if(result == DialogExResultCenter) {
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_EVENT_NOTICE_OK);
    }
}

void gen3_menu_show_notice(PokemonFap* pokemon_fap, const char* header, const char* text) {
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_header(dialog_ex, header, 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog_ex, text, 64, 13, AlignCenter, AlignTop);
    dialog_ex_set_center_button_text(dialog_ex, "OK");
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_notice_callback);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

void gen3_menu_show_menu(PokemonFap* pokemon_fap) {
    gen3_menu_dialog_shown = false;
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

/* The Pokemon being edited lives until the app exits. Its nickname and moves come from the bulk
 * file, so it is created only once the file reads, and Trade, Edit, Load and Save need the file:
 * false if it can't be read. A read that failed since (the SD card pulled) closed it, so it is
 * opened again here first. */
bool gen3_menu_editor_ready(PokemonFap* pokemon_fap) {
    if(!gen3_bulk_ready()) gen3_bulk_furi_open();
    if(!gen3_bulk_ready()) return false;
    if(!pokemon_fap->gen3_editor) {
        pokemon_fap->gen3_editor = malloc(sizeof(Gen3Editor));
        if(!gen3_edit_init(pokemon_fap->gen3_editor, furi_hal_random_get())) {
            free(pokemon_fap->gen3_editor);
            pokemon_fap->gen3_editor = NULL;
        }
    }
    return pokemon_fap->gen3_editor != NULL;
}
