#include <furi.h>
#include <gui/modules/submenu.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_letters.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* Gen III move slots. Gen3Moves: the scene state is the slot (0-3) and the highlight (row 4 is
 * "Default moves"). Gen3MoveLetter: the scene state is the chosen letter. Move names come from the
 * bulk file: a list that can't be read sends GEN3_EVENT_DATA_MISSING, a Move row shows "". */

#define GEN3_MOVES_DEFAULT_ROW 4

static const Gen3NameList gen3_move_list = {
    GEN3_MOVE_COUNT - 1,
    gen3_move_by_name_get,
    gen3_move_name_get,
};

static uint16_t gen3_move_in_slot(const Gen3Editor* ed, uint32_t slot) {
    return (uint16_t)gen3_pkm_get(&ed->pkm, (Gen3Field)(Gen3FieldMove1 + slot));
}

static uint32_t gen3_move_slot(const PokemonFap* pokemon_fap) {
    return scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Moves);
}

/* Back to the Moves screen, whatever is stacked above it */
static void gen3_move_done(PokemonFap* pokemon_fap) {
    view_dispatcher_send_custom_event(
        pokemon_fap->view_dispatcher, PokemonSceneGen3Moves | PokemonSceneSearch);
}

static bool gen3_move_on_event(void* context, SceneManagerEvent event) {
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

/* Gen3Moves */

static void gen3_moves_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Moves, index);
    view_dispatcher_send_custom_event(
        pokemon_fap->view_dispatcher,
        index == GEN3_MOVES_DEFAULT_ROW ? (uint32_t)PokemonSceneGen3Moves :
                                          (uint32_t)PokemonSceneGen3MoveLetter);
}

void pokemon_scene_gen3_moves_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    const Gen3Editor* ed = pokemon_fap->gen3_editor;
    char buf[32];
    char name[GEN3_MOVE_NAME_LEN + 1];

    submenu_reset(pokemon_fap->submenu);
    for(uint32_t i = 0; i < 4; i++) {
        uint16_t move = gen3_move_in_slot(ed, i);
        /* "" if the name can't be read, as the editor's Pokemon row */
        if(move) gen3_move_name_get(move, name);
        snprintf(buf, sizeof(buf), "Move %u:   %s", (unsigned)(i + 1), move ? name : "No Move");
        submenu_add_item(pokemon_fap->submenu, buf, i, gen3_moves_callback, pokemon_fap);
    }
    submenu_add_item(
        pokemon_fap->submenu,
        "Default moves",
        GEN3_MOVES_DEFAULT_ROW,
        gen3_moves_callback,
        pokemon_fap);

    submenu_set_selected_item(pokemon_fap->submenu, gen3_move_slot(pokemon_fap));
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_moves_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;

    /* The "Default moves" row sends this scene's own id: apply it and redraw the list. The
     * moveset comes from the bulk file; if it can't be read, the moves are kept. */
    if(event.type == SceneManagerEventTypeCustom && event.event == PokemonSceneGen3Moves) {
        if(gen3_edit_default_moves(pokemon_fap->gen3_editor))
            pokemon_scene_gen3_moves_on_enter(context);
        else
            gen3_menu_show_data_missing(pokemon_fap);
        return true;
    }
    return gen3_move_on_event(context, event);
}

void pokemon_scene_gen3_moves_on_exit(void* context) {
    UNUSED(context);
}

/* Gen3MoveLetter */

#define GEN3_MOVE_NONE 0 /* the "No Move" row; letters are never 0 */

static void gen3_move_letter_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    Gen3Editor* ed = pokemon_fap->gen3_editor;
    uint32_t slot = gen3_move_slot(pokemon_fap);

    if(index == GEN3_MOVE_NONE) {
        /* Clearing an empty slot changes nothing */
        if(gen3_move_in_slot(ed, slot) != 0) gen3_edit_set_move(ed, (uint8_t)slot, 0);
        gen3_move_done(pokemon_fap);
        return;
    }

    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3MoveLetter, index);
    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, PokemonSceneGen3MoveSet);
}

void pokemon_scene_gen3_move_letter_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    const Gen3Editor* ed = pokemon_fap->gen3_editor;
    uint32_t slot = gen3_move_slot(pokemon_fap);
    unsigned moves = 0;
    char letters[GEN3_LETTERS_MAX];
    char label[2] = {0, 0};

    for(uint32_t i = 0; i < 4; i++)
        if(gen3_move_in_slot(ed, i) != 0) moves++;

    submenu_reset(pokemon_fap->submenu);
    /* The last move can't be removed */
    if(!(moves == 1 && gen3_move_in_slot(ed, slot) != 0))
        submenu_add_item(
            pokemon_fap->submenu,
            "No Move",
            GEN3_MOVE_NONE,
            gen3_move_letter_callback,
            pokemon_fap);

    /* About 708 reads from the bulk file: an id and a name for each move */
    size_t n = gen3_letters(&gen3_move_list, letters, sizeof(letters));
    if(n == 0)
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_EVENT_DATA_MISSING);
    for(size_t i = 0; i < n; i++) {
        label[0] = letters[i];
        submenu_add_item(
            pokemon_fap->submenu,
            label,
            (uint32_t)(uint8_t)letters[i],
            gen3_move_letter_callback,
            pokemon_fap);
    }

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_move_letter_on_event(void* context, SceneManagerEvent event) {
    return gen3_move_on_event(context, event);
}

void pokemon_scene_gen3_move_letter_on_exit(void* context) {
    UNUSED(context);
}

/* Gen3MoveSet */

static void gen3_move_set_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    Gen3Editor* ed = pokemon_fap->gen3_editor;
    uint32_t slot = gen3_move_slot(pokemon_fap);

    /* The move already in the slot: nothing to do, and PP Ups stay */
    if(gen3_move_in_slot(ed, slot) != index) gen3_edit_set_move(ed, (uint8_t)slot, (uint16_t)index);
    gen3_move_done(pokemon_fap);
}

void pokemon_scene_gen3_move_set_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    const Gen3Editor* ed = pokemon_fap->gen3_editor;
    uint32_t slot = gen3_move_slot(pokemon_fap);
    char letter = (char)scene_manager_get_scene_state(
        pokemon_fap->scene_manager, PokemonSceneGen3MoveLetter);
    static uint16_t ids[GEN3_LETTER_IDS_MAX];
    char name[GEN3_MOVE_NAME_LEN + 1];
    /* Every letter shown has a move, so 0 means the list couldn't be read */
    size_t n = gen3_letter_ids(&gen3_move_list, letter, ids, GEN3_LETTER_IDS_MAX);
    bool ok = n > 0;

    submenu_reset(pokemon_fap->submenu);
    for(size_t i = 0; ok && i < n; i++) {
        bool elsewhere = false;

        /* Moves in another slot are left out */
        for(uint32_t s = 0; s < 4; s++)
            if(s != slot && gen3_move_in_slot(ed, s) == ids[i]) elsewhere = true;
        if(elsewhere) continue;
        ok = gen3_move_name_get(ids[i], name);
        if(ok)
            submenu_add_item(
                pokemon_fap->submenu, name, ids[i], gen3_move_set_callback, pokemon_fap);
    }
    if(!ok)
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_EVENT_DATA_MISSING);
    submenu_set_selected_item(pokemon_fap->submenu, gen3_move_in_slot(ed, slot));

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

bool pokemon_scene_gen3_move_set_on_event(void* context, SceneManagerEvent event) {
    return gen3_move_on_event(context, event);
}

void pokemon_scene_gen3_move_set_on_exit(void* context) {
    UNUSED(context);
}
