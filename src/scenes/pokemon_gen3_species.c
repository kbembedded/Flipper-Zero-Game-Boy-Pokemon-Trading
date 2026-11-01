#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_edit.h>
#include <src/scenes/include/pokemon_gen3_menu.h>
#include <src/views/gen3_species_select.h>

static void gen3_species_picked(void* context, uint16_t national) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* A new species reads its name and moves from the bulk file; if that fails, nothing changed */
    bool ok = gen3_edit_set_species(pokemon_fap->gen3_editor, national);
    view_dispatcher_send_custom_event(
        pokemon_fap->view_dispatcher, ok ? (uint32_t)PokemonSceneBack : GEN3_EVENT_DATA_MISSING);
}

void pokemon_scene_gen3_species_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    gen3_species_select_start(
        pokemon_fap->gen3_species_select,
        gen3_edit_national(pokemon_fap->gen3_editor),
        gen3_species_picked,
        pokemon_fap);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewGen3SpeciesSelect);
}

bool pokemon_scene_gen3_species_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event == (uint32_t)PokemonSceneBack) {
        scene_manager_previous_scene(pokemon_fap->scene_manager);
        return true;
    }
    if(event.event == GEN3_EVENT_DATA_MISSING) {
        gen3_menu_show_data_missing(pokemon_fap);
        return true;
    }
    return false;
}

void pokemon_scene_gen3_species_on_exit(void* context) {
    UNUSED(context);
}
