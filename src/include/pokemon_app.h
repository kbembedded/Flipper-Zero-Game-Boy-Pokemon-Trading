#ifndef POKEMON_APP_H
#define POKEMON_APP_H

#pragma once

#include <gui/scene_manager.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/icon.h>
#include <gui/modules/dialog_ex.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gblink.h>

#include <src/include/pokemon_data.h>
#include <src/gen3/gen3_pk3.h>

#define TAG "Pokemon"

struct pokemon_fap {
    /* Various anonymous pointers for flipper UI/navigation */
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    void* select;
    void* trade;
    Submenu* submenu;
    TextInput* text_input;
    VariableItemList* variable_item_list;
    DialogEx* dialog_ex;

    /* Struct for holding trade data */
    /* NOTE: There may be some runtime memory savings by adding more intelligence
     * to views/trade and slimming down this struct to only contain the single
     * pokemon data rather than the full 6 member party data.
     */
    PokemonData* pdata;

    /* gblink interface */
    void *gblink_handle;

    /* Gen III Link Test: its view, and its session (struct Gen3Session from
     * src/gen3/gen3_session_furi.h). Both live from entering the Link Test until
     * the Gen III menu is shown again, so they survive the "Stop link?" dialog.
     */
    void* gen3_link_test;
    struct Gen3Session* gen3_session;

    /* Gen III editor (struct Gen3Editor from src/gen3/gen3_edit.h). Allocated on the
     * first Edit Pokemon, Load .pk3 or Save .pk3 that finds the Gen III data file
     * readable (gen3_menu_editor_ready), then kept for the app's lifetime.
     */
    struct Gen3Editor* gen3_editor;

    /* Gen III species picker view. Allocated the first time the Gen III menu is shown,
     * then kept for the app's lifetime.
     */
    void* gen3_species_select;

    /* Gen III .pk3 file name being saved, without the extension */
    char gen3_file_name[GEN3_PK3_NAME_MAX + 1];

    /* Gen III trade screen's view: allocated with a trade session by Gen III -> Trade, which
     * then lives behind every Gen III scene; freed with it.
     */
    void* gen3_trade_view;
    /* The sync between the editor and the link (struct Gen3TradeSync from
     * src/gen3/gen3_trade_sync.h): allocated by the first Gen III -> Trade, then kept until the
     * app exits, so the block on a traded-away Pokemon outlives the session (O2).
     */
    struct Gen3TradeSync* gen3_trade_sync;
    /* The next trade session unlocks the GBA's National Dex (per run, never saved) */
    bool gen3_national_dex;
    /* The trade session has stopped (the GBA left): the tick doesn't start it again */
    bool gen3_trade_ended;

    /* The pokemon_cart app (Cartridge) only; the pokemon app never uses them. Its progress view,
     * and its scenes' state with the cartridge session (struct Gen3CartUi from
     * src/scenes/include/pokemon_gen3_cart.h): allocated by the first Connect, kept until the app
     * exits. */
    void* gen3_cart_progress;
    struct Gen3CartUi* gen3_cart_ui;
};

typedef struct pokemon_fap PokemonFap;

typedef enum {
    AppViewSubmenu,
    AppViewTextInput,
    AppViewVariableItem,
    AppViewDialogEx,
    AppViewSelectPokemon,
    AppViewTrade,
    AppViewGen3LinkTest,
    AppViewGen3SpeciesSelect,
    AppViewGen3Trade,
    AppViewGen3CartProgress,
} AppView;

/* Gen III trade app tick (every 100 ms, app thread, between UI events): while the trade screen
 * exists, starts the trade session once the cable reads OK, moves the received Pokemon, the GBA
 * player and the offer between the editor and the link (D6: only while the current scene can't
 * change the Pokemon, which is Gen3Trade, Gen3Menu, and the dialogs over them that don't edit:
 * Gen3TradeEnd, Gen3NatDex and Gen3LinkExit), stops the session once the GBA has left and gone,
 * and refreshes the trade screen. Does nothing otherwise. Defined in pokemon_gen3_trade.c. */
void gen3_trade_tick(PokemonFap* pokemon_fap);

/* Call before entering Gen3Edit or Gen3Load from Gen3Trade or Gen3Menu. Without a running
 * trade session: true. Otherwise runs one tick (so a finished trade is picked up first), then
 * false if the sync's pickup_fail is Gen3PickupNoData (a pickup is being retried), else
 * gen3_session_set_editing(s, true, sync->trades_seen): false while a trade is pending or a
 * received Pokemon isn't picked up yet. false: don't open the scene; show Gen3Trade. On true,
 * open the scene in the same event handler, so no tick runs in between. Defined in
 * pokemon_gen3_trade.c. */
bool gen3_trade_begin_edit(PokemonFap* pokemon_fap);

#endif /* POKEMON_APP_H */
