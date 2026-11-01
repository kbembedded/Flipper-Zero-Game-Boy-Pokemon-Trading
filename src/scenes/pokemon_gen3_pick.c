#include <furi.h>
#include <gui/modules/submenu.h>

#include <src/include/pokemon_app.h>
#include <src/include/named_list.h>
#include <src/include/stat_nl.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* IV/EV preset, nature, ability, shiny, gender, ball and Unown form: one list each. The scene it
 * is entered as is the scene state of Gen3Preset. */

/* The preset list shows stat_list, so a Gen3Preset must be the EvIv of the same name */
_Static_assert((int)Gen3PresetRandIvZeroEv == (int)RANDIV_ZEROEV, "preset order != EvIv");
_Static_assert((int)Gen3PresetRandIvLevelEv == (int)RANDIV_LEVELEV, "preset order != EvIv");
_Static_assert((int)Gen3PresetRandIvMaxEv == (int)RANDIV_MAXEV, "preset order != EvIv");
_Static_assert((int)Gen3PresetMaxIvZeroEv == (int)MAXIV_ZEROEV, "preset order != EvIv");
_Static_assert((int)Gen3PresetMaxIvLevelEv == (int)MAXIV_LEVELEV, "preset order != EvIv");
_Static_assert((int)Gen3PresetMaxIvMaxEv == (int)MAXIV_MAXEV, "preset order != EvIv");

/* Indexed by Gen3Stat; HP is never raised or lowered by a nature */
static const char* const gen3_pick_stat_names[Gen3StatCount] =
    {"HP", "Atk", "Def", "Spe", "SpA", "SpD"};

static void gen3_pick_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    Gen3Editor* ed = pokemon_fap->gen3_editor;
    uint32_t state =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Preset);
    Gen3Traits traits;
    bool trait = true;
    uint32_t done = (uint32_t)PokemonSceneBack;

    /* A PID trait: the current traits with one of them changed. gen3_edit_set_traits keeps the
     * PID when that is the value it already has. */
    gen3_traits_of(&ed->pkm, &traits);
    switch(state) {
    case PokemonSceneGen3Preset:
        gen3_edit_set_preset(ed, (Gen3Preset)index);
        trait = false;
        break;
    case PokemonSceneGen3Nature:
        traits.nature = (uint8_t)index;
        break;
    case PokemonSceneGen3Ability:
        traits.ability_slot = (uint8_t)index;
        break;
    case PokemonSceneGen3Shiny:
        traits.shiny = index != 0;
        break;
    case PokemonSceneGen3Gender:
        traits.gender = (Gen3Gender)index;
        break;
    case PokemonSceneGen3Unown:
        traits.unown_letter = (uint8_t)index;
        break;
    case PokemonSceneGen3Ball:
        gen3_edit_set_ball(ed, (uint8_t)index);
        trait = false;
        break;
    default:
        trait = false;
        break;
    }
    /* Every want from these lists is in range, so it fails only for a shiny Unown whose nature
     * and letter no shiny PID has for this OT. The Pokemon is unchanged then. */
    if(trait && !gen3_edit_set_traits(ed, &traits)) done = GEN3_EVENT_NO_SHINY;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, done);
}

static void gen3_pick_add(PokemonFap* pokemon_fap, const char* label, uint32_t index) {
    submenu_add_item(pokemon_fap->submenu, label, index, gen3_pick_callback, pokemon_fap);
}

/* "HARDY", or e.g. "LONELY +Atk -Def" */
static void gen3_pick_nature_label(uint8_t nature, char* out, size_t out_size) {
    const char* up = NULL;
    const char* down = NULL;

    for(int s = Gen3StatAtk; s < Gen3StatCount; s++) {
        int8_t effect = gen3_nature_effect(nature, (Gen3Stat)s);
        if(effect > 0) up = gen3_pick_stat_names[s];
        if(effect < 0) down = gen3_pick_stat_names[s];
    }
    if(up && down)
        snprintf(out, out_size, "%s +%s -%s", gen3_nature_name(nature), up, down);
    else
        snprintf(out, out_size, "%s", gen3_nature_name(nature));
}

void pokemon_scene_gen3_pick_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    const Gen3Editor* ed = pokemon_fap->gen3_editor;
    const Gen3SpeciesInfo* info = gen3_species(gen3_edit_national(ed));
    uint32_t state =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Preset);
    Gen3Traits traits;
    uint32_t selected;
    char label[24];
    /* The Ball list reads item names into label */
    _Static_assert(sizeof(label) >= GEN3_ITEM_NAME_LEN + 1, "label too small for an item name");

    gen3_traits_of(&ed->pkm, &traits);
    submenu_reset(pokemon_fap->submenu);

    switch(state) {
    case PokemonSceneGen3Preset:
        for(uint32_t i = 0; i < GEN3_PRESET_COUNT; i++)
            gen3_pick_add(pokemon_fap, namedlist_name_get_index(stat_list, i), i);
        selected = ed->preset;
        break;
    case PokemonSceneGen3Nature:
        for(uint8_t n = 0; n < GEN3_NATURE_COUNT; n++) {
            gen3_pick_nature_label(n, label, sizeof(label));
            gen3_pick_add(pokemon_fap, label, n);
        }
        selected = traits.nature;
        break;
    case PokemonSceneGen3Ability:
        /* Only entered for a two-ability species */
        gen3_pick_add(pokemon_fap, gen3_ability_name(info ? info->ability[0] : 0), 0);
        gen3_pick_add(pokemon_fap, gen3_ability_name(info ? info->ability[1] : 0), 1);
        selected = traits.ability_slot;
        break;
    case PokemonSceneGen3Shiny:
        gen3_pick_add(pokemon_fap, "Shiny", 1);
        gen3_pick_add(pokemon_fap, "Not Shiny", 0);
        selected = traits.shiny;
        break;
    case PokemonSceneGen3Gender:
        gen3_pick_add(pokemon_fap, "Female", Gen3GenderFemale);
        gen3_pick_add(pokemon_fap, "Male", Gen3GenderMale);
        selected = traits.gender;
        break;
    case PokemonSceneGen3Ball:
        /* Ball names are item names, from the bulk file */
        for(uint8_t ball = 1; ball <= GEN3_EDIT_BALL_MAX; ball++) {
            if(!gen3_item_name_get(ball, label)) {
                view_dispatcher_send_custom_event(
                    pokemon_fap->view_dispatcher, GEN3_EVENT_DATA_MISSING);
                break;
            }
            gen3_pick_add(pokemon_fap, label, ball);
        }
        selected = gen3_pkm_get(&ed->pkm, Gen3FieldBall);
        break;
    case PokemonSceneGen3Unown:
        for(uint8_t letter = 0; letter < GEN3_UNOWN_LETTER_COUNT; letter++) {
            label[0] = gen3_unown_char(letter);
            label[1] = '\0';
            gen3_pick_add(pokemon_fap, label, letter);
        }
        selected = traits.unown_letter;
        break;
    default:
        furi_crash("Gen3 pick: invalid state");
        break;
    }

    submenu_set_selected_item(pokemon_fap->submenu, selected);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_pick_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    /* Back on the list or the notice is left to the scene manager: the editor menu */
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == (uint32_t)PokemonSceneBack || event.event == GEN3_EVENT_NOTICE_OK) {
            scene_manager_previous_scene(pokemon_fap->scene_manager);
            consumed = true;
        } else if(event.event == GEN3_EVENT_NO_SHINY) {
            gen3_menu_show_notice(
                pokemon_fap, "No shiny PID", "for this letter,\nnature and OT ID.");
            consumed = true;
        } else if(event.event == GEN3_EVENT_DATA_MISSING) {
            /* The Ball list couldn't be read */
            gen3_menu_show_data_missing(pokemon_fap);
            consumed = true;
        }
    }

    return consumed;
}

void pokemon_scene_gen3_pick_on_exit(void* context) {
    UNUSED(context);
}
