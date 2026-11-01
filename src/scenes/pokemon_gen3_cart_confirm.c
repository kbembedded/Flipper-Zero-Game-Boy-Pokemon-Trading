#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <furi.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/scenes/include/pokemon_gen3_menu.h>

/* The Cartridge app's questions: one scene under seven ids, the one it is entered as kept in
 * Gen3CartLeave's scene state (gen3_cart_ui_confirm).
 * - Leave? (Gen3CartLeave): leaving the app drops the edits not written. Stay (or Back) returns to
 *   the scene below: the hub, or the instructions after a Retry.
 * - Put (Gen3CartPut), over the slot router: the Pokemon the editor made or loaded goes into the
 *   free slot chosen (gen3_cart_mon_place). Cancel drops it; Back returns to the editor.
 * - Write changes? (Gen3CartWriteAsk) and Discard changes? (Gen3CartDiscardAsk), over the hub: the
 *   job, or Cancel (or Back) to the hub.
 * - Restore backup? (Gen3CartRestoreAsk), over the hub once the file passed its check: both
 *   trainers, and the edits a restore drops; Restore runs the job, Cancel (or Back) to the hub.
 * - Copy ROM? or Resume copy? (Gen3CartRomAsk), over the hub: Copy, Resume, or Restart, which
 *   asks Restart copy? first, since it copies from the start, the old .part removed; Cancel (or
 *   Back) to the hub.
 * - Stop copying? (Gen3CartRomStop), over the ROM copy, which runs on: Stop cancels it; Keep
 *   going (or Back) returns to its progress. */

static char gen3_cart_confirm_header[GEN3_CART_SCREEN_HEADER_SIZE];
static char gen3_cart_confirm_text[GEN3_CART_SCREEN_ASK_SIZE];

_Static_assert(
    GEN3_CART_SCREEN_ASK_SIZE >= GEN3_CART_SCREEN_LEAVE_SIZE &&
        GEN3_CART_SCREEN_ASK_SIZE >= GEN3_CART_SCREEN_PUT_SIZE,
    "the text buffer holds each question's text");

static uint32_t gen3_cart_confirm_alias(PokemonFap* pokemon_fap) {
    return scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartLeave);
}

static void gen3_cart_confirm_show(
    PokemonFap* pokemon_fap,
    const char* header,
    const char* left,
    const char* right) {
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    gen3_cart_ui_dialog(pokemon_fap);
    dialog_ex_set_header(dialog_ex, header, 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog_ex, gen3_cart_confirm_text, 64, 12, AlignCenter, AlignTop);
    dialog_ex_set_left_button_text(dialog_ex, left);
    dialog_ex_set_right_button_text(dialog_ex, right);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

/* "Put TREECKO Lv10" / "in BOX 2\nslot 1?": the species name comes from the bulk file */
static void gen3_cart_confirm_put_enter(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    char species[GEN3_SPECIES_NAME_LEN + 1];
    char box[9];

    /* the bulk file opened again if a read failed since; the editor holds the Pokemon to put */
    const bool ready = gen3_menu_editor_ready(pokemon_fap);
    const Gen3Editor* ed = pokemon_fap->gen3_editor;
    if(!ready || !gen3_species_name_get(gen3_edit_national(ed), species)) {
        view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_EVENT_DATA_MISSING);
        return;
    }
    gen3_cart_ui_box_name(pokemon_fap, ui->box, box);
    gen3_cart_screen_put(
        species,
        (uint8_t)gen3_pkm_get(&ed->pkm, Gen3FieldLevel),
        box,
        ui->slot,
        gen3_cart_confirm_header,
        gen3_cart_confirm_text);
    gen3_cart_confirm_show(pokemon_fap, gen3_cart_confirm_header, "Cancel", "Put");
}

/* "Cart: BRENDAN 12345" / "File: MAY 22222" / "Hall of Fame is kept.", and the edits it drops */
static void gen3_cart_confirm_restore_enter(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    Gen3SaveTrainer t;
    const bool has = c && gen3_cart_trainer(c, &t);

    gen3_cart_screen_restore_ask(
        has ? &t : NULL,
        &ui->restore_trainer,
        c ? gen3_cart_changes(c) : 0,
        gen3_cart_confirm_text);
    gen3_cart_confirm_show(pokemon_fap, "Restore backup?", "Cancel", "Restore");
}

/* Copy ROM? with its size and time, or Resume copy? when a .part holds checked regions */
static void gen3_cart_confirm_rom_enter(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    uint32_t done = 0;

    ui->rom_resume = c && gen3_cart_rom_part(c, &done);
    ui->rom_done = done;
    ui->rom_restart = false;
    if(ui->rom_resume) {
        gen3_cart_screen_rom_resume(done, gen3_cart_rom_size(c), gen3_cart_confirm_text);
        gen3_cart_confirm_show(pokemon_fap, "Resume copy?", "Restart", "Resume");
    } else {
        gen3_cart_screen_rom_ask(c ? gen3_cart_rom_size(c) : 0, gen3_cart_confirm_text);
        gen3_cart_confirm_show(pokemon_fap, "Copy ROM?", "Cancel", "Copy");
    }
}

/* Resume copy?'s Restart: the same scene asks again, since a restart removes the .part */
static void gen3_cart_confirm_rom_restart(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);

    ui->rom_restart = true;
    gen3_cart_screen_rom_restart(
        ui->rom_done, c ? gen3_cart_rom_size(c) : 0, gen3_cart_confirm_text);
    gen3_cart_confirm_show(pokemon_fap, "Restart copy?", "Cancel", "Restart");
}

void pokemon_scene_gen3_cart_confirm_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;

    switch(gen3_cart_confirm_alias(pokemon_fap)) {
    case PokemonSceneGen3CartPut:
        gen3_cart_confirm_put_enter(pokemon_fap);
        break;
    case PokemonSceneGen3CartWriteAsk:
        gen3_cart_screen_write_ask(gen3_cart_ui_changes(pokemon_fap), gen3_cart_confirm_text);
        gen3_cart_confirm_show(pokemon_fap, "Write changes?", "Cancel", "Write");
        break;
    case PokemonSceneGen3CartDiscardAsk:
        gen3_cart_screen_discard_ask(gen3_cart_ui_changes(pokemon_fap), gen3_cart_confirm_text);
        gen3_cart_confirm_show(pokemon_fap, "Discard changes?", "Cancel", "Discard");
        break;
    case PokemonSceneGen3CartRestoreAsk:
        gen3_cart_confirm_restore_enter(pokemon_fap);
        break;
    case PokemonSceneGen3CartRomAsk:
        gen3_cart_confirm_rom_enter(pokemon_fap);
        break;
    case PokemonSceneGen3CartRomStop:
        snprintf(gen3_cart_confirm_text, sizeof(gen3_cart_confirm_text), "You can resume\nlater.");
        gen3_cart_confirm_show(pokemon_fap, "Stop copying?", "Keep going", "Stop");
        break;
    default:
        gen3_cart_screen_leave(gen3_cart_ui_changes(pokemon_fap), gen3_cart_confirm_text);
        gen3_cart_confirm_show(pokemon_fap, "Leave?", "Stay", "Leave");
        break;
    }
}

/* Put: the router's free slot, once more checked to be free, and the offer rules */
static void gen3_cart_confirm_put(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    const Gen3CartEditMode mode = ui->edit_mode;
    Gen3CartMon m = Gen3CartMonBadArg;

    ui->edit_mode = Gen3CartEditNone;
    if(c && (mode == Gen3CartEditNew || mode == Gen3CartEditLoaded)) {
        m = gen3_cart_mon_place(
            c, pokemon_fap->gen3_editor, ui->box, ui->slot, mode == Gen3CartEditNew, ui->mon);
    }
    if(m == Gen3CartMonOk) {
        scene_manager_search_and_switch_to_previous_scene(
            pokemon_fap->scene_manager, PokemonSceneGen3CartSlots);
    } else {
        gen3_cart_ui_mon_result(pokemon_fap, true, m);
        scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartResult);
    }
}

/* Put's Cancel: the Pokemon made or loaded goes nowhere */
static void gen3_cart_confirm_put_cancel(PokemonFap* pokemon_fap) {
    gen3_cart_ui_get(pokemon_fap)->edit_mode = Gen3CartEditNone;
    scene_manager_search_and_switch_to_previous_scene(
        pokemon_fap->scene_manager, PokemonSceneGen3CartSlots);
}

static void gen3_cart_confirm_right(PokemonFap* pokemon_fap, uint32_t alias) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    switch(alias) {
    case PokemonSceneGen3CartPut:
        gen3_cart_confirm_put(pokemon_fap);
        break;
    case PokemonSceneGen3CartWriteAsk:
        gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobWrite, NULL);
        break;
    case PokemonSceneGen3CartDiscardAsk:
        gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobDiscard, NULL);
        break;
    case PokemonSceneGen3CartRestoreAsk:
        gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobRestore, ui->restore_name);
        break;
    case PokemonSceneGen3CartRomAsk:
        /* Copy; Resume from the .part; or Restart, asked twice: a copy from the start removes the
         * .part first */
        gen3_cart_ui_run_job(
            pokemon_fap, Gen3CartJobRom, ui->rom_resume && !ui->rom_restart ? "resume" : NULL);
        break;
    case PokemonSceneGen3CartRomStop:
        /* the copy shows Stopping until it has ended; the .part keeps what it checked */
        gen3_cart_ui_stop_job(pokemon_fap);
        scene_manager_previous_scene(pokemon_fap->scene_manager);
        break;
    default:
        gen3_cart_ui_leave(pokemon_fap);
        break;
    }
}

static void gen3_cart_confirm_left(PokemonFap* pokemon_fap, uint32_t alias) {
    const Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(alias == PokemonSceneGen3CartPut) {
        gen3_cart_confirm_put_cancel(pokemon_fap);
    } else if(alias == PokemonSceneGen3CartRomAsk && ui->rom_resume && !ui->rom_restart) {
        /* Restart: asked again before the .part is removed */
        gen3_cart_confirm_rom_restart(pokemon_fap);
    } else {
        /* Cancel, Stay, or Keep going */
        scene_manager_previous_scene(pokemon_fap->scene_manager);
    }
}

bool pokemon_scene_gen3_cart_confirm_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    const uint32_t alias = gen3_cart_confirm_alias(pokemon_fap);
    const bool put = alias == PokemonSceneGen3CartPut;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GEN3_CART_EVENT_RIGHT) {
            gen3_cart_confirm_right(pokemon_fap, alias);
            consumed = true;
        } else if(event.event == GEN3_CART_EVENT_LEFT) {
            gen3_cart_confirm_left(pokemon_fap, alias);
            consumed = true;
        } else if(put && event.event == GEN3_EVENT_DATA_MISSING) {
            gen3_cart_ui_get(pokemon_fap)->edit_mode = Gen3CartEditNone;
            gen3_menu_show_data_missing(pokemon_fap);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack && put) {
        /* Back to the editor, to go on with the Pokemon */
        gen3_cart_slot_reedit(pokemon_fap);
        consumed = true;
    }
    /* Any other Back is the scene manager's: Stay, Cancel, or Keep going */

    return consumed;
}

void pokemon_scene_gen3_cart_confirm_on_exit(void* context) {
    UNUSED(context);
}
