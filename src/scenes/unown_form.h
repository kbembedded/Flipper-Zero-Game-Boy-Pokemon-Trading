#ifndef UNOWN_FORM_H
#define UNOWN_FORM_H

#pragma once

#include <src/include/pokemon_data.h>

/* Returns ascii char, or 0 if unown is not the current pokemon */
char unown_form_get(PokemonData* pdata);

void unown_form_set(PokemonData* pdata, char letter);

#endif // UNOWN_FORM_H
