#include <src/scenes/include/pokemon_gen3_cart.h>

#include <furi.h>
#include <src/views/gen3_cart_progress.h>

/* Runs the job gen3_cart_ui_run_job set, once per run, on the progress view, then shows its
 * result. Coming back to it (from a scene over it) finds the job running and doesn't start it
 * again. Back stops a job that can stop; a ROM copy asks first (Gen3CartRomStop), and runs on
 * under the question; a Write, Restore or Discard runs to its end. A Restore check that passed
 * goes on to Restore backup? instead of a result. */

/* The session takes the job once the last one has ended; until then it is tried on every tick */
static void gen3_cart_job_start(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(ui->job_started) return;
    ui->job_started =
        gen3_cart_session_run(ui->session, ui->job, ui->arg[0] != '\0' ? ui->arg : NULL);
    if(ui->job_started) gen3_cart_ui_progress(pokemon_fap, Gen3CartScreenStarting);
}

/* The job ended */
static void gen3_cart_job_end(PokemonFap* pokemon_fap, const Gen3CartSessionStatus* st) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    SceneManager* scene_manager = pokemon_fap->scene_manager;

    if(ui->job == Gen3CartJobRestoreCheck && st->result == Gen3CartOk && st->restore_has_trainer) {
        /* The file is a backup of this game: ask, over the hub, which Cancel returns to */
        ui->restore_trainer = st->restore_trainer;
        scene_manager_search_and_switch_to_previous_scene(scene_manager, PokemonSceneGen3Cart);
        gen3_cart_ui_confirm(pokemon_fap, PokemonSceneGen3CartRestoreAsk);
        return;
    }
    /* st is the status buffer, which the calls below read again */
    const Gen3CartResult r = st->result;
    const Gen3CartStep step = st->step;
    /* A Reconnect that finished a write or restore cut short shows that one's result */
    const Gen3CartJob job = gen3_cart_screen_result_job(ui->job, r, st->finished);
    /* A save backed up again, or restored, may be another trainer's */
    if(gen3_cart_screen_new_trainer(job, r)) gen3_cart_ui_take_trainer(pokemon_fap);
    gen3_cart_ui_set_result(pokemon_fap, job, step, r, false, PokemonSceneGen3Cart);
    scene_manager_next_scene(scene_manager, PokemonSceneGen3CartResult);
}

static void gen3_cart_job_tick(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(!ui->job_started) {
        gen3_cart_job_start(pokemon_fap);
        return;
    }
    const Gen3CartSessionStatus* st = gen3_cart_ui_status(pokemon_fap);
    if(st->busy) {
        gen3_cart_ui_progress(
            pokemon_fap, ui->job_stopping ? Gen3CartScreenStopping : Gen3CartScreenRunning);
        return;
    }
    gen3_cart_job_end(pokemon_fap, st);
}

static void gen3_cart_job_back(PokemonFap* pokemon_fap) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(!ui->job_started) return; /* nothing runs yet: its first tick starts it */
    if(ui->job_stopping) return;
    switch(gen3_cart_screen_back(ui->job)) {
    case Gen3CartBackIgnore:
        break;
    case Gen3CartBackAsk:
        /* Stop copying? over this scene; one that has just ended shows its result instead */
        if(gen3_cart_ui_status(pokemon_fap)->busy) {
            gen3_cart_ui_confirm(pokemon_fap, PokemonSceneGen3CartRomStop);
        }
        break;
    case Gen3CartBackStop:
        gen3_cart_ui_stop_job(pokemon_fap);
        gen3_cart_ui_progress(pokemon_fap, Gen3CartScreenStopping);
        break;
    }
}

void pokemon_scene_gen3_cart_job_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    if(ui->job_started) {
        gen3_cart_ui_progress(
            pokemon_fap, ui->job_stopping ? Gen3CartScreenStopping : Gen3CartScreenRunning);
    } else {
        gen3_cart_job_start(pokemon_fap);
        if(!ui->job_started) {
            /* The status is still the last job's: show none of it */
            gen3_cart_progress_set_hint(pokemon_fap->gen3_cart_progress, "");
            gen3_cart_progress_set(pokemon_fap->gen3_cart_progress, "Starting", "", "", -1);
        }
    }
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewGen3CartProgress);
}

bool pokemon_scene_gen3_cart_job_on_event(void* context, SceneManagerEvent event) {
    PokemonFap* pokemon_fap = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == GEN3_CART_EVENT_TICK) {
            gen3_cart_job_tick(pokemon_fap);
            consumed = true;
        } else if(event.event == GEN3_CART_EVENT_BACK) {
            gen3_cart_job_back(pokemon_fap);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        /* The progress view keeps Back: this never pops the scene with a job running */
        gen3_cart_job_back(pokemon_fap);
        consumed = true;
    }

    return consumed;
}

void pokemon_scene_gen3_cart_job_on_exit(void* context) {
    UNUSED(context);
}
