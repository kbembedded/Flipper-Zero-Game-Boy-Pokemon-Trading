#ifndef POKEMON_POKERUS_H
#define POKEMON_POKERUS_H

#include <src/include/pokemon_data.h>

#pragma once

const char* pokerus_get_status_str(PokemonData* pdata);

void pokerus_set_strain(PokemonData* pdata, uint8_t strain);

void pokerus_set_days(PokemonData *pdata, uint8_t days);

#endif // POKEMON_POKERUS_H
