#include <gui/modules/variable_item_list.h>
#include <furi.h>

#include <src/include/pokemon_data.h>

static const char* pokerus_states[] = {
    "Clean",
    "Infected",
    "Cured",
    "",
};

const char* pokerus_get_status_str(PokemonData* pdata) {
    uint8_t pokerus;

    pokerus = pokemon_stat_get(pdata, STAT_POKERUS, NONE);

    if(pokerus == 0x00)
        return pokerus_states[0];

    if((pokerus & 0x0f) != 0x00)
        return pokerus_states[1];

    return pokerus_states[2];
}

void pokerus_set_strain(PokemonData* pdata, uint8_t strain) {
    uint8_t pokerus;

    /* Need to read/modify/write the existing stat */
    pokerus = pokemon_stat_get(pdata, STAT_POKERUS, NONE);
    pokerus &= 0x0f;
    pokerus |= (strain << 4);

    if((pokerus & 0xf0) == 0x00)
        pokerus = 0;

    pokemon_stat_set(pdata, STAT_POKERUS, NONE, pokerus);
}

void pokerus_set_days(PokemonData *pdata, uint8_t days) {
    uint8_t pokerus;

    days &= 0x0f;

    /* Need to read/modify/write the existing stat */
    pokerus = pokemon_stat_get(pdata, STAT_POKERUS, NONE);
    pokerus &= 0xf0;
    pokerus |= days;
    pokemon_stat_set(pdata, STAT_POKERUS, NONE, pokerus);
}
