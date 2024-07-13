#include <src/include/pokemon_app.h>
#include <src/include/pokemon_data.h>
#include <src/include/pokemon_char_encode.h>

#include <src/include/named_list.h>

#include <src/views/trade.h>
#include <src/views/select_pokemon.h>

#include <src/scenes/pokemon_scene.h>

#include <src/scenes/pokemon_menu.h>
#include <src/scenes/pokemon_shiny.h>
#include <src/scenes/pokemon_gender.h>
#include <src/scenes/pokemon_pokerus.h>
#include <src/scenes/unown_form.h>

static void scene_change_from_main_cb(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    /* Reuse of scenes to allow for using the same functions to set names */
    switch(index) {
    case PokemonSceneNickname:
    case PokemonSceneOTName:
    case PokemonSceneUnownForm:
        scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneNickname, index);
        break;
    case PokemonSceneLevel:
    case PokemonSceneOTID:
        scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneLevel, index);
        break;
    case PokemonSceneGender:
        if(select_gender_is_static(
               pokemon_fap->pdata,
               table_stat_base_get(
                   pokemon_fap->pdata->pokemon_table,
                   pokemon_stat_get(pokemon_fap->pdata, STAT_NUM, NONE),
                   STAT_BASE_GENDER_RATIO,
                   NONE)))
            return;
        break;
    }

    /* Set the navigation handler back to the basic one in the main menu. We only
     * want gen_back_event_callback from this menu as going back from the gen menu
     * means we need to free pdata.
     */
    view_dispatcher_set_navigation_event_callback(
        pokemon_fap->view_dispatcher, main_menu_back_event_callback);

    /* Set scene state to the current index so we can have that element highlighted when
     * we return.
     */
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGenITrade, index);
    scene_manager_next_scene(pokemon_fap->scene_manager, index);
}

bool gen_back_event_callback(void* context) {
    furi_assert(context);
    PokemonFap* pokemon_fap = context;

    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneExitConfirm);
    return true;
}

void pokemon_scene_gen_on_enter(void* context) {
    char buf[32];
    char name_buf[11]; // All name buffers are 11 bytes at most, including term
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    int pkmn_num;
    uint32_t state;

    // Set up trade party struct
    if(!pokemon_fap->pdata) {
        state = scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGenITrade);
        switch(state) {
        case PokemonSceneGenITrade:
            state = GEN_I;
            break;
        case PokemonSceneGenIITrade:
            state = GEN_II;
            break;
        default:
            state = 0;
            break;
        }
        pokemon_fap->pdata = pokemon_data_alloc(state);

        /* Clear the scene state as this is the first entry in to this scene
	 * we definitely want to be completely reset.
	 */
        scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGenITrade, 0);

        /* Allocate select and trade views */
        /* Allocates its own view and adds it to the main view_dispatcher */
        pokemon_fap->select = select_pokemon_alloc(
            pokemon_fap->pdata,
            pokemon_fap->view_dispatcher,
            pokemon_fap->scene_manager,
            AppViewSelectPokemon);

        // Trade View
        /* Allocates its own view and adds it to the main view_dispatcher */
        pokemon_fap->trade = trade_alloc(
            pokemon_fap->pdata, &pokemon_fap->pins, pokemon_fap->view_dispatcher, AppViewTrade);
    }

    pkmn_num = pokemon_stat_get(pokemon_fap->pdata, STAT_NUM, NONE);

    /* Clear the scene state of the Move scene since that is used to set the
     * highlighted menu item.
     * This could be done in move, but move would need its own custom exit handler
     * which is fine but would just waste a few more bytes compared to us handling
     * it here.
     */
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneMove, 0);
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneItemSet, 0);

    submenu_reset(pokemon_fap->submenu);

    snprintf(
        buf,
        sizeof(buf),
        "Pokemon:   %s",
        table_stat_name_get(pokemon_fap->pdata->pokemon_table, pkmn_num));
    submenu_add_item(
        pokemon_fap->submenu, buf, PokemonSceneSelect, scene_change_from_main_cb, pokemon_fap);

    pokemon_name_get(pokemon_fap->pdata, STAT_NICKNAME, name_buf, sizeof(name_buf));
    snprintf(buf, sizeof(buf), "Nickname:  %s", name_buf);
    submenu_add_item(
        pokemon_fap->submenu, buf, PokemonSceneNickname, scene_change_from_main_cb, pokemon_fap);

    snprintf(
        buf,
        sizeof(buf),
        "Level:           %d",
        pokemon_stat_get(pokemon_fap->pdata, STAT_LEVEL, NONE));
    submenu_add_item(
        pokemon_fap->submenu, buf, PokemonSceneLevel, scene_change_from_main_cb, pokemon_fap);

    if(pokemon_fap->pdata->gen == GEN_II) {
        snprintf(
            buf,
            sizeof(buf),
            "Held Item:   %s",
            namedlist_name_get_index(
                pokemon_fap->pdata->item_list,
                pokemon_stat_get(pokemon_fap->pdata, STAT_HELD_ITEM, NONE)));
        submenu_add_item(
            pokemon_fap->submenu, buf, PokemonSceneItem, scene_change_from_main_cb, pokemon_fap);
    }

    submenu_add_item(
        pokemon_fap->submenu,
        "Select Moves",
        PokemonSceneMove,
        scene_change_from_main_cb,
        pokemon_fap);

    if(pokemon_fap->pdata->gen == GEN_I) {
        submenu_add_item(
            pokemon_fap->submenu,
            "Select Types",
            PokemonSceneType,
            scene_change_from_main_cb,
            pokemon_fap);
    }

    submenu_add_item(
        pokemon_fap->submenu,
        namedlist_name_get_index(
            pokemon_fap->pdata->stat_list, pokemon_stat_get(pokemon_fap->pdata, STAT_SEL, NONE)),
        PokemonSceneStats,
        scene_change_from_main_cb,
        pokemon_fap);

    if(pokemon_fap->pdata->gen == GEN_II) {
        snprintf(
            buf,
            sizeof(buf),
            "Shiny:             %s",
            select_shiny_is_shiny(pokemon_fap->pdata) ? "Yes" : "No");
        submenu_add_item(
            pokemon_fap->submenu, buf, PokemonSceneShiny, scene_change_from_main_cb, pokemon_fap);

        snprintf(buf, sizeof(buf), "Gender:         %s", select_gender_get(pokemon_fap->pdata));
        submenu_add_item(
            pokemon_fap->submenu, buf, PokemonSceneGender, scene_change_from_main_cb, pokemon_fap);

        snprintf(buf, sizeof(buf), "Pokerus:       %s", select_pokerus_status(pokemon_fap));
        submenu_add_item(
            pokemon_fap->submenu, buf, PokemonScenePokerus, scene_change_from_main_cb, pokemon_fap);

        if(pokemon_stat_get(pokemon_fap->pdata, STAT_NUM, NONE) == 0xC8) { // Unown
            snprintf(buf, sizeof(buf), "Unown Form: %c", unown_form_get(pokemon_fap->pdata));
            submenu_add_item(
                pokemon_fap->submenu,
                buf,
                PokemonSceneUnownForm,
                scene_change_from_main_cb,
                pokemon_fap);
        }
    }

    snprintf(
        buf,
        sizeof(buf),
        "OT ID#:          %05d",
        pokemon_stat_get(pokemon_fap->pdata, STAT_OT_ID, NONE));
    submenu_add_item(
        pokemon_fap->submenu, buf, PokemonSceneOTID, scene_change_from_main_cb, pokemon_fap);

    pokemon_name_get(pokemon_fap->pdata, STAT_OT_NAME, name_buf, sizeof(name_buf));
    snprintf(buf, sizeof(buf), "OT Name:      %s", name_buf);
    submenu_add_item(
        pokemon_fap->submenu, buf, PokemonSceneOTName, scene_change_from_main_cb, pokemon_fap);

    submenu_add_item(
        pokemon_fap->submenu, "Trade PKMN", PokemonSceneTrade, scene_change_from_main_cb, pokemon_fap);

    /* TODO: Add Save pokemon option here */

    /* HACK: No matter what gen were in, we just store the scene state in PokemonSceneGenITrade */
    submenu_set_selected_item(
        pokemon_fap->submenu,
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGenITrade));

    view_dispatcher_set_navigation_event_callback(
        pokemon_fap->view_dispatcher, gen_back_event_callback);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewMainMenu);
}

bool pokemon_scene_gen_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pokemon_scene_gen_on_exit(void* context) {
    UNUSED(context);
}
