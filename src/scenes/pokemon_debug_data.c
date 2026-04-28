#ifdef DEBUG_DATA
#include <src/include/pokemon_app.h>
#include <src/include/pokemon_data.h>

#include <src/scenes/include/pokemon_scene.h>

void pokemon_scene_debug_data_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;
    TradeBlock *tb = NULL;

    /* We're going to convert to and then from the tradeblock struct.
     * The data should all match.
     */
    tb = pokemon_data_trade_block_get(pokemon_fap->pdata, NULL);
    pokemon_data_trade_block_set(pokemon_fap->pdata, tb, 0);


    view_dispatcher_send_custom_event(pokemon_fap->view_dispatcher, PokemonSceneBack);
}

bool pokemon_scene_debug_data_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void pokemon_scene_debug_data_on_exit(void* context) {
    UNUSED(context);
}
#endif
