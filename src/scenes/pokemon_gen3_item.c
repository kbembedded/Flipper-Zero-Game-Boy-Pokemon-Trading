#include <furi.h>
#include <gui/modules/submenu.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_letters.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* Gen III held item. Gen3ItemLetter: the scene state is the chosen letter. Item names come from the
 * bulk file: a list that can't be read sends GEN3_EVENT_DATA_MISSING. */

#define GEN3_ITEM_NONE 0 /* the "No Item" row; letters are never 0 */

static const Gen3NameList gen3_item_list = {
    GEN3_HOLDABLE_COUNT,
    gen3_holdable_by_name_get,
    gen3_item_name_get,
};

static void gen3_item_done(PokemonFap* pokemon_fap) {
    view_dispatcher_send_custom_event(
        pokemon_fap->view_dispatcher, PokemonSceneGen3Edit | PokemonSceneSearch);
}

static bool gen3_item_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    if(event.type != SceneManagerEventTypeCustom) return false;

    /* A list that couldn't be read; not a scene to open */
    if(event.event == GEN3_EVENT_DATA_MISSING) {
        gen3_menu_show_data_missing(pokemon_fap);
        return true;
    }
    if(event.event & PokemonSceneSearch)
        scene_manager_search_and_switch_to_previous_scene(
            pokemon_fap->scene_manager, event.event & ~(uint32_t)PokemonSceneSearch);
    else
        scene_manager_next_scene(pokemon_fap->scene_manager, event.event);
    return true;
}

/* Gen3ItemLetter */

static void gen3_item_letter_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    if(index == GEN3_ITEM_NONE) {
        gen3_edit_set_item(pokemon_fap->gen3_editor, 0);
        gen3_item_done(pokemon_fap);
        return;
    }

    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3ItemLetter, index);
    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, PokemonSceneGen3ItemSet);
}

void pokemon_scene_gen3_item_letter_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    char letters[GEN3_LETTERS_MAX];
    char label[2] = {0, 0};
    /* About 462 reads from the bulk file: an id and a name for each holdable item */
    size_t n = gen3_letters(&gen3_item_list, letters, sizeof(letters));

    submenu_reset(pokemon_fap->submenu);
    submenu_add_item(
        pokemon_fap->submenu, "No Item", GEN3_ITEM_NONE, gen3_item_letter_callback, pokemon_fap);
    if(n == 0)
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_EVENT_DATA_MISSING);
    for(size_t i = 0; i < n; i++) {
        label[0] = letters[i];
        submenu_add_item(
            pokemon_fap->submenu,
            label,
            (uint32_t)(uint8_t)letters[i],
            gen3_item_letter_callback,
            pokemon_fap);
    }

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_item_letter_on_event(void* context, SceneManagerEvent event) {
    return gen3_item_on_event(context, event);
}

void pokemon_scene_gen3_item_letter_on_exit(void* context) {
    UNUSED(context);
}

/* Gen3ItemSet */

static void gen3_item_set_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    gen3_edit_set_item(pokemon_fap->gen3_editor, (uint16_t)index);
    gen3_item_done(pokemon_fap);
}

void pokemon_scene_gen3_item_set_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    char letter = (char)scene_manager_get_scene_state(
        pokemon_fap->scene_manager, PokemonSceneGen3ItemLetter);
    static uint16_t ids[GEN3_LETTER_IDS_MAX];
    char name[GEN3_ITEM_NAME_LEN + 1];
    /* Every letter shown has an item, so 0 means the list couldn't be read */
    size_t n = gen3_letter_ids(&gen3_item_list, letter, ids, GEN3_LETTER_IDS_MAX);
    bool ok = n > 0;

    submenu_reset(pokemon_fap->submenu);
    for(size_t i = 0; ok && i < n; i++) {
        ok = gen3_item_name_get(ids[i], name);
        if(ok)
            submenu_add_item(
                pokemon_fap->submenu, name, ids[i], gen3_item_set_callback, pokemon_fap);
    }
    if(!ok)
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_EVENT_DATA_MISSING);
    submenu_set_selected_item(
        pokemon_fap->submenu, gen3_pkm_get(&pokemon_fap->gen3_editor->pkm, Gen3FieldHeldItem));

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_item_set_on_event(void* context, SceneManagerEvent event) {
    return gen3_item_on_event(context, event);
}

void pokemon_scene_gen3_item_set_on_exit(void* context) {
    UNUSED(context);
}
