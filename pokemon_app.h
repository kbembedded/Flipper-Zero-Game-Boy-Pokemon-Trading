#ifndef POKEMON_APP_H
#define POKEMON_APP_H

#pragma once

#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/icon.h>

#include "pokemon_data.h"

#define TAG "Pokemon"

struct pokemon_data_table {
    const char* name;
    const Icon* icon;
    const uint8_t species;
};

typedef struct pokemon_data_table PokemonTable;

typedef enum {
    GAMEBOY_INITIAL,
    GAMEBOY_READY,
    GAMEBOY_WAITING,
    GAMEBOY_TRADE_READY,
    GAMEBOY_SEND,
    GAMEBOY_PENDING,
    GAMEBOY_TRADING
} render_gameboy_state_t;

struct pokemon_fap {
    ViewDispatcher* view_dispatcher;

    /* View ports for each of the application's steps */
    View* select_view;
    View* trade_view;

    /* Table of pokemon data for Gen I */
    const PokemonTable* pokemon_table;

    /* Struct for holding trade data */
    /* NOTE: There may be some runtime memory savings by adding more intelligence
     * to views/trade and slimming down this struct to only contain the single
     * pokemon data rather than the full 6 member party data.
     */
    TradeBlock* trade_party;

    /* The currently selected pokemon */
    int curr_pokemon;

    /* Some state tracking */
    /* This, combined with some globals in trade.cpp, can probably be better
     * consolidated at some point.
     */
    bool trading;
    bool connected;
    render_gameboy_state_t gameboy_status;

    /* TODO: Other variables will end up here, like selected level, EV/IV,
     * moveset, etc. Likely will want to be another sub struct similar to
     * the actual pokemon data structure.
     */
};

typedef struct pokemon_fap PokemonFap;

typedef enum {
    AppViewSelectPokemon,
    AppViewTrade,
    AppViewExitConfirm,
} AppView;

#endif /* POKEMON_APP_H */
