#include <pokemon_icons.h>

#include <storage/storage.h>

#include <src/include/pokemon_data.h>
#include <src/include/pokemon_app.h>
#include <src/include/pokemon_char_encode.h>

#include <src/include/pokemon_table.h>

#include <src/include/named_list.h>
#include <src/include/item_nl.h>
#include <src/include/stat_nl.h>
#include <src/include/type_nl.h>
#include <src/include/move_nl.h>

#include <src/missingno_i.h>
#include <src/pokemon_data_i.h>

#define RECALC_NONE 0x00
#define RECALC_EXP 0x01
#define RECALC_EVS 0x02
#define RECALC_IVS 0x04
#define RECALC_STATS 0x08
#define RECALC_NICKNAME 0x10
#define RECALC_MOVES 0x20
#define RECALC_TYPES 0x40
#define RECALC_ALL 0xFF

#define FXBM_SPRITE_SIZE 404 // Each 56x56 sprite

struct pdata_priv {
    Storage* storage;
    struct fxbm_sprite* bitmap;
    uint8_t bitmap_num;
    FuriString* asset_path;
    /* Pointer to the main info struct */
    struct pokemon_info* info;
};

/* XXX: This function header can eventually be removed */
static void pokemon_stat_ev_calc(PokemonData* pdata, EvIv val);
static void pokemon_stat_iv_calc(PokemonData* pdata, EvIv val);

/* Text lookups to make debug output cleaner and easier to parse as a human */
static char* stat_text_get(DataStat stat) {
    switch(stat) {
    case STAT_ATK:          return "ATK";
    case STAT_DEF:          return "DEF";
    case STAT_SPD:          return "SPD";
    case STAT_SPC:          return "SPC";
    case STAT_SPC_ATK:      return "SPC_ATK";
    case STAT_SPC_DEF:      return "SPC_DEF";
    case STAT_HP:           return "HP";
    case STAT_TYPE:         return "Type";
    case STAT_MOVE:         return "Move";
    case STAT_ATK_EV:       return "ATK_EV";
    case STAT_DEF_EV:       return "DEF_EV";
    case STAT_SPD_EV:       return "SPD_EV";
    case STAT_SPC_ATK_EV:   [[fallthrough]];
    case STAT_SPC_DEF_EV:   [[fallthrough]];
    case STAT_SPC_EV:       return "SPC_EV";
    case STAT_HP_EV:        return "HP_EV";
    case STAT_IV:           return "IV";
    case STAT_ATK_IV:       return "ATK_IV";
    case STAT_DEF_IV:       return "DEF_IV";
    case STAT_SPD_IV:       return "SPD_IV";
    case STAT_SPC_ATK_IV:   [[fallthrough]];
    case STAT_SPC_DEF_IV:   [[fallthrough]];
    case STAT_SPC_IV:       return "SPC_IV";
    case STAT_HP_IV:        return "HP_IV";
    case STAT_LEVEL:        return "Lvl.";
    case STAT_NUM:          return "Num.";
    case STAT_CONDITION:    return "Cond.";
    case STAT_NICKNAME:     return "Nick.";
    case STAT_OT_NAME:      return "OT Name";
    case STAT_OT_ID:        return "OT ID";
    case STAT_TRAINER_NAME: return "Trainer Name";
    case STAT_SEL:          return "EV/IV Sel."; // which EV/IV calc to use
    case STAT_EXP:          return "Exp.";
    case STAT_HELD_ITEM:    return "Held Item";
    case STAT_POKERUS:      return "Pokerus";
    default:                return "UNKNOWN STAT";
    }
}


/* Calculates stat from current level */
static void pokemon_stat_calc(PokemonData* pdata, DataStat stat) {
    furi_assert(pdata);
    uint8_t iv;
    uint16_t ev;
    uint8_t base;
    uint8_t level;
    uint16_t calc;

    level = pokemon_stat_get(pdata, STAT_LEVEL, NONE);
    base = table_stat_base_get(pdata->pokemon_table,
                               pokemon_stat_get(pdata, STAT_NUM, NONE),
                               stat + STAT_BASE_OFFS, NONE);

    ev = pokemon_stat_get(pdata, stat + STAT_EV_OFFS, NONE);
    iv = pokemon_stat_get(pdata, stat + STAT_IV_OFFS, NONE);

    /* Gen I and II calculation */
    // https://bulbapedia.bulbagarden.net/wiki/Stat#Generations_I_and_II
    calc = floor((((2 * (base + iv)) + floor(sqrt(ev) / 4)) * level) / 100);

    if(stat == STAT_HP)
        calc += (level + 10);
    else
        calc += 5;

    pokemon_stat_set(pdata, stat, NONE, calc);
}

static void pokemon_exp_calc(PokemonData* pdata) {
    furi_assert(pdata);
    int level;
    uint32_t exp;
    uint8_t growth = table_stat_base_get(
        pdata->pokemon_table, pokemon_stat_get(pdata, STAT_NUM, NONE), STAT_BASE_GROWTH, NONE);

    level = (int)pokemon_stat_get(pdata, STAT_LEVEL, NONE);
    /* Calculate exp */
    switch(growth) {
    case GROWTH_FAST:
        // https://bulbapedia.bulbagarden.net/wiki/Experience#Fast
        exp = (4 * level * level * level) / 5;
        break;
    case GROWTH_MEDIUM_FAST:
        // https://bulbapedia.bulbagarden.net/wiki/Experience#Medium_Fast
        exp = (level * level * level);
        break;
    case GROWTH_MEDIUM_SLOW:
        // https://bulbapedia.bulbagarden.net/wiki/Experience#Medium_Slow
        exp = (((level * level * level) * 6 / 5) - (15 * level * level) + (100 * level) - 140);
        break;
    case GROWTH_SLOW:
        // https://bulbapedia.bulbagarden.net/wiki/Experience#Slow
        exp = (5 * level * level * level) / 4;
        break;
    default:
        FURI_LOG_E(TAG, "invalid growth");
        furi_crash();
        break;
    }

    pokemon_stat_set(pdata, STAT_EXP, NONE, exp);
}

/* Recalculate values and stats based on their dependencies.
 * The order of the if statements are in order of dependence from
 * depending on no other value, to dpeneding on multiple other values.
 *
 * level:	depends on:	none
 * iv: 		depends on: 	none (only what the EV/IV general setting is, which recalculates EV/IV at time of set)
 * ev:		depends on: 	level (sometimes)
 * exp:		depends on:	level, index
 * moves:	depends on:	index
 * types:	depends on:	index
 * nickname:	depends on:	index
 * atk/def/etc:	depends on:	level, iv, ev, index
 */
static void pokemon_recalculate(PokemonData* pdata, DataStat stat) {
    furi_assert(pdata);
    struct pdata_priv* priv = pdata->priv;
    struct pokemon_info* info = priv->info;
    uint8_t recalc;
    int i;

    /* From the stat that was just updated, create a bitfield of stats that
     * need to be recalculated from it.
     */
    switch (stat) {
    case STAT_LEVEL:
        recalc = (RECALC_STATS | RECALC_EXP | RECALC_EVS);
	break;
    case STAT_NUM:
        recalc = RECALC_ALL;
	break;
    case STAT_SEL:
        recalc = (RECALC_EVS | RECALC_IVS | RECALC_STATS);
	break;
    default:
        return;
    }

    /* Ordered in order of priority for calculating other stats */
    if(recalc & RECALC_NICKNAME) pokemon_name_set(pdata, STAT_NICKNAME, NULL);

    if(recalc & RECALC_MOVES) {
        for(i = MOVE_0; i <= MOVE_3; i++) {
            pokemon_stat_set(
                pdata,
                STAT_MOVE,
                i,
                table_stat_base_get(
                    pdata->pokemon_table,
                    pokemon_stat_get(pdata, STAT_NUM, NONE),
                    STAT_BASE_MOVE,
                    i));
        }
    }

    if(recalc & RECALC_TYPES) {
        for(i = TYPE_0; i <= TYPE_1; i++) {
            pokemon_stat_set(
                pdata,
                STAT_TYPE,
                i,
                table_stat_base_get(
                    pdata->pokemon_table,
                    pokemon_stat_get(pdata, STAT_NUM, NONE),
                    STAT_BASE_TYPE,
                    i));
        }
    }

    if(recalc & RECALC_EXP) pokemon_exp_calc(pdata);

    if(recalc & RECALC_EVS) pokemon_stat_ev_calc(pdata, info->stat_sel);

    /* This just rerolls the IVs, nothing really to calculate */
    if(recalc & RECALC_IVS) pokemon_stat_iv_calc(pdata, info->stat_sel);

    /* Note: This will still end up calculating spc_def on gen i pokemon.
     * However, the way the accessors are set up the calculated value will
     * never be written anywhere. This is just wasted CPU time.
     */
    if(recalc & RECALC_STATS) {
        for(i = STAT_OFFS; i < STAT_END; i++) {
            pokemon_stat_calc(pdata, i);
        }
    }
}

/* This DOES NOT encode/decode characters, that needs to happen when sending
 * this struct to the trade block.
 */
void pokemon_name_set(PokemonData* pdata, DataStat stat, char* name)
{
	furi_assert(pdata);
	struct pdata_priv* priv = pdata->priv;
	struct pokemon_info* info = priv->info;
	char name_buf[LEN_NAME_BUF];
	unsigned int i;

	/* If name is NULL, then we need to set the name to the default name
	 * which is their species name, all caps.
	 *
	 * QUIRK! This is only ever expected to be called with STAT_NICKNAME,
	 * rather that deal with a NULL deref, just set the default pokemon
	 * nickname.
	 */
	if (name == NULL) {
		strncpy(name_buf,
			table_stat_name_get(pdata->pokemon_table,
					    pokemon_stat_get(pdata, STAT_NUM, NONE)),
			sizeof(name_buf));
		/* Next, walk through and toupper() each character */
		for (i = 0; i < sizeof(name_buf); i++)
			name_buf[i] = toupper(name_buf[i]);
		name = name_buf;
	}

	switch (stat) {
	case STAT_NICKNAME:
		/* XXX: Should these use strncpy? */
		strlcpy(info->nickname, name, LEN_NAME_BUF);
		break;
	case STAT_OT_NAME:
		strlcpy(info->ot_name, name, LEN_NAME_BUF);
		break;
	case STAT_TRAINER_NAME:
		strlcpy(info->trainer_name, name, LEN_NAME_BUF);
		break;
	default:
        	FURI_LOG_E(TAG, "invalid name set");
        	furi_crash();
		break;
	}
	FURI_LOG_D(TAG, "[data] %s name set to %s", stat_text_get(stat), name);
}

/* This DOES NOT encode/decode characters, that needs to happen when sending
 * this struct to the trade block.
 */
void pokemon_name_get(PokemonData* pdata, DataStat stat, char* dest, size_t len)
{
	furi_assert(pdata);
	struct pdata_priv* priv = pdata->priv;
	struct pokemon_info* info = priv->info;

	switch (stat) {
	case STAT_NICKNAME:
		strlcpy(dest, info->nickname, len);
		break;
	case STAT_OT_NAME:
		strlcpy(dest, info->ot_name, len);
		break;
	case STAT_TRAINER_NAME:
		strlcpy(dest, info->trainer_name, len);
		break;
	default:
        	FURI_LOG_E(TAG, "invalid name get");
        	furi_crash();
		break;
	}
}

/* Each sprite 56x56 is 404 bytes long */
/* XXX: TODO: We don't need to thrash memory here by alloc and freeing
 * bitmap every time we get a new sprite. We already assume that the sprite
 * size is a constant anyway.
 */
struct fxbm_sprite* pokemon_icon_get(PokemonData* pdata, int num) {
    furi_assert(pdata);
    File* file;
    FuriString* path;
    uint32_t size;
    bool is_error = true;
    struct pdata_priv* pdata_priv = pdata->priv;

    if(pdata_priv->bitmap_num != num) {
        if(pdata_priv->bitmap) {
            free(pdata_priv->bitmap);
            pdata_priv->bitmap = NULL;
        }

        file = storage_file_alloc(pdata_priv->storage);
        path = furi_string_alloc_set(pdata_priv->asset_path);
        furi_string_cat_printf(path, "all_sprites.fxbm");

        if(storage_file_open(file, furi_string_get_cstr(path), FSAM_READ, FSOM_OPEN_EXISTING)) {
            storage_file_seek(file, (num - 1) * FXBM_SPRITE_SIZE, true);
            if(storage_file_read(file, &size, sizeof(size)) == sizeof(size)) {
                pdata_priv->bitmap = malloc(size);
                if(storage_file_read(file, pdata_priv->bitmap, size) ==
                   FXBM_SPRITE_SIZE - sizeof(size)) {
                    FURI_LOG_D(TAG, "Opened file \'%s\'", furi_string_get_cstr(path));
                    is_error = false;
                } else {
                    free(pdata_priv->bitmap);
                }
            }
        }

        if(is_error) {
            FURI_LOG_E(
                TAG, "Failed to open \'%s\' or access sprite data", furi_string_get_cstr(path));
            pdata_priv->bitmap = (struct fxbm_sprite*)((uint8_t*)(__000_fxbm) + sizeof(size));
            num = 0;
        }

        storage_file_free(file);
        furi_string_free(path);

        pdata_priv->bitmap_num = num;
    }

    return pdata_priv->bitmap;
}

uint16_t pokemon_stat_get(PokemonData* pdata, DataStat stat, DataStatSub which)
{
	furi_assert(pdata);
	struct pdata_priv* priv = pdata->priv;
	struct pokemon_info* info = priv->info;

	switch (stat) {
	case STAT_ATK:		return info->atk;
	case STAT_DEF:		return info->def;
	case STAT_SPD:		return info->spd;
	case STAT_SPC:
	case STAT_SPC_ATK:	return info->spc_atk;
	case STAT_SPC_DEF:	return info->spc_def;
	case STAT_HP:		return info->hp;
	case STAT_ATK_EV:	return info->atk_ev;
	case STAT_DEF_EV:	return info->def_ev;
	case STAT_SPD_EV:	return info->spd_ev;
	case STAT_SPC_EV:
	case STAT_SPC_ATK_EV:
	case STAT_SPC_DEF_EV:	return info->spc_ev;
	case STAT_HP_EV:	return info->hp_ev;
	case STAT_IV:		return info->iv.ivs;

	/* The IVs in GB byte order, are always:
	 * atk, def, spd, spc
	 * Like every other 16 bit quantity that the Flipper acts on, we need to
	 * byte swap them normally. However, the below accessors for individual
	 * IV nibbles directly pull from the data structures which will always
	 * be in GB endianness and directly return.
	 */
	case STAT_SPD_IV:	return info->iv.iv.spd;
	/* In order to line up all of the dynamic stat accessors used as part of the
	 * stat calculation loop, we need to overload the SPC IV accessor to allow
	 * accessing SPC, SPC_ATK, and SPC_DEF. Note that only SPC exists, the ATK
	 * and DEF are the overloaded values. This is so when, for example, gen i
	 * calculates its SPC value, or gen ii calculates is SPC_DEF value, it will
	 * always grab the same IV nibble.
	 */
	case STAT_SPC_IV:
	case STAT_SPC_ATK_IV:
	case STAT_SPC_DEF_IV:	return info->iv.iv.spc;
	case STAT_ATK_IV:	return info->iv.iv.atk;
	case STAT_DEF_IV:	return info->iv.iv.def;
	case STAT_HP_IV:
		/* NOTE:
		 * HP IV is calculated as the LSB of each other IV, assembled in the
		 * same bit order down to a single nibble.
		 */
		return ( ((info->iv.iv.atk & 0x01) << 3) |
			 ((info->iv.iv.def & 0x01) << 2) |
			 ((info->iv.iv.spd & 0x01) << 1) |
			 ((info->iv.iv.spc & 0x01)));
	case STAT_LEVEL:	return info->level;
	/* STAT_NUM is the 0 indexed number of the pokemon. We store
	 * STAT_NUM as index because aside from gen I, the index is
	 * the same as the national dex number. This means translating
	 * to and from gen II is just a copy. To and from gen I requires
	 * looking up the index from the main pokemon table.
	 */
	case STAT_NUM:		return (info->index - 1);
	case STAT_MOVE:		return info->move[which];
	case STAT_TYPE:		return info->type[which];
	case STAT_OT_ID:	return info->ot_id;
	case STAT_POKERUS:	return info->pokerus;
	case STAT_SEL:		return info->stat_sel;
	case STAT_EXP:		return info->exp;
	case STAT_HELD_ITEM:	return info->catch_held;
	default:
		FURI_LOG_E(TAG, "stat_get: invalid stat %d", stat);
		furi_crash();
		break;
	}
}

void pokemon_stat_set(PokemonData* pdata, DataStat stat, DataStatSub which, uint32_t val)
{
	furi_assert(pdata);
	struct pdata_priv* priv = pdata->priv;
	struct pokemon_info* info = priv->info;

	switch (stat) {
	case STAT_ATK:		info->atk = val;		break;
	case STAT_DEF:		info->def = val;		break;
	case STAT_SPD:		info->spd = val;		break;
	case STAT_SPC:		[[fallthrough]];
	case STAT_SPC_ATK:	info->spc_atk = val;		break;
	case STAT_SPC_DEF:	info->spc_def = val;		break;
	case STAT_HP:		info->hp = val;
				info->max_hp = val;		break;
	case STAT_ATK_EV: 	info->atk_ev = val;		break;
	case STAT_DEF_EV:	info->def_ev = val;		break;
	case STAT_SPD_EV:	info->spd_ev = val;		break;
	/* SPC ATK/DEF are not real values, they are all handled by the single
	 * SPC EV, but match them anyway to allow for more flexible loop access.
	 */
	case STAT_SPC_EV:	[[fallthrough]];
	case STAT_SPC_ATK_EV:	[[fallthrough]];
	case STAT_SPC_DEF_EV:	info->spc_ev = val;		break;
	case STAT_HP_EV:	info->hp_ev = val;		break;
	// STAT IV should only be accessed as each individual IV
	case STAT_IV:		info->iv.ivs = val;		break;
	case STAT_SPD_IV:	info->iv.iv.spd = val & 0xF;	break;
	case STAT_SPC_IV:	[[fallthrough]];
	case STAT_SPC_ATK_IV:	[[fallthrough]];
	case STAT_SPC_DEF_IV:	info->iv.iv.spc = val & 0xF;	break;
	case STAT_ATK_IV:	info->iv.iv.atk = val & 0xF;	break;
	case STAT_DEF_IV:	info->iv.iv.def = val & 0xF;	break;
	case STAT_LEVEL:	info->level = val;		break;
	/* STAT_NUM is the 0 indexed number of the pokemon. We store
	 * STAT_NUM as index because aside from gen I, the index is
	 * the same as the national dex number. This means translating
	 * to and from gen II is just a copy. To and from gen I requires
	 * looking up the index from the main pokemon table.
	 */
	case STAT_NUM:		info->index = val + 1;		break;
	case STAT_MOVE:		info->move[which] = val;	break;
	case STAT_TYPE:		info->type[which] = val;	break;
	case STAT_OT_ID:	info->ot_id = val;		break;
	case STAT_POKERUS:	info->pokerus = val;		break;
	case STAT_SEL:		info->stat_sel = val;		break;
	case STAT_EXP:		info->exp = val;		break;
	case STAT_HELD_ITEM:	info->catch_held = val;		break;
	default:
		FURI_LOG_E(TAG, "stat_set: invalid stat %d", stat);
		furi_crash();
		break;
	}

	FURI_LOG_D(TAG, "[data] stat %s:%d set to 0x%lX", stat_text_get(stat), which, val);
	pokemon_recalculate(pdata, stat);
}

static void pokemon_stat_ev_calc(PokemonData* pdata, EvIv val) {
    furi_assert(pdata);
    int level;
    uint16_t ev;
    DataStat i;

    level = pokemon_stat_get(pdata, STAT_LEVEL, NONE);

    /* Generate STATEXP */
    switch(val) {
    case RANDIV_LEVELEV:
    case MAXIV_LEVELEV:
        ev = (0xffff / 100) * level;
        break;
    case RANDIV_MAXEV:
    case MAXIV_MAXEV:
        ev = 0xffff;
        break;
    default: // *_ZEROEV
        ev = 0;
        break;
    }

    for(i = STAT_EV_OFFS; i < STAT_EV_END; i++) {
        pokemon_stat_set(pdata, i, NONE, ev);
    }
}

static void pokemon_stat_iv_calc(PokemonData* pdata, EvIv val) {
    furi_assert(pdata);

    /* Set up IVs */
    switch(val) {
    case RANDIV_ZEROEV:
    case RANDIV_LEVELEV:
    case RANDIV_MAXEV:
        pokemon_stat_set(pdata, STAT_IV, NONE, (uint16_t)rand());
        break;
    default: // MAXIV_*
        pokemon_stat_set(pdata, STAT_IV, NONE, 0xFFFF);
        break;
    }
}

PokemonData* pokemon_data_alloc(uint8_t gen) {
	UNUSED(gen);

	PokemonData* pdata;
	struct pdata_priv* pdata_priv = NULL;

	pdata = malloc(sizeof(PokemonData));
	pdata->priv = malloc(sizeof(struct pdata_priv));
	pdata_priv = pdata->priv;
	pdata->gen = gen;

        /* Set the max pokedex number, 0 indexed */
	/* XXX: This probably shouldn't be here? */
	/* XXX: Need to figure out how to handle someone with a gen II pokemon
	 * set up in the pdata struct going back in to gen I selection. Maybe
	 * Create a popup of some kind that says the current pokemon does not
	 * exist in the selected gen, do you want to keep this current pokemon
	 * or reset to defaults?
	 */
	if (gen == GEN_I)
		pdata->dex_max = 150;
	else
		pdata->dex_max = 250;

	/* Set up lists */
	pdata->move_list = move_nl_pointer_get();
	pdata->type_list = type_nl_pointer_get();
	pdata->stat_list = stat_nl_pointer_get();
	pdata->item_list = item_nl_pointer_get();
	pdata->pokemon_table = table_pointer_get();

	/* Allocate the main info struct */
	pdata_priv->info = malloc(sizeof(struct pokemon_info));

	pdata_priv->storage = furi_record_open(RECORD_STORAGE);
	pdata_priv->asset_path = furi_string_alloc_set(APP_ASSETS_PATH());
	storage_common_resolve_path_and_ensure_app_directory(pdata_priv->storage, pdata_priv->asset_path);

	/* Set up initial data */
	/* Trainer/OT name, not to exceed 7 characters! */
	pokemon_name_set(pdata, STAT_TRAINER_NAME, "Flipper");
	pokemon_name_set(pdata, STAT_OT_NAME, "Flipper");

	/* OT trainer ID# */
	pokemon_stat_set(pdata, STAT_OT_ID, NONE, 42069);

	/* Notes:
	 * Move pp isn't explicitly set up, should be fine
	 * Catch/held isn't explicitly set up, should be okay for only Gen I support now
	 * Status condition isn't explicity let up, would you ever want to?
	 */

	/* Set up initial pokemon and level */
	/* This causes all other stats to be recalculated */
	pokemon_stat_set(pdata, STAT_NUM, NONE, 0); // First Pokemon
	pokemon_stat_set(pdata, STAT_LEVEL, NONE, 2); // Minimum level of 2

	return pdata;
};

#define UINT32_TO_EXP(output_array, input)				\
	do {								\
		(output_array)[2] = (uint8_t)((input) & 0xFF);		\
		(output_array)[1] = (uint8_t)(((input) >> 8) & 0xFF);	\
		(output_array)[0] = (uint8_t)(((input) >> 16) & 0xFF);	\
	} while(0)
#define EXP_TO_UINT32(output, input_array)				\
	do {								\
		(output) = (uint32_t)((input_array)[2]) |		\
			   (uint32_t)(((input_array)[1]) << 8) |	\
			   (uint32_t)(((input_array)[0]) << 16);	\
	} while(0)
#define COPY(dst, src) do { dst = src; } while (0)
#define MEMCPY(dst, src) do { memcpy(dst, src, sizeof(dst)); } while (0)
#define SWAP16(dst, src) do { dst = __builtin_bswap16(src); } while (0)

/* XXX: status_condition left as 0, we don't care about it */
/* XXX: How to handle IV after spc_ev? */
#define GEN_COMMON_FIELDS(X) \
	X(atk,		atk,		SWAP16) \
	X(def,		def,		SWAP16) \
	X(spd,		spd,		SWAP16) \
	X(hp,		hp,		SWAP16) \
	X(max_hp,	max_hp,		SWAP16) \
	X(atk_ev,	atk_ev,		SWAP16) \
	X(def_ev,	def_ev,		SWAP16) \
	X(spd_ev,	spd_ev,		SWAP16) \
	X(spc_ev,	spc_ev,		SWAP16) \
	X(hp_ev,	hp_ev,		SWAP16) \
	X(iv,		iv.ivs,		SWAP16) \
	X(level,	level,		COPY) \
	X(move,		move,		MEMCPY) \
	X(move_pp,	move_pp,	MEMCPY) \
	X(ot_id,	ot_id,		SWAP16) \
	X(exp,		exp,		EXP) \
	X(catch_held,	catch_held,	COPY)

#define GEN_I_FIELDS(X) \
	GEN_COMMON_FIELDS(X) \
	X(spc,		spc_atk,	SWAP16) \
	X(level_again,	level,		COPY) \
	X(type,		type,		MEMCPY)

/* status_condition and move_pp are left as 0, we don't care about them */
/* Friendship, too. */
/* caught data, too, how does that work? */
/* XXX: How to handle IV after spc_ev? */
/* XXX: trainer_id is unused? */
#define GEN_II_FIELDS(X) \
	GEN_COMMON_FIELDS(X) \
	X(spc_atk,	spc_atk,	SWAP16) \
	X(spc_def,	spc_def,	SWAP16) \
	X(index,	index,		COPY) \
	X(pokerus,	pokerus,	COPY)

TradeBlock* pokemon_data_trade_block_get(PokemonData* pdata, TradeBlock* tb)
{
	struct pdata_priv* priv = pdata->priv;
	struct pokemon_info* info = priv->info;
	TradeBlockGenI* tbgen1 = NULL;
	TradeBlockGenII* tbgen2 = NULL;

	/* This can be called without alloc being called previously, if tb is
	 * NULL, then alloc and get from the info struct.
	 */
	/* If tb is not NULL, verify that the generation it was allocated
	 * for matches the current generation pdata is configured for.
	 */
	if (tb == NULL)
		tb = pokemon_data_trade_block_alloc(pdata);
	else
		furi_check(tb->gen == pdata->gen);

	tbgen1 = tb->trade_block;
	tbgen2 = tb->trade_block;

	switch (pdata->gen) {
	case GEN_I:
		/* Set the first party member to be the index of our pokemon */
		/* Gen I's dex ordering does not correlate with the actual ID
		 * in-game, we have to look up the index from the table.
		 */
		tbgen1->party_members[0] = table_stat_base_get(pdata->pokemon_table,
							       info->index-1,
							       STAT_BASE_INDEX,
							       NONE);
		tbgen1->party[0].index = tbgen1->party_members[0];

		/* Set the pokemon's OT name */
		pokemon_encoded_array_clear(tbgen1->ot_name[0].str, LEN_NAME_BUF);
		pokemon_str_to_encoded_array(tbgen1->ot_name[0].str, info->ot_name, LEN_OT_NAME-1);

		/* Set the pokemon's nickname */
		pokemon_encoded_array_clear(tbgen1->nickname[0].str, LEN_NAME_BUF);
		pokemon_str_to_encoded_array(tbgen1->nickname[0].str, info->nickname, LEN_NICKNAME-1);

		/* Unroll our macro above to copy in remaining values
		 * converstion(dst, src);
		 */
		#define EXP UINT32_TO_EXP
		#define X(gen1, common, conversion) conversion(tbgen1->party[0].gen1, info->common);
			GEN_I_FIELDS(X)
		#undef X
		#undef EXP

		break;
	case GEN_II:
		/* Set the first party member to be the index of our pokemon */
		tbgen2->party_members[0] = info->index;

		/* Set the pokemon's OT name */
		pokemon_encoded_array_clear(tbgen2->ot_name[0].str, LEN_NAME_BUF);
		pokemon_str_to_encoded_array(tbgen2->ot_name[0].str, info->ot_name, LEN_OT_NAME-1);

		/* Set the pokemon's nickname */
		pokemon_encoded_array_clear(tbgen2->nickname[0].str, LEN_NAME_BUF);
		pokemon_str_to_encoded_array(tbgen2->nickname[0].str, info->nickname, LEN_NICKNAME-1);

		/* Unroll our macro above to copy in remaining values
		 * converstion(dst, src);
		 */
		#define EXP UINT32_TO_EXP
		#define X(gen2, common, conversion) conversion(tbgen2->party[0].gen2, info->common);
			GEN_II_FIELDS(X)
		#undef X
		#undef EXP

		break;
	default:
		FURI_LOG_E(TAG, "invalid Gen");
        	furi_crash();
		break;
	}

	return tb;
}

/* XXX: This doesn't need pdata, just needs gen callout */
TradeBlock* pokemon_data_trade_block_alloc(PokemonData* pdata)
{
	struct pdata_priv* priv = pdata->priv;
	struct pokemon_info* info = priv->info;
	TradeBlock* tb = malloc(sizeof(TradeBlock));
	TradeBlockGenI* tbgen1 = NULL;
	TradeBlockGenII* tbgen2 = NULL;

	tb->gen = pdata->gen;

	switch (pdata->gen) {
	case GEN_I:
		/* Allocate trade block and set its size for the trade view to use */
		tb->trade_block_sz = sizeof(TradeBlockGenI);
		tb->party_sz = (sizeof(PokemonPartyGenI) * 6);
		tbgen1 = malloc(tb->trade_block_sz);
		tbgen1->party_cnt = 1;

		/* The party_members element needs to be 0xff for unused */
		memset(tbgen1->party_members, 0xFF, sizeof(tbgen1->party_members));

		/* Set the trainer's display name */
		pokemon_encoded_array_clear(tbgen1->trainer_name.str, LEN_NAME_BUF);
		pokemon_str_to_encoded_array(tbgen1->trainer_name.str, info->trainer_name, LEN_OT_NAME-1);

		/* Set up the generic tb struct pointers */
		tb->party = tbgen1->party;
		tb->trade_block = tbgen1;
		break;
	case GEN_II:
		/* Allocate trade block and set its size for the trade view to use */
		tb->trade_block_sz = sizeof(TradeBlockGenII);
		tb->party_sz = sizeof(PokemonPartyGenII) * 6;
		tbgen2 = malloc(tb->trade_block_sz);
		tbgen2->party_cnt = 1;

		/* The party_members element needs to be 0xff for unused */
		memset(tbgen2->party_members, 0xFF, sizeof(tbgen2->party_members));

		/* Set the trainer's display name */
		pokemon_encoded_array_clear(tbgen2->trainer_name.str, LEN_NAME_BUF);
		pokemon_str_to_encoded_array(tbgen2->trainer_name.str, info->trainer_name, LEN_OT_NAME-1);

		/* Set up the generic tb struct pointers */
		tb->party = tbgen2->party;
		tb->trade_block = tbgen2;
		break;
	default:
		FURI_LOG_E(TAG, "invalid Gen");
        	furi_crash();
		break;
	}

	/* Pre-populate the tradeblock with data */
	pokemon_data_trade_block_get(pdata, tb);

	return tb;
}



void pokemon_data_trade_block_free(TradeBlock* tb)
{
	free(tb->trade_block);
	free(tb);
}

void pokemon_data_trade_block_set(PokemonData* pdata, TradeBlock* tb, uint8_t which)
{
	struct pdata_priv* priv = pdata->priv;
	struct pokemon_info* info = priv->info;
	TradeBlockGenI* tbgen1 = NULL;
	TradeBlockGenII* tbgen2 = NULL;
#ifdef DEBUG_DATA
	struct pokemon_info* info_old = NULL;
	void* trade_block_old = NULL;

	info_old = malloc(sizeof(struct pokemon_info));
	memcpy(info_old, priv->info, sizeof(struct pokemon_info));

	trade_block_old = malloc(tb->trade_block_sz);
	memcpy(trade_block_old, tb->trade_block, tb->trade_block_sz);
#endif

	switch (pdata->gen) {
	case GEN_I:
		tbgen1 = tb->trade_block;
		/* Re-sync the info struct with incoming data.
		 * NOTE WELL!
		 * Not every varible in the info struct may be updated!
		 */
		
		/* Set the pokemon's OT name */
		memset(info->ot_name, '\0', LEN_NAME_BUF);
		pokemon_encoded_array_to_str(info->ot_name, tbgen1->ot_name[which].str, LEN_OT_NAME-1);

		/* Set the pokemon's nickname */
		memset(info->nickname, '\0', LEN_NAME_BUF);
		pokemon_encoded_array_to_str(info->nickname, tbgen1->nickname[which].str, LEN_NICKNAME-1);

		/* Unroll our macro above to copy in remaining values
		 * converstion(dst, src);
		 */
		#define EXP EXP_TO_UINT32
		#define X(gen1, common, conversion) conversion(info->common, tbgen1->party[which].gen1);
			GEN_I_FIELDS(X)
		#undef X
		#undef EXP
		break;
	case GEN_II:
		tbgen2 = tb->trade_block;
		/* Re-sync the info struct with incoming data.
		 * NOTE WELL!
		 * Not every varible in the info struct may be updated!
		 */
		
		/* Set the pokemon's OT name */
		memset(info->ot_name, '\0', LEN_NAME_BUF);
		pokemon_encoded_array_to_str(info->ot_name, tbgen2->ot_name[which].str, LEN_OT_NAME-1);

		/* Set the pokemon's nickname */
		memset(info->nickname, '\0', LEN_NAME_BUF);
		pokemon_encoded_array_to_str(info->nickname, tbgen2->nickname[which].str, LEN_NICKNAME-1);

		/* Unroll our macro above to copy in remaining values
		 * converstion(dst, src);
		 */
		#define EXP EXP_TO_UINT32
		#define X(gen2, common, conversion) conversion(info->common, tbgen2->party[which].gen2);
			GEN_II_FIELDS(X)
		#undef X
		#undef EXP
		break;
	}

#ifdef DEBUG_DATA
	//furi_break(false);
	/* Compare contents of new and old here */
	FURI_LOG_D(TAG, "\t\tOld tradeblock\t\tNew tradeblock");
	for (size_t i = 0; i < tb->trade_block_sz; i++)
		FURI_LOG_D(TAG, "0x%04X:\t0x%02X\t\t0x%02X\t%d", i, ((uint8_t *)trade_block_old)[i], ((uint8_t *)tb->trade_block)[i],
				((uint8_t *)trade_block_old)[i] == ((uint8_t *)tb->trade_block)[i] ? 1 : 0);

	FURI_LOG_D(TAG, "\t\tOld info\t\tNew info");
	for (size_t i = 0; i < sizeof(struct pokemon_info); i++)
		FURI_LOG_D(TAG, "0x%04X:\t0x%02X\t\t0x%02X\t%d", i, ((uint8_t *)info_old)[i], ((uint8_t *)info)[i],
				((uint8_t *)info_old)[i] == ((uint8_t *)info)[i] ? 1 : 0);

	free(info_old);
	free(trade_block_old);
#endif
}

void pokemon_data_free(PokemonData* pdata) {
	struct pdata_priv* pdata_priv = NULL;

	furi_assert(pdata);
	pdata_priv = pdata->priv;

	furi_record_close(RECORD_STORAGE);
	if (pdata_priv->bitmap && pdata_priv->bitmap_num != 0) free(pdata_priv->bitmap);
	furi_string_free(pdata_priv->asset_path);
	free(pdata_priv->info);
	free(pdata->priv);
	free(pdata);
	pdata = NULL;
};
