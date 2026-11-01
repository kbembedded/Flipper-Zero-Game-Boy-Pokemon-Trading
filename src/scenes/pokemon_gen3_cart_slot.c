#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <dialogs/dialogs.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <storage/storage.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_pk3_furi.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* The slot router: a row chosen in the list lands here. It decides in on_enter, and moves on from
 * on_event, on a custom event it sends itself:
 * - a Pokemon: opened in the editor (gen3_cart_mon_open), as Existing; back from the editor, put
 *   into its slot if it was edited (gen3_cart_mon_close), then the list again. One the game or the
 *   Flipper couldn't take back whole (Mail, an egg, an item Ruby hasn't, ...) isn't opened.
 * - a free slot: New Pokemon or Load .pk3, then the editor; back from it, Put? (Gen3CartPut).
 * Only gen3_cart_mon_close and gen3_cart_mon_place put a Pokemon, and only after the editor
 * loaded or made it (edit_mode not None), so whatever the editor held before never reaches a slot.
 * The slot is the ui's box and slot. */

/* The scene state is a Gen3CartSlotState (gen3_cart_screen.h), which decides on entering
 * (gen3_cart_screen_slot_entry) */

/* Its events to itself, and the free slot's items */
#define GEN3_CART_SLOT_EVENT_EDIT 0x47330560u
#define GEN3_CART_SLOT_EVENT_RESULT 0x47330561u
#define GEN3_CART_SLOT_EVENT_LIST 0x47330562u
#define GEN3_CART_SLOT_EVENT_PUT 0x47330563u
#define GEN3_CART_SLOT_ITEM_NEW 0x47330564u
#define GEN3_CART_SLOT_ITEM_LOAD 0x47330565u

static void gen3_cart_slot_set_state(PokemonFap* pokemon_fap, uint32_t state) {
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartSlot, state);
}

static uint32_t gen3_cart_slot_state(PokemonFap* pokemon_fap) {
    return scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartSlot);
}

static void gen3_cart_slot_send(PokemonFap* pokemon_fap, uint32_t event) {
    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, event);
}

static void gen3_cart_slot_callback(void* context, uint32_t index) {
    gen3_cart_slot_send(context, index);
}

/* The editor, on the Pokemon just opened, made or loaded, from its first row */
static void gen3_cart_slot_to_editor(PokemonFap* pokemon_fap) {
    gen3_cart_slot_set_state(pokemon_fap, Gen3CartSlotEdit);
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Edit, 0);
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3Edit);
}

/* A free slot: what to put there */
static void gen3_cart_slot_menu(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Submenu* submenu = pokemon_fap->submenu;
    char box[9];
    char header[24]; /* "ABCDEFGH slot 30": 16 characters */

    gen3_cart_slot_set_state(pokemon_fap, Gen3CartSlotMenu);
    gen3_cart_ui_box_name(pokemon_fap, ui->box, box);
    snprintf(header, sizeof(header), "%s slot %u", box, (unsigned)ui->slot + 1u);
    submenu_reset(submenu);
    submenu_set_header(submenu, header);
    submenu_add_item(
        submenu, "New Pokemon", GEN3_CART_SLOT_ITEM_NEW, gen3_cart_slot_callback, pokemon_fap);
    submenu_add_item(
        submenu, "Load .pk3", GEN3_CART_SLOT_ITEM_LOAD, gen3_cart_slot_callback, pokemon_fap);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewSubmenu);
}

/* Just chosen in the list */
static void gen3_cart_slot_open(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    Gen3CartMon m = Gen3CartMonBadArg;

    /* the bulk file opened again if a read failed since: names and the editor need it */
    if(!gen3_menu_editor_ready(pokemon_fap)) {
        m = Gen3CartMonNoData;
    } else if(c) {
        m = gen3_cart_mon_open(
            c, pokemon_fap->gen3_editor, ui->box, ui->slot, ui->orig, &ui->orig_len, &ui->pkm);
    }
    switch(m) {
    case Gen3CartMonOk:
        ui->edit_mode = Gen3CartEditExisting;
        gen3_cart_slot_set_state(pokemon_fap, Gen3CartSlotEdit);
        scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Edit, 0);
        gen3_cart_slot_send(pokemon_fap, GEN3_CART_SLOT_EVENT_EDIT);
        break;
    case Gen3CartMonEmpty:
        gen3_cart_slot_menu(pokemon_fap);
        break;
    case Gen3CartMonNoData:
        gen3_cart_slot_send(pokemon_fap, GEN3_EVENT_DATA_MISSING);
        break;
    default:
        gen3_cart_ui_mon_result(pokemon_fap, false, m);
        gen3_cart_slot_send(pokemon_fap, GEN3_CART_SLOT_EVENT_RESULT);
        break;
    }
}

/* Back from the editor on the Pokemon opened from this slot */
static void gen3_cart_slot_close(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    Gen3CartMon m = Gen3CartMonBadArg;
    bool put = false;

    if(c && pokemon_fap->gen3_editor) {
        m = gen3_cart_mon_close(
            c,
            pokemon_fap->gen3_editor,
            ui->box,
            ui->slot,
            ui->orig,
            ui->orig_len,
            ui->mon,
            &ui->pkm,
            &put);
    }
    ui->edit_mode = Gen3CartEditNone;
    gen3_cart_slot_set_state(pokemon_fap, Gen3CartSlotOpen);
    if(m == Gen3CartMonOk) {
        gen3_cart_slot_send(pokemon_fap, GEN3_CART_SLOT_EVENT_LIST);
    } else {
        gen3_cart_ui_mon_result(pokemon_fap, true, m);
        gen3_cart_slot_send(pokemon_fap, GEN3_CART_SLOT_EVENT_RESULT);
    }
}

void gen3_cart_slot_reedit(PokemonFap* pokemon_fap) {
    gen3_cart_slot_set_state(pokemon_fap, Gen3CartSlotReedit);
    scene_manager_previous_scene(pokemon_fap->scene_manager);
}

void pokemon_scene_gen3_cart_slot_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    switch(gen3_cart_screen_slot_entry(gen3_cart_slot_state(pokemon_fap), ui->edit_mode)) {
    case Gen3CartSlotDoClose:
        gen3_cart_slot_close(pokemon_fap);
        break;
    case Gen3CartSlotDoPut:
        /* made or loaded for this free slot */
        gen3_cart_slot_send(pokemon_fap, GEN3_CART_SLOT_EVENT_PUT);
        break;
    case Gen3CartSlotDoEdit:
        gen3_cart_slot_set_state(pokemon_fap, Gen3CartSlotEdit);
        gen3_cart_slot_send(pokemon_fap, GEN3_CART_SLOT_EVENT_EDIT);
        break;
    case Gen3CartSlotDoOpen:
        ui->edit_mode = Gen3CartEditNone;
        gen3_cart_slot_set_state(pokemon_fap, Gen3CartSlotOpen);
        gen3_cart_slot_open(pokemon_fap);
        break;
    }
}

/* New Pokemon: the save's trainer as its OT, and its game as MetGame */
static void gen3_cart_slot_new(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    Gen3SaveTrainer t;

    if(!c || !gen3_cart_trainer(c, &t)) {
        gen3_cart_ui_mon_result(pokemon_fap, true, Gen3CartMonBadArg);
        scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartResult);
        return;
    }
    /* false: the bulk file can't be read (opened again first if a read failed since), and
     * nothing may go into the slot */
    if(!gen3_menu_editor_ready(pokemon_fap) ||
       !gen3_cart_new_mon(
           pokemon_fap->gen3_editor, &t, gen3_cart_id(c)->met_game, furi_hal_random_get())) {
        ui->edit_mode = Gen3CartEditNone;
        gen3_menu_show_data_missing(pokemon_fap);
        return;
    }
    ui->edit_mode = Gen3CartEditNew;
    gen3_cart_slot_to_editor(pokemon_fap);
}

static void gen3_cart_slot_load_failed(PokemonFap* pokemon_fap, const char* text) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    ui->edit_mode = Gen3CartEditNone;
    snprintf(ui->result_header, sizeof(ui->result_header), "Load failed");
    ui->result_text = text;
    ui->retry_mode = Gen3CartRetryNone;
    ui->result_back = PokemonSceneGen3CartSlots;
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartResult);
}

/* Own function, so that nothing of the load is in the frame the file browser runs in. The
 * Pokemon goes into the ui's buffer, then the editor; what the load fixed is shown first. */
__attribute__((noinline)) static void
    gen3_cart_slot_load_picked(PokemonFap* pokemon_fap, Storage* storage, const char* path) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3ImportInfo info;
    Gen3Check check = gen3_pk3_load_file(storage, path, &ui->pkm, &info);

    /* the import read the bulk file a moment ago; it can't any more */
    if(check == Gen3CheckOk && !gen3_edit_load(pokemon_fap->gen3_editor, &ui->pkm)) {
        check = Gen3CheckNoData;
    }
    if(check != Gen3CheckOk) {
        gen3_cart_slot_load_failed(pokemon_fap, gen3_cart_screen_check_text(check));
        return;
    }
    ui->edit_mode = Gen3CartEditLoaded;
    gen3_load_changes(&info, ui->notice, sizeof(ui->notice));
    gen3_cart_screen_wrap(ui->notice);
    if(ui->notice[0] != '\0') {
        gen3_cart_slot_set_state(pokemon_fap, Gen3CartSlotNotice);
        gen3_menu_show_notice(pokemon_fap, "Loaded", ui->notice);
    } else {
        gen3_cart_slot_to_editor(pokemon_fap);
    }
}

/* Load .pk3: from the folder the pokemon app saves them in */
static void gen3_cart_slot_load(PokemonFap* pokemon_fap) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FuriString* dir = furi_string_alloc();
    FuriString* path = furi_string_alloc();

    if(!gen3_pk3_dir(storage, dir)) {
        gen3_cart_slot_load_failed(pokemon_fap, "No SD card");
    } else {
        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        DialogsFileBrowserOptions options;
        dialog_file_browser_set_basic_options(&options, ".pk3", NULL);
        options.base_path = furi_string_get_cstr(dir);
        const bool picked = dialog_file_browser_show(dialogs, path, dir, &options);
        furi_record_close(RECORD_DIALOGS);
        /* none picked: the menu again */
        if(picked) gen3_cart_slot_load_picked(pokemon_fap, storage, furi_string_get_cstr(path));
    }

    furi_string_free(path);
    furi_string_free(dir);
    furi_record_close(RECORD_STORAGE);
}

bool pokemon_scene_gen3_cart_slot_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const uint32_t state = gen3_cart_slot_state(pokemon_fap);
    bool consumed = true;

    if(event.type == SceneManagerEventTypeBack) {
        /* The free slot's menu, or the notice of a .pk3 loaded: nothing goes into the slot */
        ui->edit_mode = Gen3CartEditNone;
        scene_manager_search_and_switch_to_previous_scene(
            scene_manager, PokemonSceneGen3CartSlots);
        return true;
    }
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case GEN3_CART_SLOT_EVENT_EDIT:
        /* Its highlight as it was: on the first row for one just opened */
        scene_manager_next_scene(scene_manager, PokemonSceneGen3Edit);
        break;
    case GEN3_CART_SLOT_EVENT_RESULT:
        scene_manager_next_scene(scene_manager, PokemonSceneGen3CartResult);
        break;
    case GEN3_CART_SLOT_EVENT_LIST:
        scene_manager_search_and_switch_to_previous_scene(
            scene_manager, PokemonSceneGen3CartSlots);
        break;
    case GEN3_CART_SLOT_EVENT_PUT:
        gen3_cart_ui_confirm(pokemon_fap, PokemonSceneGen3CartPut);
        break;
    case GEN3_EVENT_DATA_MISSING:
        ui->edit_mode = Gen3CartEditNone;
        gen3_menu_show_data_missing(pokemon_fap);
        break;
    case GEN3_CART_SLOT_ITEM_NEW:
        if(state == Gen3CartSlotMenu) gen3_cart_slot_new(pokemon_fap);
        break;
    case GEN3_CART_SLOT_ITEM_LOAD:
        if(state == Gen3CartSlotMenu) gen3_cart_slot_load(pokemon_fap);
        break;
    case GEN3_EVENT_NOTICE_OK:
        if(state == Gen3CartSlotNotice && ui->edit_mode == Gen3CartEditLoaded) {
            gen3_cart_slot_to_editor(pokemon_fap);
        }
        break;
    default:
        consumed = false;
        break;
    }

    return consumed;
}

void pokemon_scene_gen3_cart_slot_on_exit(void* context) {
    UNUSED(context);
}
