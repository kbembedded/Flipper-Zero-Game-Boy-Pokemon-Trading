#include <gui/modules/submenu.h>

#include <src/include/pokemon_app.h>
#include <src/include/pokemon_data.h>
#include <src/include/pokemon_attribute.h>

#include <src/scenes/include/pokemon_scene.h>

static void select_gender_selected_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    pokemon_gender_set(pokemon_fap->pdata, index);

    scene_manager_previous_scene(pokemon_fap->scene_manager);
}

void pokemon_scene_select_gender_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    submenu_reset(pokemon_fap->submenu);

    submenu_add_item(
        pokemon_fap->submenu, "Female", GENDER_FEMALE, select_gender_selected_callback, pokemon_fap);

    submenu_add_item(
        pokemon_fap->submenu, "Male", GENDER_MALE, select_gender_selected_callback, pokemon_fap);
}

bool pokemon_scene_select_gender_on_event(void* context, SceneManagerEvent event)
{
	UNUSED(event);
	UNUSED(context);

	return false;
}

void pokemon_scene_select_gender_on_exit(void* context) {
    UNUSED(context);
}
