#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <src/include/pokemon_app.h>
#include <src/scenes/include/pokemon_scene.h>

#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_cart_furi.h>
#include <src/gen3/gen3_cart_screen.h>
#include <src/gen3/gen3_pkm.h>

/* The Cartridge app's scenes' shared header (pokemon_gen3_cart*.c): their state, kept from the
 * first Connect until the app exits, and the helpers they share, defined in pokemon_gen3_cart.c
 * with the hub. */

/* The cart hub is registered as Gen3Menu (pokemon_cart_scene_config.h): the Gen III editor scenes
 * search back to Gen3Menu, and in the Cartridge app that is this hub. */
#define PokemonSceneGen3Cart PokemonSceneGen3Menu

/* Custom events of the Cartridge scenes, clear of scene ids and the other Gen III events. A tick
 * still queued when a scene is left reaches the next one, which ignores it. The scenes' own
 * submenu items and events are 0x473305xx too, each file's in a range of its own: the hub
 * 0x11-0x1F, the boxes 0x20-0x2E, the slots 0x40-0x5D, the slot router 0x60-0x6F. */
#define GEN3_CART_EVENT_TICK 0x47330501u /* the progress view's 250 ms timer */
#define GEN3_CART_EVENT_BACK 0x47330502u /* Back on the progress view */
#define GEN3_CART_EVENT_LEFT 0x47330503u /* a dialog's buttons (gen3_cart_ui_dialog) */
#define GEN3_CART_EVENT_CENTER 0x47330504u
#define GEN3_CART_EVENT_RIGHT 0x47330505u
/* The party and box scenes, to themselves: nothing to browse, back to the hub */
#define GEN3_CART_EVENT_HUB 0x47330506u

typedef struct Gen3CartUi {
    Gen3CartSession* session;
    /* The box the slots list (GEN3_SAVE_PARTY for the party), the slot chosen in it, and the bytes
     * the Pokemon opened from it had in the editor (gen3_cart_mon_open) */
    uint8_t box, slot;
    Gen3CartEditMode edit_mode;
    uint8_t orig[GEN3_PARTY_SIZE];
    size_t orig_len;
    /* The job Gen3CartJobRun runs (gen3_cart_ui_run_job); arg "" for none */
    Gen3CartJob job;
    char arg[GEN3_CART_NAME_MAX];
    bool job_started; /* the session took it: coming back to Gen3CartJobRun doesn't run it again */
    bool job_stopping; /* Back stopped it, and it hasn't ended yet */
    /* What Gen3CartResult shows */
    Gen3CartResult result;
    char result_header[24];
    const char* result_text; /* a static string; NULL: gen3_cart_result_text(result) */
    uint32_t result_back; /* the scene OK searches back to; PokemonSceneGen3Cart by default */
    Gen3CartRetry retry_mode; /* what Retry does; Gen3CartRetryNone: no Retry */
    /* Working buffers for opening and putting a save's Pokemon: never stack locals */
    uint8_t mon[GEN3_PARTY_SIZE];
    Gen3Pkm pkm;
    char notice[40];
    /* Restore: the file picked, as the jobs name it, and the trainer its check found */
    char restore_name[GEN3_CART_NAME_MAX];
    Gen3SaveTrainer restore_trainer;
    bool rom_resume; /* Copy ROM: a .part was found, so the question is Resume or Restart */
    uint32_t rom_done; /* the bytes it holds */
    bool rom_restart; /* Restart was chosen: Restart copy? asks before the .part is removed */
    /* A ROM copy's rate, from its first progress in this run: that progress's bytes and tick */
    bool rate_started;
    uint32_t rate_done;
    uint32_t rate_tick;
    char result_buf[GEN3_CART_SCREEN_ASK_SIZE]; /* a result text made for it (the ROM's name) */
} Gen3CartUi;

/* Allocates on first use the ui, the session (gen3_cart_session_alloc, which starts its worker
 * and creates the log) and the progress view (pokemon_fap->gen3_cart_progress, view id
 * AppViewGen3CartProgress), whose tick and Back post GEN3_CART_EVENT_TICK and
 * GEN3_CART_EVENT_BACK. */
Gen3CartUi* gen3_cart_ui_get(PokemonFap* pokemon_fap);
/* Frees the session (cancelling its job and waiting for it to end), the progress view and the ui.
 * Safe if nothing was allocated. Call with another view shown, before gblink_free and
 * view_dispatcher_free. */
void gen3_cart_ui_end(PokemonFap* pokemon_fap);
/* Leaves the app: cancels any job, then stops the scene manager and the view dispatcher, so the
 * app frees everything and exits. */
void gen3_cart_ui_leave(PokemonFap* pokemon_fap);

/* Copies the session's status into one static buffer and returns it (all zero before the session
 * exists). The status is never a stack local (no app stack local is over 128 bytes). App thread
 * only; the next call overwrites it. */
const Gen3CartSessionStatus* gen3_cart_ui_status(PokemonFap* pokemon_fap);
/* The cart's state, for the app thread: NULL before the session exists, and while a job runs (the
 * cart is the job's then). It reads the status, so it refreshes gen3_cart_ui_status's buffer. Only
 * the app starts jobs, so the cart stays the app's until it starts one. */
Gen3Cart* gen3_cart_ui_cart(PokemonFap* pokemon_fap);
/* Pokemon slots changed and not written; 0 before the session exists, and while a job runs */
uint16_t gen3_cart_ui_changes(PokemonFap* pokemon_fap);
/* The editor takes the save's trainer (gen3_cart_trainer), so a new Pokemon for this cart gets it
 * as OT; nothing without one. Once a job that may leave another save's backup has ended Ok
 * (gen3_cart_screen_new_trainer): the Connect, Back up again and Restore. */
void gen3_cart_ui_take_trainer(PokemonFap* pokemon_fap);

/* Opens the confirm scene (pokemon_gen3_cart_confirm.c) as scene: Gen3CartLeave, Gen3CartPut,
 * Gen3CartWriteAsk, Gen3CartDiscardAsk, Gen3CartRestoreAsk, Gen3CartRomAsk or Gen3CartRomStop.
 * Which one is kept in Gen3CartLeave's scene state, set here before scene_manager_next_scene. */
void gen3_cart_ui_confirm(PokemonFap* pokemon_fap, uint32_t scene);

/* The name a box shows: "Party" for GEN3_SAVE_PARTY, else the save's (gen3_cart_box_name), or
 * "Box <n>" if it can't be read or is blank */
void gen3_cart_ui_box_name(PokemonFap* pokemon_fap, uint8_t box, char out[9]);

/* Sets Gen3CartResult for a Pokemon not opened ("Can't edit") or not put ("Can't put"): the
 * reason's text, no Retry, and OK back to the list (Gen3CartSlots) */
void gen3_cart_ui_mon_result(PokemonFap* pokemon_fap, bool put, Gen3CartMon m);

/* From the Put question, which is over the slot router: back to the router, which opens the
 * editor again on the Pokemon made or loaded. Defined in pokemon_gen3_cart_slot.c. */
void gen3_cart_slot_reedit(PokemonFap* pokemon_fap);

/* Resets the DialogEx and sends its Left, Center and Right as GEN3_CART_EVENT_LEFT, _CENTER and
 * _RIGHT to the current scene */
void gen3_cart_ui_dialog(PokemonFap* pokemon_fap);

/* Shows the progress view for the session's status: state as gen3_cart_screen_progress takes it */
void gen3_cart_ui_progress(PokemonFap* pokemon_fap, Gen3CartScreenState state);

/* Runs job (arg NULL for none) in Gen3CartJobRun: back to it if it is in the scene stack (Retry
 * on its result), else on top of the current scene. Its result's OK returns to the hub. arg is
 * copied into ui->arg, so it is another buffer (ui->restore_name, a literal). */
void gen3_cart_ui_run_job(PokemonFap* pokemon_fap, Gen3CartJob job, const char* arg);
/* Asks the job Gen3CartJobRun runs to stop (gen3_cart_session_cancel), once; Gen3CartJobRun shows
 * Stopping until it has ended */
void gen3_cart_ui_stop_job(PokemonFap* pokemon_fap);

/* Sets Gen3CartResult's header, text, Retry and the scene its OK returns to, from a job's end
 * (gen3_cart_screen_result, with the cart's link as the job left it); call once the job has ended,
 * before scene_manager_next_scene(..., Gen3CartResult) */
void gen3_cart_ui_set_result(
    PokemonFap* pokemon_fap,
    Gen3CartJob job,
    Gen3CartStep step,
    Gen3CartResult r,
    bool dropped,
    uint32_t back);
