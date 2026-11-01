#include <src/scenes/include/pokemon_gen3_cart.h>

#include <stdio.h>

#include <dialogs/dialogs.h>
#include <furi.h>
#include <src/gen3/gen3_pk3_furi.h>

/* Restore backup: the file browser in the backups folder, then the Restore check job on the file
 * picked (gen3_cart_restore_check), whose Ok goes on to Restore backup? over the hub
 * (Gen3CartRestoreAsk). The browser's Back returns to the hub. The hub offers this only while
 * linked and gen3_cart_restorable, which asks for the identification's can_write, not for
 * gen3_cart_writable: a cart whose save is empty or damaged, which isn't writable, can still
 * have a backup put back. */

#define GEN3_CART_RESTORE_DIR GEN3_SHARED_DATA_DIR "/" GEN3_CART_BACKUP_DIR

/* A path the job can't take: "Can't restore", OK back to the hub */
static void gen3_cart_restore_refuse(PokemonFap* pokemon_fap, const char* text) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);

    snprintf(ui->result_header, sizeof(ui->result_header), "Can't restore");
    ui->result_text = text;
    ui->retry_mode = Gen3CartRetryNone;
    ui->result_back = PokemonSceneGen3Cart;
    scene_manager_next_scene(pokemon_fap->scene_manager, PokemonSceneGen3CartResult);
}

/* Own function, so that nothing of it is in the frame the file browser runs in. The jobs name a
 * file relative to the shared folder, and never take a name cut short. */
__attribute__((noinline)) static void
    gen3_cart_restore_picked(PokemonFap* pokemon_fap, const char* path) {
    Gen3CartUi* ui = gen3_cart_ui_get(pokemon_fap);
    const Gen3CartScreenName n =
        gen3_cart_screen_restore_name(path, GEN3_SHARED_DATA_DIR, ui->restore_name);

    if(n != Gen3CartScreenNameOk) {
        gen3_cart_restore_refuse(pokemon_fap, gen3_cart_screen_restore_name_text(n));
        return;
    }
    gen3_cart_ui_run_job(pokemon_fap, Gen3CartJobRestoreCheck, ui->restore_name);
}

void pokemon_scene_gen3_cart_restore_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    FuriString* path = furi_string_alloc_set(GEN3_CART_RESTORE_DIR);
    DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
    DialogsFileBrowserOptions options;

    dialog_file_browser_set_basic_options(&options, ".sav", NULL);
    options.base_path = GEN3_CART_RESTORE_DIR;
    const bool picked = dialog_file_browser_show(dialogs, path, path, &options);
    furi_record_close(RECORD_DIALOGS);

    if(picked) {
        gen3_cart_restore_picked(pokemon_fap, furi_string_get_cstr(path));
    } else {
        scene_manager_previous_scene(pokemon_fap->scene_manager);
    }
    furi_string_free(path);
}

bool pokemon_scene_gen3_cart_restore_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pokemon_scene_gen3_cart_restore_on_exit(void* context) {
    UNUSED(context);
}
