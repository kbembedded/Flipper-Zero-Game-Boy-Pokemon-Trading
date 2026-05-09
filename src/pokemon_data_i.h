#ifndef __POKEMON_DATA_I_H__
#define __POKEMON_DATA_I_H__

typedef struct pokemon_party_data_gen_i PokemonPartyGenI;
typedef struct trade_block_gen_i TradeBlockGenI;
typedef struct pokemon_party_data_gen_ii PokemonPartyGenII;
typedef struct trade_block_gen_ii TradeBlockGenII;

/* Creat a uniform, generation independent struct to represent the current pokemon
 * in the Flipper's memory. These are then copied into a struct that is laid out
 * in memory for the target trade generation. Upon receiving a traded pokemon,
 * that struct is back-copied into this struct.
 *
 * This takes up additional memory to keep multiple copies like that, but
 * greatly reduces code complexity and potential mistakes, and likely reduces
 * code size far more than this adds memory pressure. We also need to track
 * far fewer bytes as many of the final memory values are calculated.
 */
/* XXX: This is maintained in flipper endianness! */
/* XXX: Need to include all of the variables that could ever be in any trade
 * struct, methinks...
 */
typedef union {
	struct {
		uint16_t spc:4;
		uint16_t spd:4;
		uint16_t def:4;
		uint16_t atk:4;
	} iv;
	uint16_t ivs;
} iv_union;

struct __attribute__((__packed__)) pokemon_info {
	uint8_t index;
	uint32_t exp;
	//uint32_t exp;
	char ot_name[LEN_NAME_BUF];
	char trainer_name[LEN_NAME_BUF];
	char nickname[LEN_NAME_BUF];
	/* XXX: Do we want to track HP and max HP separately?
	 * Why? is there some benefit to keeping that information?
	 * On the one hand, it would allow us to keep exact copies of what
	 * was traded to us. On the other, but why?
	 */
	uint16_t hp;
	uint16_t max_hp;
	/* Level is normally calculated from exp, however, level is more human
	 * readable compared to just exp. Therefore, we only let the user set
	 * the level and then back calculate exp from that as well as Max HP,
	 * ATK, DEF, SPD, SPC.
	 */
	uint8_t level;
	/* Type is only something can can be modified in gen I, and even then
	 * any moves that modify type (transform, etc.) will clobber these later.
	 */
	/* XXX: We might also want to track exp separately, that way if we're
	 * traded in a pokemon, we don't lose the level and recalculate it all.
	 */
	uint8_t type[2];
	/* catch_held has multiple purposes depending on generation. However,
	 * we really only use it for held item in gen II. In gen I, this value
	 * is the catch rate, which, when transferring between generations can
	 * leave the pokemon holding a random item equivalent to its catch rate.
	 */
	uint8_t catch_held;
	uint8_t move[4];
	uint8_t move_pp[4];
	uint16_t ot_id;
	uint16_t hp_ev;
	uint16_t atk_ev;
	uint16_t def_ev;
	uint16_t spd_ev;
	uint16_t spc_ev;
	iv_union iv;
	uint8_t pokerus;

	/* Right now, these values are calculated from exp, et al., it may make sense
	 * to instead JIT calculate when going from this struct to the real
	 * trade block. However, all of our calculation code is expecting
	 * to set the data live to the trade block.
	 */
	uint16_t atk;
	uint16_t def;
	uint16_t spd;
	uint16_t spc_atk;
	uint16_t spc_def;

	/* Current EV/IV stat selection */
	EvIv stat_sel;
};

/* The struct is laid out exactly as the data trasfer that gets sent for trade
 * information. It has to be packed in order to not have padding in the Flipper.
 * Packing is always potentially filled with pitfalls, however this has worked
 * in testing without issue and this code isn't meant to be portable.
 */

/* NOTE: These are all opposite endianness on the flipper than they are in the
 * GB/Z80. e.g. a uint16_t value of 0x2c01 translates to 0x012c.
 * Need to use __builtin_bswap16(val) to switch between Flipper and Pokemon.
 */
/* This is 44 bytes in memory */
struct __attribute__((__packed__)) pokemon_party_data_gen_i {
    uint8_t index;
    uint16_t hp; // Calculated from level
    /* Level is normally calculated from exp, however, level is more human
     * readable/digestable compared to exp. Therefore, we set legel and then
     * from that calculate, (Max)HP, ATK, DEF, SPD, SPC.
     */
    uint8_t level;
    uint8_t status_condition; // Do you really want to trade a Poisoned pokemon?
    uint8_t type[2]; // Pokemon with a single type just repeat the type twice
    uint8_t catch_held; // Unsure if this has any effect in Gen 1
    uint8_t move[4];
    uint16_t ot_id;
    uint8_t exp[3]; // Calculated from level
    uint16_t hp_ev;
    uint16_t atk_ev;
    uint16_t def_ev;
    uint16_t spd_ev;
    uint16_t spc_ev;
    uint16_t iv;
    uint8_t move_pp[4];
    uint8_t level_again; // Copy of level
    uint16_t max_hp; // Calculated from level
    uint16_t atk; // Calculated from level
    uint16_t def; // Calculated from level
    uint16_t spd; // Calculated from level
    uint16_t spc; // Calculated from level
};

struct __attribute__((__packed__)) name {
    /* Reused a few times, but in Gen I, all name strings are 11 bytes in memory.
     * At most, 10 symbols and a TERM_ byte.
     * Note that some strings must be shorter than 11.
     */
    uint8_t str[LEN_NAME_BUF];
};
typedef struct name Name;

/* This is 415 bytes in memory/transmitted */
struct __attribute__((__packed__)) trade_block_gen_i {
    Name trainer_name;
    uint8_t party_cnt;
    /* Only the first pokemon is ever used even though there are 7 bytes here.
     * If the remaining 6 bytes are _not_ 0xff, then the trade window renders
     * garbage for the Flipper's party.
     */
    uint8_t party_members[7];
    /* Only the first pokemon is set up, even though there are 6 total party members */
    PokemonPartyGenI party[6];
    /* XXX: TODO: Instead of having an array of 6 party members, have a single party
     * and then 5 more "dead" party entries*/
    /* Only the first pokemon has an OT name and nickname even though there are 6 members */
    /* OT name should not exceed 7 chars! */
    Name ot_name[6];
    Name nickname[6];
};

/* NOTE: These are all opposite endianness on the flipper than they are in the
 * GB/Z80. e.g. a uint16_t value of 0x2c01 translates to 0x012c.
 * Need to use __builtin_bswap16(val) to switch between Flipper and Pokemon.
 */
/* This is 48 bytes in memory */
struct __attribute__((__packed__)) pokemon_party_data_gen_ii {
    uint8_t index;
    uint8_t catch_held; // Named to match gen I use, its the held item
    uint8_t move[4];
    uint16_t ot_id;
    uint8_t exp[3];
    uint16_t hp_ev;
    uint16_t atk_ev;
    uint16_t def_ev;
    uint16_t spd_ev;
    uint16_t spc_ev;
    uint16_t iv;
    uint8_t move_pp[4];
    /* After a trade, friendship is always reset to 120, 70?, no use changing it */
    uint8_t friendship;
    uint8_t pokerus;
    /* Only used in Crystal, not worth implementing unless someone really wants it */
    uint16_t caught_data;
    /* Level is normally calculated from exp, however, level is more human
     * readable/digestable compared to exp. Therefore, we set level and then
     * from that calculate, (Max)HP, ATK, DEF, SPD, SPC.
     */
    uint8_t level;
    uint8_t status_condition;
    uint8_t unused;
    uint16_t hp;
    uint16_t max_hp;
    uint16_t atk;
    uint16_t def;
    uint16_t spd;
    uint16_t spc_atk;
    uint16_t spc_def;
};

/* NOTE:
 * For eggs in gen ii, the handling is a bit clever. The party structure is set
 * up as normal for the pokemon that will hatch. The only difference is the
 * friendship vairable is used to denote number of egg cycles remaining.
 * Then, in the party_members array, that pokemon's index is set to 0xFD which
 * is the index for an egg. Once traded, its now an egg.
 * Creating an egg is not implemented at this time because I don't really see
 * a reason to. But, knowing some of these details makes it really easy to
 * implement later on.
 */

struct __attribute__((__packed__)) trade_block_gen_ii {
    Name trainer_name;
    uint8_t party_cnt;
    /* Only the first pokemon is ever used even though there are 7 bytes here.
     * If the remaining 6 bytes are _not_ 0xff, then the trade window renders
     * garbage for the Flipper's party.
     */
    uint8_t party_members[7];
    uint16_t trainer_id;
    /* Only the first pokemon is set up, even though there are 6 total party members */
    PokemonPartyGenII party[6];
    /* XXX: TODO: Instead of having an array of 6 party members, have a single party
     * and then 5 more "dead" party entries*/
    /* Only the first pokemon has an OT name and nickname even though there are 6 members */
    /* OT name should not exceed 7 chars! */
    Name ot_name[6];
    Name nickname[6];
};

#endif // __POKEMON_DATA_I_H__
