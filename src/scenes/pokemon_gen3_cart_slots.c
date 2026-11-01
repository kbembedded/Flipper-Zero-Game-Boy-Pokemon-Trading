#include <src/scenes/include/pokemon_gen3_cart.h>

#include <string.h>

#include <furi.h>
#include <src/gen3/gen3_data.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* One box's slots, or the party's, headed by its name: "07 TREECKO Lv5", "07 EGG", "07 BAD EGG",
 * "07 -- empty --", with '*' after a slot changed and not written. The party lists its Pokemon,
 * then its next slot while it has fewer than six. Each row is read from the working copy into the
 * ui's 100-byte buffer, one at a time. Choosing a row opens the slot router. The scene state keeps
 * the highlighted row. */

/* Slot k's item */
#define GEN3_CART_ITEM_SLOTS 0x47330540u

static void gen3_cart_slots_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

/* The rows, read one by one; false if a species name couldn't be read (the bulk file) */
static bool gen3_cart_slots_rows(PokemonFap* pokemon_fap, Gen3Cart* c) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const bool party = ui->box == GEN3_SAVE_PARTY;
    const uint8_t count = party ? gen3_cart_party_count(c) : 0;
    const uint8_t rows = !party                      ? GEN3_SAVE_BOX_SLOTS :
                         count < GEN3_SAVE_PARTY_MAX ? (uint8_t)(count + 1u) :
                                                       GEN3_SAVE_PARTY_MAX;
    char label[GEN3_CART_SCREEN_ROW_SIZE];
    char species[GEN3_SPECIES_NAME_LEN + 1];
    Gen3MonSummary s;

    for(uint8_t k = 0; k < rows; k++) {
        const Gen3MonSummary* row = NULL;
        size_t len = 0;
        species[0] = '\0';
        if(party && k == count) {
            memset(&s, 0, sizeof(s));
            s.kind = Gen3MonEmpty;
            row = &s;
        } else if(gen3_cart_mon_get(c, ui->box, k, ui->mon, &len) == Gen3CartOk) {
            gen3_save_mon_summary(ui->mon, len, &s);
            row = &s;
            if(s.kind == Gen3MonOk && !gen3_species_name_get(s.national, species)) return false;
        }
        gen3_cart_screen_row(k, row, species, gen3_cart_mon_changed(c, ui->box, k), label);
        submenu_add_item(
            pokemon_fap->submenu,
            label,
            GEN3_CART_ITEM_SLOTS + k,
            gen3_cart_slots_callback,
            pokemon_fap);
    }
    return true;
}

void pokemon_scene_gen3_cart_slots_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Submenu* submenu = pokemon_fap->submenu;
    Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    char name[9];

    submenu_reset(submenu);
    if(!c || !gen3_cart_browsable(c)) {
        /* The working copy went (a failed put, say): nothing to list */
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_EVENT_HUB);
    } else {
        gen3_cart_ui_box_name(pokemon_fap, gen3_cart_ui_get(pokemon_fap)->box, name);
        submenu_set_header(submenu, name);
        /* the bulk file opened again if a read failed since, for the species names */
        if(!gen3_menu_editor_ready(pokemon_fap) || !gen3_cart_slots_rows(pokemon_fap, c)) {
            /* The edits stay in the working copy, below the hub */
            view_dispatcher_send_custom_event(
                pokemon_fap->view_dispatcher, GEN3_EVENT_DATA_MISSING);
        }
        submenu_set_selected_item(
            submenu,
            scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartSlots));
    }
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_cart_slots_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    bool consumed = false;

    if(event.type != SceneManagerEventTypeCustom) return false; /* Back: the boxes */
    if(event.event == GEN3_CART_EVENT_HUB) {
        scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Cart);
        consumed = true;
    } else if(event.event == GEN3_EVENT_DATA_MISSING) {
        gen3_menu_show_data_missing(pokemon_fap);
        consumed = true;
    } else if(
        event.event >= GEN3_CART_ITEM_SLOTS &&
        event.event < GEN3_CART_ITEM_SLOTS + GEN3_SAVE_BOX_SLOTS) {
        Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
        scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartSlots, event.event);
        ui->slot = (uint8_t)(event.event - GEN3_CART_ITEM_SLOTS);
        /* The router starts afresh: nothing the editor holds goes into this slot unopened */
        ui->edit_mode = Gen3CartEditNone;
        scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartSlot, 0);
        scene_manager_next_scene(scene_manager, PokemonSceneGen3CartSlot);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_cart_slots_on_exit(void* context) {
    UNUSED(context);
}
