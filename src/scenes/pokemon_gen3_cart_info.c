#include <src/scenes/include/pokemon_gen3_cart.h>

#include <furi.h>

/* Cart info: the cart as the last whole connect found it, its save, and why writing is off. Five
 * lines and no button (they wouldn't fit together): Back returns to the hub. */

static char gen3_cart_info_header[GEN3_CART_SCREEN_HEADER_SIZE];
static char gen3_cart_info_text[GEN3_CART_SCREEN_INFO_SIZE];

void pokemon_scene_gen3_cart_info_on_enter(void* context) {
    PokemonFap* pokemon_fap = context;
    /* NULL while a job runs, whose cart it is then */
    const Gen3Cart* c = gen3_cart_ui_cart(pokemon_fap);
    DialogEx* dialog_ex = pokemon_fap->dialog_ex;

    gen3_cart_ui_dialog(pokemon_fap);
    if(!c) {
        dialog_ex_set_header(dialog_ex, "Cartridge", 64, 0, AlignCenter, AlignTop);
        dialog_ex_set_text(dialog_ex, "Busy. Try again.", 64, 12, AlignCenter, AlignTop);
        view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
        return;
    }
    const Gen3CartId* id = gen3_cart_id(c);
    Gen3SaveTrainer trainer;
    Gen3CartScreenInfo in = {
        .id = id,
        .backup_current = gen3_cart_backup_current(c),
        .backup_name = gen3_cart_backup_name(c),
        .slots = NULL,
        .trainer = gen3_cart_trainer(c, &trainer) ? &trainer : NULL,
    };
    /* The analysis is the backup's, and only a cart with a known layout has one */
    if(in.backup_current && id->can_browse) in.slots = gen3_cart_slots(c);
    in.writable = gen3_cart_writable(c, &in.why);
    gen3_cart_screen_info(&in, gen3_cart_info_header, gen3_cart_info_text);

    dialog_ex_set_header(dialog_ex, gen3_cart_info_header, 64, 0, AlignCenter, AlignTop);
    dialog_ex_set_text(dialog_ex, gen3_cart_info_text, 64, 12, AlignCenter, AlignTop);
    view_dispatcher_switch_to_view(pokemon_fap->view_dispatcher, AppViewDialogEx);
}

bool pokemon_scene_gen3_cart_info_on_event(void* context, SceneManagerEvent event) {
    /* Back is the scene manager's: the hub */
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pokemon_scene_gen3_cart_info_on_exit(void* context) {
    UNUSED(context);
}
