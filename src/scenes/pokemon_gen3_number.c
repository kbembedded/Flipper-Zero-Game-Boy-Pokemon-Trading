#include <furi.h>
#include <gui/modules/text_input.h>
#include <gui/view_dispatcher.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_edit.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* Level, OT TID and OT SID. The scene it is entered as is the scene state of Gen3Level. */

static char gen3_level_buf[4]; /* "100" */
static char gen3_id_buf[6]; /* "65535" */
/* The new TID or SID was applied, but the Pokemon is no longer shiny (Gen3OtIdShinyLost) */
static bool gen3_number_shiny_lost = false;

static bool gen3_number_validator(const char* text, FuriString* error, void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    Gen3Editor* ed = pokemon_fap->gen3_editor;
    uint32_t state =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Level);
    bool level = state == PokemonSceneGen3Level;
    uint32_t min = level ? 1 : 0;
    uint32_t max = level ? 100 : 65535;
    uint32_t ot_id = gen3_pkm_get(&ed->pkm, Gen3FieldOtId);
    uint32_t number = 0;
    bool ok = text[0] != '\0';

    /* Digits only. The buffers hold at most 5 digits, so this can't overflow. */
    for(size_t i = 0; ok && text[i] != '\0'; i++) {
        if(text[i] < '0' || text[i] > '9')
            ok = false;
        else
            number = number * 10 + (uint32_t)(text[i] - '0');
    }

    if(!ok || number < min || number > max) {
        furi_string_set(error, level ? "Level must\nbe 1-100!" : "ID must\nbe 0-65535!");
        return false;
    }

    /* The other half of the OT ID is kept. The level setter does nothing for the current level,
     * so confirming it unchanged keeps exp and HP. */
    Gen3OtIdResult result = Gen3OtIdOk;
    if(level)
        gen3_edit_set_level(ed, (uint8_t)number);
    else if(state == PokemonSceneGen3Tid)
        result = gen3_edit_set_ot_id(ed, (uint16_t)number, (uint16_t)(ot_id >> 16));
    else
        result = gen3_edit_set_ot_id(ed, (uint16_t)ot_id, (uint16_t)number);
    gen3_number_shiny_lost = result == Gen3OtIdShinyLost;

    return true;
}

static void gen3_number_result_callback(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, PokemonSceneBack);
}

void pokemon_scene_gen3_number_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    const Gen3Pkm* p = &pokemon_fap->gen3_editor->pkm;
    uint32_t state =
        scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Level);
    uint32_t ot_id = gen3_pkm_get(p, Gen3FieldOtId);
    const char* header;
    char* buf;
    size_t buf_size;

    switch(state) {
    case PokemonSceneGen3Level:
        header = "Enter level (numbers only)";
        buf = gen3_level_buf;
        buf_size = sizeof(gen3_level_buf);
        snprintf(buf, buf_size, "%u", (unsigned)(uint8_t)gen3_pkm_get(p, Gen3FieldLevel));
        break;
    case PokemonSceneGen3Tid:
        header = "Enter TID (numbers only)";
        buf = gen3_id_buf;
        buf_size = sizeof(gen3_id_buf);
        snprintf(buf, buf_size, "%u", (unsigned)(uint16_t)ot_id);
        break;
    case PokemonSceneGen3Sid:
        header = "Enter SID (numbers only)";
        buf = gen3_id_buf;
        buf_size = sizeof(gen3_id_buf);
        snprintf(buf, buf_size, "%u", (unsigned)(uint16_t)(ot_id >> 16));
        break;
    default:
        furi_crash("Gen3 num: invalid state");
        break;
    }

    gen3_number_shiny_lost = false;
    text_input_reset(pokemon_fap->text_input);
    text_input_set_validator(pokemon_fap->text_input, gen3_number_validator, pokemon_fap);
    text_input_set_result_callback(
        pokemon_fap->text_input, gen3_number_result_callback, pokemon_fap, buf, buf_size, true);
    text_input_set_header_text(pokemon_fap->text_input, header);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewTextInput);
}

bool pokemon_scene_gen3_number_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    /* Back on the keyboard or the notice is left to the scene manager: the editor menu */
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == (uint32_t)PokemonSceneBack && gen3_number_shiny_lost) {
            gen3_number_shiny_lost = false;
            gen3_menu_show_notice(pokemon_fap, "Shiny lost", "No shiny PID\nfor this OT ID.");
            consumed = true;
        } else if(event.event == (uint32_t)PokemonSceneBack || event.event == GEN3_EVENT_NOTICE_OK) {
            scene_manager_previous_scene(pokemon_fap->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void pokemon_scene_gen3_number_on_exit(void* context) {
    UNUSED(context);
}
