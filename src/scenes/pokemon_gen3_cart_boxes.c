#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* Party and boxes: the party with its count, then the save's 14 boxes by name, from the working
 * copy. Choosing one lists its slots. The scene state keeps the highlighted row. */

/* Row k's item: 0 the party, 1..14 the boxes */
#define GEN3_CART_ITEM_BOXES 0x47330520u
#define GEN3_CART_BOXES_ROWS (1u + GEN3_SAVE_BOXES)

static void gen3_cart_boxes_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

void pokemon_scene_gen3_cart_boxes_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Submenu* submenu = pokemon_fap->submenu;
    Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    char label[20]; /* "Party (6/6)", or a box name */

    submenu_reset(submenu);
    submenu_set_header(submenu, "Party and boxes");
    if(!gen3_menu_editor_ready(pokemon_fap)) {
        /* The bulk file, opened again if a read failed since, is what the lists below need */
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_EVENT_DATA_MISSING);
    } else if(c && gen3_cart_browsable(c)) {
        snprintf(label, sizeof(label), "Party (%u/6)", (unsigned)gen3_cart_party_count(c));
        submenu_add_item(
            submenu, label, GEN3_CART_ITEM_BOXES, gen3_cart_boxes_callback, pokemon_fap);
        for(uint8_t box = 0; box < GEN3_SAVE_BOXES; box++) {
            gen3_cart_ui_box_name(pokemon_fap, box, label);
            submenu_add_item(
                submenu,
                label,
                GEN3_CART_ITEM_BOXES + 1u + box,
                gen3_cart_boxes_callback,
                pokemon_fap);
        }
        submenu_set_selected_item(
            submenu,
            scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartBoxes));
    } else {
        /* The working copy went (a failed put, say): nothing to list */
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_EVENT_HUB);
    }
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_cart_boxes_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false; /* Back: the hub */
    if(event.event == GEN3_CART_EVENT_HUB) {
        scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Cart);
        consumed = true;
    } else if(event.event == GEN3_EVENT_DATA_MISSING) {
        gen3_menu_show_data_missing(pokemon_fap);
        consumed = true;
    } else if(
        event.event >= GEN3_CART_ITEM_BOXES &&
        event.event < GEN3_CART_ITEM_BOXES + GEN3_CART_BOXES_ROWS) {
        const uint32_t row = event.event - GEN3_CART_ITEM_BOXES;
        scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartBoxes, event.event);
        gen3_cart_ui_get(pokemon_fap)->box = row == 0 ? GEN3_SAVE_PARTY : (uint8_t)(row - 1u);
        /* a list opened from here starts at its top */
        scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartSlots, 0);
        scene_manager_next_scene(scene_manager, PokemonSceneGen3CartSlots);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_cart_boxes_on_exit(void* context) {
    UNUSED(context);
}
