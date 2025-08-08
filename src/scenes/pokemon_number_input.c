#include <furi.h>
#include <gui/modules/text_input.h>
#include <gui/view_dispatcher.h>
#include <stdlib.h>

#include <src/include/pokemon_app.h>
#include <src/include/pokemon_data.h>

#include <src/scenes/include/pokemon_scene.h>

static void select_number_input_callback(void* context, int32_t number) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    DataStat stat;
    uint32_t state = scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneLevel);

    switch(state) {
    case PokemonSceneLevel:
        stat = STAT_LEVEL;
        break;
    case PokemonSceneOTID:
        stat = STAT_OT_ID;
        break;
    default:
        furi_crash("Num: invalid state 2");
    }

    pokemon_stat_set(pokemon_fap->pdata, stat, NONE, number);

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, PokemonSceneBack);
}

void pokemon_scene_select_number_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    char* header;
    uint32_t state = scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneLevel);
    int32_t min;
    int32_t max;
    DataStat stat;

    switch(state) {
    case PokemonSceneLevel:
        header = "Enter level (numbers only)";
        min = 2;
        max = 100;
        stat = STAT_LEVEL;
        break;
    case PokemonSceneOTID:
        header = "Enter OT ID (numbers only)";
        min = 0;
        max = 65535;
        stat = STAT_OT_ID;
        break;
    default:
        furi_crash("Num: invalid state");
        break;
    }

    number_input_set_result_callback(
        pokemon_fap->number_input, select_number_input_callback, pokemon_fap, (int32_t)pokemon_stat_get(pokemon_fap->pdata, stat, NONE), min, max);
    number_input_set_header_text(pokemon_fap->number_input, header);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewNumberInput);
}

bool pokemon_scene_select_number_on_event(void* context, SceneManagerEvent event) {
    furi_assert(context);
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if (event.type == SceneManagerEventTypeCustom && event.event & PokemonSceneBack) {
        scene_manager_previous_scene(pokemon_fap->scene_manager);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_select_number_on_exit(void* context) {
    UNUSED(context);
}
