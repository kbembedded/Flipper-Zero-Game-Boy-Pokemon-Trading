#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GEN3_LINK_PLAYER_BLOCK_SIZE  60
#define GEN3_TRAINER_CARD_BLOCK_SIZE 100
#define GEN3_LINKTYPE_TRADE_SETUP    0x1133
/* LinkPlayer versions are this + the game's VERSION_* (fr: link.c: InitLocalLinkPlayer;
 * fr: include/constants/global.h). Every receiver reads only the low byte. */
#define GEN3_LP_VERSION_BASE         0x4000
#define GEN3_VERSION_RUBY            (GEN3_LP_VERSION_BASE + 2)
#define GEN3_LANGUAGE_ENGLISH        2

typedef struct {
    uint16_t version;
    uint16_t lp_field_2;
    uint32_t trainer_id; /* low 16 = TID, high 16 = SID */
    uint8_t name[11];
    uint8_t gender;
    uint32_t link_type;
    uint16_t id;
    uint16_t language;
} Gen3LinkPlayer;

void gen3_link_player_block_build(const Gen3LinkPlayer* lp, uint8_t out[GEN3_LINK_PLAYER_BLOCK_SIZE]);

/* false if either magic's first 15 bytes differ from "GameFreak inc.\0" (byte 15 is not compared) */
bool gen3_link_player_block_parse(const uint8_t in[GEN3_LINK_PLAYER_BLOCK_SIZE], Gen3LinkPlayer* lp);

/* 100-byte trainer card for block request type 2 (§A5 table), from lp's gender, TID and name[0..7] */
void gen3_trainer_card_build(const Gen3LinkPlayer* lp, uint8_t out[GEN3_TRAINER_CARD_BLOCK_SIZE]);

/* Part B link types (docs/gen3/link-protocol-trade.md §B0) */
#define GEN3_LINKTYPE_TRADE              0x1111 /* room again after the trade menu (§B0 row 1') */
#define GEN3_LINKTYPE_TRADE_CONNECTING   0x1122 /* trade menu (§B1.6) */
#define GEN3_LINKTYPE_TRADE_DISCONNECTED 0x1144 /* trade animation and link save (§B4.1) */
/* lp_field_2 that makes Ruby unlock its National Dex (§B4.5) */
#define GEN3_LP_NATIONAL_DEX 0x8000

/* The other games' LinkPlayer versions, as GEN3_VERSION_RUBY above */
#define GEN3_VERSION_SAPPHIRE  (GEN3_LP_VERSION_BASE + 1)
#define GEN3_VERSION_EMERALD   (GEN3_LP_VERSION_BASE + 3)
#define GEN3_VERSION_FIRERED   (GEN3_LP_VERSION_BASE + 4)
#define GEN3_VERSION_LEAFGREEN (GEN3_LP_VERSION_BASE + 5)

/* FRLG and Emerald give name[8..10] (block 0x20..0x22) to progressFlags, neverRead and
 * progressFlagsCopy (fr: include/link.h: struct LinkPlayer; Part C §C1). */
#define GEN3_LP_PROGRESS_INDEX 8
/* Has the National Dex: FRLG reads bits 0-3 as one flag (fr: trade.c: CanTradeSelectedMon tests
 * progressFlagsCopy & 0xF), and sets bit 0 for it itself (fr: link.c: InitLocalLinkPlayer) */
#define GEN3_FRLG_PROGRESS_NATIONAL_DEX_MASK 0x0F
#define GEN3_FRLG_PROGRESS_NATIONAL_DEX      0x01
/* FLAG_SYS_CAN_LINK_WITH_RS (fr: link.c: InitLocalLinkPlayer) */
#define GEN3_FRLG_PROGRESS_CAN_LINK_RS 0x10
/* struct TrainerCard.version, after the Ruby layout (fr: include/trainer_card.h) */
#define GEN3_FRLG_CARD_VERSION 0x38

/* The three below run in the link ISR, on the GBA's LinkPlayerBlock and in the club's claim.
 * They are pure, with no heap and no stack buffer. Keep it so. */

/* name[8], the GBA's progressFlags. Never name[10]: a GBA doesn't write its own
 * progressFlagsCopy (only a receiver does, fr: link.c: ConvertLinkPlayerName), so it goes out
 * stale. */
uint8_t gen3_link_player_progress(const Gen3LinkPlayer* lp);
/* §C1: name[8] = flags, name[9] (neverRead) = 0, name[10] (progressFlagsCopy) = flags.
 * name[0..7] is unchanged. */
void gen3_link_player_set_progress(Gen3LinkPlayer* lp, uint8_t flags);
/* §C2, the FRLG card: bytes 0x00..0x37 as gen3_trainer_card_build (FRLG's struct
 * TrainerCardRSE), [0x38] = lp->version & 0xFF, which selects the Kanto card
 * (fr: trainer_card.c: GetCardType), and 0x39..0x63 zero. */
void gen3_trainer_card_build_frlg(
    const Gen3LinkPlayer* lp,
    uint8_t out[GEN3_TRAINER_CARD_BLOCK_SIZE]);
