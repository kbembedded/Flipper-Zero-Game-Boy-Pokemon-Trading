#include <gui/modules/variable_item_list.h>
#include <furi.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_edit.h>

/* Pokerus: the same Strain / Days layout as pokemon_pokerus.c. The raw byte is strain << 4 | days.
 * A strain of 0 with days 0 means never infected, so the four strains are stored as the upper
 * nibble A = 4, B = 1, C = 2, D = 3, and None as the whole byte 0. */

#define GEN3_POKERUS_STRAINS 5 /* None, A, B, C, D */
#define GEN3_POKERUS_DAYS    16

static const char* const gen3_pokerus_strain_text[GEN3_POKERUS_STRAINS] =
    {"None", "A", "B", "C", "D"};

static const uint8_t gen3_pokerus_nibble[GEN3_POKERUS_STRAINS] = {0, 4, 1, 2, 3};

/* Row index of the stored strain nibble: 0 None, 1 A .. 4 D */
static uint8_t gen3_pokerus_strain_index(uint8_t value) {
    uint8_t nibble = value >> 4;

    /* A nibble of 4, 8 or 12 is also strain A: its low two bits are 0 */
    return nibble == 0 ? 0 : (uint8_t)((nibble & 3) + 1);
}

static void gen3_pokerus_rebuild(PokemonFap* pokemon_fap);

static void gen3_pokerus_write(PokemonFap* pokemon_fap, uint8_t strain, uint8_t days) {
    gen3_edit_set_pokerus(
        pokemon_fap->gen3_editor,
        strain == 0 ? 0 : (uint8_t)((gen3_pokerus_nibble[strain] << 4) | days));
}

static void gen3_pokerus_strain_callback(VariableItem* item) {
    PokemonFap* pokemon_fap = variable_item_get_context(item);
    uint8_t value = (uint8_t)gen3_pkm_get(&pokemon_fap->gen3_editor->pkm, Gen3FieldPokerus);

    gen3_pokerus_write(
        pokemon_fap, variable_item_get_current_value_index(item), value & 0x0F);
    gen3_pokerus_rebuild(pokemon_fap);
    variable_item_list_set_selected_item(pokemon_fap->variable_item_list, 0);
}

static void gen3_pokerus_days_callback(VariableItem* item) {
    PokemonFap* pokemon_fap = variable_item_get_context(item);
    uint8_t value = (uint8_t)gen3_pkm_get(&pokemon_fap->gen3_editor->pkm, Gen3FieldPokerus);

    gen3_pokerus_write(
        pokemon_fap, gen3_pokerus_strain_index(value), variable_item_get_current_value_index(item));
    gen3_pokerus_rebuild(pokemon_fap);
    variable_item_list_set_selected_item(pokemon_fap->variable_item_list, 1);
}

static void gen3_pokerus_rebuild(PokemonFap* pokemon_fap) {
    uint8_t value = (uint8_t)gen3_pkm_get(&pokemon_fap->gen3_editor->pkm, Gen3FieldPokerus);
    uint8_t strain = gen3_pokerus_strain_index(value);
    uint8_t days = value & 0x0F;
    char buf[4];
    VariableItem* item;

    variable_item_list_reset(pokemon_fap->variable_item_list);

    item = variable_item_list_add(
        pokemon_fap->variable_item_list,
        "Strain:",
        GEN3_POKERUS_STRAINS,
        gen3_pokerus_strain_callback,
        pokemon_fap);
    variable_item_set_current_value_index(item, strain);
    variable_item_set_current_value_text(item, gen3_pokerus_strain_text[strain]);

    /* Days mean nothing without a strain */
    item = variable_item_list_add(
        pokemon_fap->variable_item_list,
        "Days remain:",
        strain == 0 ? 0 : GEN3_POKERUS_DAYS,
        gen3_pokerus_days_callback,
        pokemon_fap);
    if(strain != 0) {
        snprintf(buf, sizeof(buf), "%u", (unsigned)days);
        variable_item_set_current_value_index(item, days);
        variable_item_set_current_value_text(item, buf);
    }
}

void pokemon_scene_gen3_pokerus_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    gen3_pokerus_rebuild(pokemon_fap);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewVariableItem);
}

bool pokemon_scene_gen3_pokerus_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pokemon_scene_gen3_pokerus_on_exit(void* context) {
    UNUSED(context);
}
