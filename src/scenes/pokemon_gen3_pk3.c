#include <furi.h>
#include <dialogs/dialogs.h>
#include <gui/modules/dialog_ex.h>
#include <storage/storage.h>

#include <src/include/pokemon_app.h>

#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_pk3_furi.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* The .pk3 scenes: Gen3Load (file browser, then the result), Gen3Save (overwrite check, then the
 * result) and Gen3Result (what happened). Gen3SaveName is the text scene, pokemon_gen3_text.c. */

#define GEN3_RESULT_HEADER_MAX 24
#define GEN3_RESULT_TEXT_MAX   96

/* DialogEx keeps the pointers it is given, so these outlive the scene */
static char gen3_result_header[GEN3_RESULT_HEADER_MAX + 1];
static char gen3_result_text[GEN3_RESULT_TEXT_MAX + 1];
static char gen3_overwrite_text[GEN3_PK3_NAME_MAX + 5]; /* name + ".pk3" + NUL */

static void gen3_pk3_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, result);
}

void gen3_result_set(const char* header, const char* text) {
    snprintf(gen3_result_header, sizeof(gen3_result_header), "%s", header);
    snprintf(gen3_result_text, sizeof(gen3_result_text), "%s", text);
}

static void gen3_pk3_show_result(PokemonFap* pokemon_fap, const char* header, const char* text) {
    gen3_result_set(header, text);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3Result);
}

/* ---- Load ---- */

/* What the load changed in the file, briefly, for the result text. "" if nothing. */
void gen3_load_changes(const Gen3ImportInfo* info, char* out, size_t out_size) {
    const Gen3NormaliseInfo* n = &info->normalise;
    bool moves = n->moves_defaulted || n->duplicates_removed || n->gaps_packed;
    /* A held Mail item, or a stale mail index in a 100-byte file */
    bool mail = info->mail_removed || n->mail_cleared;

    if(!mail && !moves && !n->exp_clamped && !n->ability_fixed) {
        out[0] = '\0';
        return;
    }
    /* Stats are always recalculated from exp, IVs, EVs and nature. At most
     * "\nFixed: mail moves level ability" (32 characters); with a 10-character nickname and
     * species name and "Lv. 100" the text is 61 characters, under GEN3_RESULT_TEXT_MAX. */
    snprintf(
        out,
        out_size,
        "\nFixed:%s%s%s%s",
        mail ? " mail" : "",
        moves ? " moves" : "",
        n->exp_clamped ? " level" : "",
        n->ability_fixed ? " ability" : "");
}

/* The text is formatted straight into the result scene's buffer. */
static void gen3_load_show_loaded(PokemonFap* pokemon_fap, const Gen3ImportInfo* info) {
    const Gen3Pkm* p = &pokemon_fap->gen3_editor->pkm;
    char nick[GEN3_NICKNAME_LEN + 1];
    char species[GEN3_SPECIES_NAME_LEN + 1];
    char changes[40];

    gen3_pkm_nickname(p, nick, sizeof(nick));
    gen3_species_name_get(gen3_edit_national(pokemon_fap->gen3_editor), species);
    gen3_load_changes(info, changes, sizeof(changes));
    snprintf(gen3_result_header, sizeof(gen3_result_header), "Loaded");
    snprintf(
        gen3_result_text,
        sizeof(gen3_result_text),
        "%s\n%s Lv. %u%s",
        nick,
        species,
        (unsigned)gen3_pkm_get(p, Gen3FieldLevel),
        changes);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3Result);
}

/* Own function, so the Pokemon and the text buffers aren't in load_on_enter's frame, which is
 * held while the file browser runs. */
__attribute__((noinline)) static void
    gen3_load_picked(PokemonFap* pokemon_fap, Storage* storage, const char* path) {
    Gen3Pkm pkm;
    Gen3ImportInfo info;
    Gen3Check check = gen3_pk3_load_file(storage, path, &pkm, &info);

    if(check == Gen3CheckOk && !gen3_edit_load(pokemon_fap->gen3_editor, &pkm)) {
        /* The import read the bulk file a moment ago; it can't any more */
        check = Gen3CheckNoData;
    }

    if(check == Gen3CheckOk) {
        gen3_load_show_loaded(pokemon_fap, &info);
    } else {
        gen3_pk3_show_result(pokemon_fap, "Load failed", gen3_check_text(check));
    }
}

void pokemon_scene_gen3_load_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = furi_string_alloc();
    FuriString* path = furi_string_alloc();

    if(!gen3_pk3_dir(storage, dir)) {
        gen3_pk3_show_result(pokemon_fap, "Load failed", "No SD card");
    } else {
        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options, ".pk3", NULL);
        options.base_path = furi_string_get_cstr(dir);
        bool picked = dialog_file_browser_show(dialogs, path, dir, &options);
        furi_record_close(RECORD_DIALOGS);

        if(!picked) {
            scene_manager_previous_scene(pokemon_fap->scene_manager);
        } else {
            gen3_load_picked(pokemon_fap, storage, furi_string_get_cstr(path));
        }
    }

    furi_string_free(path);
    furi_string_free(dir);
    furi_record_close(RECORD_STORAGE);
}

bool pokemon_scene_gen3_load_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pokemon_scene_gen3_load_on_exit(void* context) {
    UNUSED(context);
}

/* ---- Save ---- */

static void gen3_save_do(PokemonFap* pokemon_fap, Storage* storage, const char* path) {
    char name[GEN3_PK3_NAME_MAX + 5];

    snprintf(name, sizeof(name), "%s.pk3", pokemon_fap->gen3_file_name);
    if(gen3_pk3_save_file(storage, path, &pokemon_fap->gen3_editor->pkm)) {
        gen3_pk3_show_result(pokemon_fap, "Saved", name);
    } else {
        gen3_pk3_show_result(pokemon_fap, "Save failed", "Check the SD card");
    }
}

/* <dir>/<name>.pk3, false without an SD card */
static bool gen3_save_path(PokemonFap* pokemon_fap, Storage* storage, FuriString* path) {
    if(!gen3_pk3_dir(storage, path)) return false;
    furi_string_cat_printf(path, "/%s.pk3", pokemon_fap->gen3_file_name);
    return true;
}

void pokemon_scene_gen3_save_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* path = furi_string_alloc();

    if(!gen3_save_path(pokemon_fap, storage, path)) {
        gen3_pk3_show_result(pokemon_fap, "Save failed", "No SD card");
    } else if(storage_file_exists(storage, furi_string_get_cstr(path))) {
        DialogEx* dialog_ex = pokemon_fap->dialog_ex;

        snprintf(
            gen3_overwrite_text,
            sizeof(gen3_overwrite_text),
            "%s.pk3",
            pokemon_fap->gen3_file_name);
        dialog_ex_reset(dialog_ex);
        dialog_ex_set_header(dialog_ex, "Overwrite?", 64, 0, AlignCenter, AlignTop);
        dialog_ex_set_text(dialog_ex, gen3_overwrite_text, 64, 24, AlignCenter, AlignTop);
        dialog_ex_set_left_button_text(dialog_ex, "Cancel");
        dialog_ex_set_right_button_text(dialog_ex, "Overwrite");
        dialog_ex_set_context(dialog_ex, pokemon_fap);
        dialog_ex_set_result_callback(dialog_ex, gen3_pk3_dialog_callback);
        view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
    } else {
        gen3_save_do(pokemon_fap, storage, furi_string_get_cstr(path));
    }

    furi_string_free(path);
    furi_record_close(RECORD_STORAGE);
}

bool pokemon_scene_gen3_save_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == DialogExResultLeft) {
            consumed = scene_manager_previous_scene(pokemon_fap->scene_manager);
        } else if(event.event == DialogExResultRight) {
            Storage* storage = furi_record_open(RECORD_STORAGE);
            FuriString* path = furi_string_alloc();

            if(gen3_save_path(pokemon_fap, storage, path))
                gen3_save_do(pokemon_fap, storage, furi_string_get_cstr(path));
            else
                gen3_pk3_show_result(pokemon_fap, "Save failed", "No SD card");

            furi_string_free(path);
            furi_record_close(RECORD_STORAGE);
            consumed = true;
        }
    }
    /* Back is left to the scene manager: the file name input */

    return consumed;
}

void pokemon_scene_gen3_save_on_exit(void* context) {
    UNUSED(context);
}

/* ---- Result ---- */

void pokemon_scene_gen3_result_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_header(dialog_ex, gen3_result_header, 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog_ex, gen3_result_text, 64, 14, AlignCenter, AlignTop);
    dialog_ex_set_center_button_text(dialog_ex, "OK");
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_pk3_dialog_callback);

    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

bool pokemon_scene_gen3_result_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if((event.type == SceneManagerEventTypeCustom && event.event == DialogExResultCenter) ||
       event.type == SceneManagerEventTypeBack) {
        scene_manager_search_and_switch_to_previous_scene(
            pokemon_fap->scene_manager, PokemonSceneGen3Menu);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_result_on_exit(void* context) {
    UNUSED(context);
}
