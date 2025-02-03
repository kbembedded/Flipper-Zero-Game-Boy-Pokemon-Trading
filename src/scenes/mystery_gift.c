#include <furi.h>
#include <gui/modules/submenu.h>
#include <infrared_worker.h>

#include <src/include/pokemon_app.h>

#include <src/include/named_list.h>



static const NamedList mg_region_list[] = {
	{ "USA", 0x90, GEN_II },
	{ "ESP", 0x96, GEN_II },
	{ "ITA", 0x99, GEN_II },
	{ "FRA", 0x9a, GEN_II },
	{ "GER", 0x9f, GEN_II },
	{ "2 GS", 0xf3, GEN_II },
	{},
};

static const NamedList mg_item_list[] = {
	{ "Berry",		0x00,	GEN_II },
	{ "PRZCureBerry",	0x01,	GEN_II },
	{ "Mint Berry",	        0x02,	GEN_II },
	{ "Ice Berry",	        0x03,	GEN_II },
	{ "Burnt Berry",	0x04,	GEN_II },
	{ "PSNCureBerry",	0x05,	GEN_II },
	{ "Guard Spec.",	0x06,	GEN_II },
	{ "X Defend",	        0x07,	GEN_II },
	{ "X Attack",	        0x08,	GEN_II },
	{ "Bitter Berry",	0x09,	GEN_II },
	{ "Dire Hit",	        0x0A,	GEN_II },
	{ "X Special",	        0x0B,	GEN_II },
	{ "X Accuracy",	        0x0C,	GEN_II },
	{ "Eon Mail",	        0x0D,	GEN_II },
	{ "Morph Mail",	        0x0E,	GEN_II },
	{ "Music Mail",	        0x0F,	GEN_II },
	{ "MiracleBerry",	0x10,	GEN_II },
	{ "Gold Berry",	        0x11,	GEN_II },
	{ "Revive",	        0x12,	GEN_II },
	{ "Great Ball",	        0x13,	GEN_II },
	{ "Super Repel",	0x14,	GEN_II },
	{ "Max Repel",	        0x15,	GEN_II },
	{ "Elixir",	        0x16,	GEN_II },
	{ "Ether",	        0x17,	GEN_II },
	{ "Water Stone",	0x18,	GEN_II },
	{ "Fire Stone",	        0x19,	GEN_II },
	{ "Leaf Stone",	        0x1A,	GEN_II },
	{ "Thunderstone",	0x1B,	GEN_II },
	{ "Max Ether",	        0x1C,	GEN_II },
	{ "Max Elixir",	        0x1D,	GEN_II },
	{ "Max Revive",	        0x1E,	GEN_II },
	{ "Scope Lens",	        0x1F,	GEN_II },
	{ "HP Up",	        0x20,	GEN_II },
	{ "PP Up",	        0x21,	GEN_II },
	{ "Rare Candy",	        0x22,	GEN_II },
	{ "Bluesky Mail",	0x23,	GEN_II },
	{ "Mirage Mail",	0x24,	GEN_II },
	{ "Great Ball",	        0x25,	GEN_II },
	{},
};

static const NamedList mg_decoration_list[] = {
	{ "Jigglypuff Doll",         0x00,	GEN_II },
	{ "Poliwag Doll",            0x01,	GEN_II },
	{ "Diglett Doll",            0x02,	GEN_II },
	{ "Staryu Doll",             0x03,	GEN_II },
	{ "Magikarp Doll",           0x04,	GEN_II },
	{ "Oddish Doll",             0x05,	GEN_II },
	{ "Gengar Doll",             0x06,	GEN_II },
	{ "Shellder Doll",           0x07,	GEN_II },
	{ "Grimer Doll",             0x08,	GEN_II },
	{ "Voltorb Doll",            0x09,	GEN_II },
	{ "Clefairy Poster",         0x0A,	GEN_II },
	{ "Jigglypuff Poster",       0x0B,	GEN_II },
	{ "Super NES",               0x0C,	GEN_II },
	{ "Weedle Doll",             0x0D,	GEN_II },
	{ "Geodude Doll",            0x0E,	GEN_II },
	{ "Machop Doll",             0x0F,	GEN_II },
	{ "Magna Plant",             0x10,	GEN_II },
	{ "Tropic Plant",            0x11,	GEN_II },
	{ "NES",                     0x12,	GEN_II },
	{ "Nintendo 64",             0x13,	GEN_II },
	{ "Bulbasaur Doll",          0x14,	GEN_II },
	{ "Squirtle Doll",           0x15,	GEN_II },
	{ "Pink Bed",                0x16,	GEN_II },
	{ "Polkadot Bed",            0x17,	GEN_II },
	{ "Red Carpet",              0x18,	GEN_II },
	{ "Blue Carpet",             0x19,	GEN_II },
	{ "Yellow Carpet",           0x1A,	GEN_II },
	{ "Green Carpet",            0x1B,	GEN_II },
	{ "Jumbo Plant",             0x1C,	GEN_II },
	{ "Virtual Boy",             0x1D,	GEN_II },
	{ "Big Onix Doll",           0x1E,	GEN_II },
	{ "Pikachu Poster",          0x1F,	GEN_II },
	{ "Big Lapras Doll",         0x20,	GEN_II },
	{ "Surf Pikachu Doll",       0x21,	GEN_II },
	{ "Pikachu Bed",             0x22,	GEN_II },
	{ "Unown Doll",              0x23,	GEN_II },
	{ "Tentacool Doll",          0x24,	GEN_II },
	{ "Red Carpet",			0x25,	GEN_II },
	{},
};

static void mystery_gift_start_callback(void *context)
{
	furi_assert(context);
	UNUSED(context);


}

struct mystery_gift {
    FuriMutex* mutex;
    InfraredWorker* ir_work;
};

struct mystery_gift *mg_ctx = NULL;

void mystery_gift_scene_on_enter(void* context) {
    PokemonFap* pokemon_fap = (PokemonFap*)context;

    mg_ctx = malloc(sizeof(struct mystery_gift));

   /* Set up mutex */
   mg_ctx->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    /* Set up IR worker */
    mg_ctx->ir_work = infrared_worker_alloc();
    infrared_worker_rx_enable_blink_on_receiving(mg_ctx->ir_work, true);
    infrared_worker_set_raw_signal(mg_ctx->ir_work, timings_array, timings_count, 38000, 0.33);
    /* XXX: Blink on TX somehow */

    submenu_reset(pokemon_fap->submenu);

    submenu_add_item(
        pokemon_fap->submenu, "Go-baby-go!", 0, mystery_gift_start_callback, pokemon_fap);
}
