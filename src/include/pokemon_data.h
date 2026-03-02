#ifndef POKEMON_DATA_H
#define POKEMON_DATA_H

#pragma once

#include <furi.h>
#include <gui/icon.h>
#include <storage/storage.h>
#include <toolbox/stream/stream.h>
#include <toolbox/stream/file_stream.h>

#include <math.h>
#include <stdint.h>

#include <src/include/named_list.h>
#include <src/include/stat_nl.h>
#include <src/include/pokemon_table.h>
#include <src/include/stats.h>

/* Generation defines */
#define GEN_I 0x01
#define GEN_II 0x02

/* Some length macros */
#define LEN_NAME_BUF 11
#define LEN_NICKNAME 11 // Max 10 chars
#define LEN_OT_NAME 8 // Max 7 chars
#define LEN_NUM_BUF 6
#define LEN_LEVEL 4 // Max 3 digits
#define LEN_OT_ID 6 // Max 5 digits

/* Based on the flipperzero-game-engine sprite structure */
/* Should be able to privatize this */
struct fxbm_sprite {
    uint32_t width;
    uint32_t height;
    uint8_t data[];
};

typedef struct trade_block {
    /* Pointer to the live trade block */
    void* trade_block;
    /* The length of the current trade block */
    size_t trade_block_sz;
    /* Shortcut pointer to the actual party data in the trade_block */
    void* party;
    /* Length of the party */
    size_t party_sz;
    /* Generation that the trade_block is intended for */
    uint8_t gen;
} TradeBlock;

struct pokemon_data {
    const NamedList* move_list; /* XXX: This can just use the call to list get */
    const NamedList* stat_list; /* XXX: This can just use the call to list get */
    const NamedList* type_list; /* XXX: This can just use the call to list get */
    const NamedList* item_list; /* XXX: This can just use the call to list get */
    const PokemonTable* pokemon_table; /* XXX: This can just use the call to table get */

    /* TODO: Once moved to the new tradeblock setup, this can be removed and
     * instead the calls to trade_block_get/set will be used to get data in and
     * out.
     */
    /* Pointer to the live trade_block */
    void* trade_block; /* XXX: should be able to privatize this */
    /* The length of the current trade_block. */
    size_t trade_block_sz; /* XXX: Should be able to privatize this */
    /* Shortcut pointer to the actual party data in the trade block */
    void* party; /* XXX: Should be able to privatize this */
    size_t party_sz; /* XXX: Should be able to privatize this */

    /* Current generation */
    uint8_t gen; /* XXX: Should be able to privatize this */

    /* 0 indexed max pokedex number */
    uint8_t dex_max; /* XXX: Should be able to privatize this */

    /* Pointer to info that is private to pokemon_data */
    void *priv;
};
typedef struct pokemon_data PokemonData;

PokemonData* pokemon_data_alloc(uint8_t gen);
void pokemon_data_free(PokemonData* pdata);

struct fxbm_sprite* pokemon_icon_get(PokemonData* pdata, int num);

/* XXX: Convert this to a "give me a flat blob of data" */
void pokemon_stat_memcpy(PokemonData* dst, PokemonData* src, uint8_t which);
uint16_t pokemon_stat_get(PokemonData* pdata, DataStat stat, DataStatSub num);
void pokemon_stat_set(PokemonData* pdata, DataStat stat, DataStatSub which, uint16_t val);
void pokemon_default_nickname_set(char* dest, PokemonData* pdata, size_t n);
void pokemon_name_set(PokemonData* pdata, DataStat stat, char* name);
void pokemon_name_get(PokemonData* pdata, DataStat stat, char* dest, size_t len);

TradeBlock* pokemon_data_trade_block_alloc(PokemonData* pdata);
TradeBlock* pokemon_data_trade_block_get(PokemonData* pdata, TradeBlock* tb);
void pokemon_data_trade_block_set(PokemonData* pdata, TradeBlock* tb, uint8_t which);
void pokemon_data_trade_block_free(TradeBlock* tb);
#endif /* POKEMON_DATA_H */
