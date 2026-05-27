#ifndef STATS_H
#define STATS_H

#pragma once

/* NOTE: ATK should be first, if not, the enum needs to be adjusted too! */
#define POKEMON_STAT(X) \
	X(ATK)		\
	X(DEF)		\
	X(SPD)		\
	X(SPC)		\
	X(SPC_ATK)	\
	X(SPC_DEF)	\
	X(HP)

typedef enum {
	/* Main stats */
#define X(name) STAT_##name,
	POKEMON_STAT(X)
#undef X
	STAT_END,

	/* Set STAT_OFFS manually to STAT_ATK for clean and consistent use of
	 * enum values for looping.
	 */
	STAT_OFFS = STAT_ATK,

	/* Effort Value (EV) stats */
	STAT_EV_OFFS = STAT_END,
#define X(name) STAT_##name##_EV = STAT_##name + STAT_EV_OFFS,
	POKEMON_STAT(X)
#undef X
	STAT_EV_END,

	/* Individual Value (IV) stats
	 * Note that in Gen I and II, IV is a 16 bit value split in to four
	 * nibbles. The pokemon data info structure is set up with the correct
	 * endianness for these generations to access as a single value, as well
	 * as a union of bitfields.
	 *
	 * All of that is to say, most general purpose access should happen
	 * through individual IVs, rather than the whole IV. And then the
	 * translation to and from trade block format would grab the whole IV.
	 */
	STAT_IV_OFFS = STAT_EV_END,
#define X(name) STAT_##name##_IV = STAT_##name + STAT_IV_OFFS,
	POKEMON_STAT(X)
#undef X
	STAT_IV_END,

	STAT_BASE_OFFS = STAT_IV_END,
#define X(name) STAT_BASE_##name = STAT_##name + STAT_BASE_OFFS,
	POKEMON_STAT(X)
#undef X
	/* NOTE! While accessing SPC/APC_AT will do the correct thing for both
	 * Gen I and Gen II, accessing SPC_DEF for Gen I will return a value
	 * that is not used in Gen I games. This normally isn't an issue, but
	 * is a potential gotcha to be aware of.
	 */
	STAT_BASE_TYPE,
	STAT_BASE_MOVE,
	STAT_BASE_GROWTH,
	STAT_BASE_GENDER_RATIO,
	STAT_BASE_INDEX,
	STAT_BASE_END,

	/* Additional live stats that arn't grouped with the above. */
	STAT_TYPE,
	STAT_MOVE,
	STAT_IV,
	STAT_LEVEL,
	STAT_NUM,
	STAT_CONDITION,
	STAT_NICKNAME,
	STAT_OT_NAME,
	STAT_OT_ID,
	STAT_TRAINER_NAME,
	STAT_SEL,
	STAT_EXP,
	STAT_HELD_ITEM,
	STAT_POKERUS,
} DataStat;

typedef enum {
    MOVE_0 = 0,
    MOVE_1,
    MOVE_2,
    MOVE_3,

    TYPE_0 = 0,
    TYPE_1,

    EXP_0 = 0,
    EXP_1,
    EXP_2,

    NONE = 0, // Just a filler value
} DataStatSub;

#endif // STATS_H
