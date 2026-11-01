#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <src/scenes/include/pokemon_gen3_menu.h>
#include <src/views/gen3_cart_progress.h>

/* The Cartridge app's hub, reached once a Connect ran to its end, and the helpers every Cartridge
 * scene shares. The hub is registered as Gen3Menu (pokemon_cart_scene_config.h): the Gen III
 * editor scenes search back to Gen3Menu, and in this app that is this hub. */

/* The hub's items, clear of scene ids and the Gen III events. The scene state keeps the one last
 * chosen. */
#define GEN3_CART_ITEM_INFO 0x47330511u
#define GEN3_CART_ITEM_RECONNECT 0x47330512u
#define GEN3_CART_ITEM_BACKUP 0x47330513u
#define GEN3_CART_ITEM_BROWSE 0x47330514u
#define GEN3_CART_ITEM_WRITE 0x47330515u
#define GEN3_CART_ITEM_WRITING_OFF 0x47330516u
#define GEN3_CART_ITEM_DISCARD 0x47330517u
#define GEN3_CART_ITEM_RESTORE 0x47330518u
#define GEN3_CART_ITEM_ROM 0x47330519u

/* gen3_cart_ui_status's buffer */
static Gen3CartSessionStatus gen3_cart_status;
/* The two labels made on each entry (the submenu keeps copies) */
static char gen3_cart_write_label[24]; /* "Write changes (426)" */
static char gen3_cart_writing_off_label[40]; /* "Writing off: " and a refuse text */
/* A ROM copy's percent and time left, around its bar */
static char gen3_cart_percent[8];
static char gen3_cart_left[GEN3_CART_SCREEN_LEFT_SIZE];

/* Timer thread: hand the tick to the app thread, which reads the session */
static void gen3_cart_tick_callback(void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_EVENT_TICK);
}

static void gen3_cart_back_callback(void* context) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, GEN3_CART_EVENT_BACK);
}

Gen3CartUi* gen3_cart_ui_get(PokemonFap* pokemon_fap) {
    if(!pokemon_fap->gen3_cart_ui) {
        Gen3CartUi* ui = malloc(sizeof(Gen3CartUi));
        memset(ui, 0, sizeof(*ui));
        ui->result_back = PokemonSceneGen3Cart;

        pokemon_fap->gen3_cart_progress =
            gen3_cart_progress_alloc(pokemon_fap->view_dispatcher, AppViewGen3CartProgress);
        gen3_cart_progress_set_tick(
            pokemon_fap->gen3_cart_progress, gen3_cart_tick_callback, pokemon_fap);
        gen3_cart_progress_set_back(
            pokemon_fap->gen3_cart_progress, gen3_cart_back_callback, pokemon_fap);

        ui->session = gen3_cart_session_alloc(pokemon_fap->gblink_handle);
        pokemon_fap->gen3_cart_ui = ui;
    }
    return pokemon_fap->gen3_cart_ui;
}

void gen3_cart_ui_end(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = pokemon_fap->gen3_cart_ui;

    if(ui) {
        if(ui->session) gen3_cart_session_free(ui->session);
        free(ui);
        pokemon_fap->gen3_cart_ui = NULL;
    }
    if(pokemon_fap->gen3_cart_progress) {
        gen3_cart_progress_free(
            pokemon_fap->view_dispatcher,
            AppViewGen3CartProgress,
            pokemon_fap->gen3_cart_progress);
        pokemon_fap->gen3_cart_progress = NULL;
    }
}

void gen3_cart_ui_leave(PokemonFap* pokemon_fap) {
    if(pokemon_fap->gen3_cart_ui) gen3_cart_session_cancel(pokemon_fap->gen3_cart_ui->session);
    scene_manager_stop(pokemon_fap->scene_manager);
    view_dispatcher_stop(pokemon_fap->view_dispatcher);
}

const Gen3CartSessionStatus* gen3_cart_ui_status(PokemonFap* pokemon_fap) {
    if(pokemon_fap->gen3_cart_ui) {
        gen3_cart_session_status(pokemon_fap->gen3_cart_ui->session, &gen3_cart_status);
    } else {
        memset(&gen3_cart_status, 0, sizeof(gen3_cart_status));
    }
    return &gen3_cart_status;
}

Gen3Cart* gen3_cart_ui_cart(PokemonFap* pokemon_fap) {
    if(!pokemon_fap->gen3_cart_ui) return NULL;
    if(gen3_cart_ui_status(pokemon_fap)->busy) return NULL;
    return gen3_cart_session_cart(pokemon_fap->gen3_cart_ui->session);
}

uint16_t gen3_cart_ui_changes(PokemonFap* pokemon_fap) {
    const Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    return c ? gen3_cart_changes(c) : 0;
}

void gen3_cart_ui_take_trainer(PokemonFap* pokemon_fap) {
    const Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    Gen3SaveTrainer t;
    Gen3Trainer ot;

    /* c: NULL while a job runs, which none does once the job has ended */
    if(!c || !pokemon_fap->gen3_editor || !gen3_cart_trainer(c, &t)) return;
    gen3_cart_trainer_ot(&t, &ot);
    gen3_edit_set_trainer(pokemon_fap->gen3_editor, &ot);
}

void gen3_cart_ui_confirm(PokemonFap* pokemon_fap, uint32_t scene) {
    scene_manager_set_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3CartLeave, scene);
    scene_manager_next_scene(pokemon_fap->scene_manager, scene);
}

void gen3_cart_ui_box_name(PokemonFap* pokemon_fap, uint8_t box, char out[9]) {
    Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);

    if(box == GEN3_SAVE_PARTY) {
        snprintf(out, 9, "Party");
    } else if(!c || !gen3_cart_box_name(c, box, out) || out[0] == '\0') {
        snprintf(out, 9, "Box %u", (unsigned)box + 1u);
    }
}

void gen3_cart_ui_mon_result(PokemonFap* pokemon_fap, bool put, Gen3CartMon m) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    Gen3CartScreenHubIn in;
    Gen3CartScreenHub items;

    /* the hub's items as they are now, which a pending write's text names */
    gen3_cart_screen_hub_in(gen3_cart_ui_cart(pokemon_fap), &in);
    gen3_cart_screen_hub(&in, &items);
    snprintf(
        ui->result_header,
        sizeof(ui->result_header),
        "%s",
        put ? GEN3_CART_SCREEN_CANT_PUT : GEN3_CART_SCREEN_CANT_EDIT);
    ui->result_text = gen3_cart_screen_mon_text(m, &items);
    ui->retry_mode = Gen3CartRetryNone;
    ui->result_back = PokemonSceneGen3CartSlots;
}

static void gen3_cart_dialog_callback(DialogExResult result, void* context) {
    PokemonFap* pokemon_fap = context;
    uint32_t event;

    switch(result) {
    case DialogExResultLeft:
        event = GEN3_CART_EVENT_LEFT;
        break;
    case DialogExResultCenter:
        event = GEN3_CART_EVENT_CENTER;
        break;
    case DialogExResultRight:
        event = GEN3_CART_EVENT_RIGHT;
        break;
    default:
        return;
    }
    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, event);
}

void gen3_cart_ui_dialog(PokemonFap* pokemon_fap) {
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    dialog_ex_reset(dialog_ex);
    dialog_ex_set_context(dialog_ex, pokemon_fap);
    dialog_ex_set_result_callback(dialog_ex, gen3_cart_dialog_callback);
}

/* A ROM copy's time left, at the rate it has copied at since the first progress this run showed */
static const char* gen3_cart_ui_time_left(Gen3CartUi* ui, const Gen3CartSessionStatus* st) {
    const uint32_t now = furi_get_tick();

    if(!ui->rate_started) {
        ui->rate_started = true;
        ui->rate_done = st->done;
        ui->rate_tick = now;
    }
    const uint64_t ms = (uint64_t)(now - ui->rate_tick) * 1000u / furi_kernel_get_tick_frequency();
    gen3_cart_screen_time_left(
        ui->rate_done,
        st->done,
        st->total,
        ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms,
        gen3_cart_left);
    return gen3_cart_left;
}

void gen3_cart_ui_progress(PokemonFap* pokemon_fap, Gen3CartScreenState state) {
    const Gen3CartSessionStatus* st = gen3_cart_ui_status(pokemon_fap);
    Gen3CartUi* ui = pokemon_fap->gen3_cart_ui;
    Gen3CartScreenProgress p;

    gen3_cart_screen_progress(st, state, &p);
    if(ui && state == Gen3CartScreenRunning && st->busy && st->job == Gen3CartJobRom &&
       (st->step == Gen3CartStepRomRead || st->step == Gen3CartStepRomVerify)) {
        /* An hours-long copy: its percent over the bar, unless the cable warning is there, and
         * the time left under it */
        if(p.line1[0] == '\0' && p.percent >= 0) {
            snprintf(gen3_cart_percent, sizeof(gen3_cart_percent), "%d%%", (int)p.percent);
            p.line1 = gen3_cart_percent;
        }
        p.line2 = gen3_cart_ui_time_left(ui, st);
    }
    gen3_cart_progress_set_hint(pokemon_fap->gen3_cart_progress, p.hint);
    gen3_cart_progress_set(pokemon_fap->gen3_cart_progress, p.title, p.line1, p.line2, p.percent);
}

void gen3_cart_ui_run_job(PokemonFap* pokemon_fap, Gen3CartJob job, const char* arg) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    ui->job = job;
    snprintf(ui->arg, sizeof(ui->arg), "%s", arg ? arg : "");
    ui->job_started = false;
    ui->job_stopping = false;
    ui->rate_started = false;
    ui->result_back = PokemonSceneGen3Cart;
    if(!scene_manager_search_and_switch_to_previous_scene(
           pokemon_fap->scene_manager, PokemonSceneGen3CartJobRun)) {
        scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartJobRun);
    }
}

void gen3_cart_ui_stop_job(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(!ui->job_started || ui->job_stopping) return;
    gen3_cart_session_cancel(ui->session);
    ui->job_stopping = true;
}

void gen3_cart_ui_set_result(
    PokemonFap* pokemon_fap,
    Gen3CartJob job,
    Gen3CartStep step,
    Gen3CartResult r,
    bool dropped,
    uint32_t back) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    Gen3CartScreenResult res;

    gen3_cart_screen_result(job, step, r, dropped, c && gen3_cart_linked(c), &res);
    ui->result = r;
    snprintf(ui->result_header, sizeof(ui->result_header), "%s", res.header);
    ui->result_text = res.text;
    ui->retry_mode = res.retry;
    ui->result_back = back;
    if(job == Gen3CartJobRom && r == Gen3CartOk && c) {
        /* where the copy is */
        char name[GEN3_CART_NAME_MAX];
        gen3_cart_rom_name(c, name);
        gen3_cart_screen_rom_done(name, ui->result_buf);
        ui->result_text = ui->result_buf;
    }
}

/* ---- The hub ---- */

static void gen3_cart_item_callback(void* context, uint32_t index) {
    PokemonFap* pokemon_fap = context;

    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, index);
}

static void gen3_cart_add(PokemonFap* pokemon_fap, const char* label, uint32_t item) {
    submenu_add_item(pokemon_fap->submenu, label, item, gen3_cart_item_callback, pokemon_fap);
}

void pokemon_scene_gen3_cart_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Submenu* submenu = pokemon_fap->submenu;
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    /* NULL while a job runs: then only Cart info, which waits for it too */
    Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    Gen3CartScreenHubIn in;
    Gen3CartScreenHub items;

    /* Back here, nothing in the editor goes into a slot any more */
    ui->edit_mode = Gen3CartEditNone;
    gen3_cart_screen_hub_in(c, &in);
    gen3_cart_screen_hub(&in, &items);

    submenu_reset(submenu);
    submenu_set_header(
        submenu, c && gen3_cart_id(c)->title[0] ? gen3_cart_id(c)->title : "Cartridge");
    if(items.browse) gen3_cart_add(pokemon_fap, "Party and boxes", GEN3_CART_ITEM_BROWSE);
    if(items.write) {
        snprintf(
            gen3_cart_write_label,
            sizeof(gen3_cart_write_label),
            "Write changes (%u)",
            (unsigned)in.changes);
        gen3_cart_add(pokemon_fap, gen3_cart_write_label, GEN3_CART_ITEM_WRITE);
    }
    if(items.writing_off) {
        snprintf(
            gen3_cart_writing_off_label,
            sizeof(gen3_cart_writing_off_label),
            "Writing off: %s",
            gen3_cart_refuse_text(in.why));
        gen3_cart_add(pokemon_fap, gen3_cart_writing_off_label, GEN3_CART_ITEM_WRITING_OFF);
    }
    if(items.discard) gen3_cart_add(pokemon_fap, "Discard changes", GEN3_CART_ITEM_DISCARD);
    if(items.reconnect) gen3_cart_add(pokemon_fap, "Reconnect", GEN3_CART_ITEM_RECONNECT);
    if(items.backup) gen3_cart_add(pokemon_fap, "Back up again", GEN3_CART_ITEM_BACKUP);
    if(items.restore) gen3_cart_add(pokemon_fap, "Restore backup", GEN3_CART_ITEM_RESTORE);
    if(items.rom) gen3_cart_add(pokemon_fap, "Copy ROM", GEN3_CART_ITEM_ROM);
    gen3_cart_add(pokemon_fap, "Cart info", GEN3_CART_ITEM_INFO);
    submenu_set_selected_item(
        submenu, scene_manager_get_scene_state(pokemon_fap->scene_manager, PokemonSceneGen3Cart));

    gen3_menu_show_menu(pokemon_fap);
}

/* An item that reads or changes the cart, chosen while no job runs (the menu was built so) */
static bool gen3_cart_idle(PokemonFap* pokemon_fap) {
    return gen3_cart_ui_cart(pokemon_fap) != NULL;
}

bool pokemon_scene_gen3_cart_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    SceneManager* scene_manager = pokemon_fap->scene_manager;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case GEN3_CART_ITEM_INFO:
        case GEN3_CART_ITEM_WRITING_OFF:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            scene_manager_next_scene(scene_manager, PokemonSceneGen3CartInfo);
            consumed = true;
            break;
        case GEN3_CART_ITEM_BROWSE:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            if(gen3_cart_idle(pokemon_fap)) {
                scene_manager_set_scene_state(scene_manager, PokemonSceneGen3CartBoxes, 0);
                scene_manager_next_scene(scene_manager, PokemonSceneGen3CartBoxes);
            }
            consumed = true;
            break;
        case GEN3_CART_ITEM_WRITE:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            if(gen3_cart_idle(pokemon_fap)) {
                gen3_cart_ui_confirm(pokemon_fap, PokemonSceneGen3CartWriteAsk);
            }
            consumed = true;
            break;
        case GEN3_CART_ITEM_DISCARD:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            if(gen3_cart_idle(pokemon_fap)) {
                gen3_cart_ui_confirm(pokemon_fap, PokemonSceneGen3CartDiscardAsk);
            }
            consumed = true;
            break;
        case GEN3_CART_ITEM_RESTORE:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            if(gen3_cart_idle(pokemon_fap)) {
                scene_manager_next_scene(scene_manager, PokemonSceneGen3CartRestore);
            }
            consumed = true;
            break;
        case GEN3_CART_ITEM_ROM:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            if(gen3_cart_idle(pokemon_fap)) {
                gen3_cart_ui_confirm(pokemon_fap, PokemonSceneGen3CartRomAsk);
            }
            consumed = true;
            break;
        case GEN3_CART_ITEM_RECONNECT:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobReconnect, NULL);
            consumed = true;
            break;
        case GEN3_CART_ITEM_BACKUP:
            scene_manager_set_scene_state(scene_manager, PokemonSceneGen3Cart, event.event);
            gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobBackup, NULL);
            consumed = true;
            break;
        case GEN3_MENU_EVENT_DATA_OK:
            if(gen3_menu_dialog_up()) {
                gen3_menu_show_menu(pokemon_fap);
                consumed = true;
            }
            break;
        default:
            break;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(gen3_menu_dialog_up()) {
            /* Back on the "Data missing" dialog returns to the hub */
            gen3_menu_show_menu(pokemon_fap);
        } else if(!gen3_cart_idle(pokemon_fap)) {
            /* A job runs, whose changes can't be counted now: leaving waits until it has ended */
        } else if(gen3_cart_ui_changes(pokemon_fap) > 0) {
            gen3_cart_ui_confirm(pokemon_fap, PokemonSceneGen3CartLeave);
        } else {
            gen3_cart_ui_leave(pokemon_fap);
        }
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_cart_on_exit(void* context) {
    UNUSED(context);
}
