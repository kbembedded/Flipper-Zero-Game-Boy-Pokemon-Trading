#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_trade_block.h>

/* The GBA's game and trade rules, read from its LinkPlayer, and which of them the Flipper's offer
 * meets (docs/gen3/link-protocol-trade.md §C1, §C3). Pure C, no heap.
 *
 * Every function here runs in the link ISR. They call only gen3_pkm_get,
 * gen3_national_from_internal (a flash table) and gen3_link_player_progress, and keep no stack
 * buffer. Keep it so: never gen3_normalise, gen3_edit_* or a bulk accessor (gen3_data.h) here. */

/* Unknown is 0, so a zeroed status or club reads as Unknown. */
typedef enum {
    Gen3LinkGameUnknown,
    Gen3LinkGameRS,
    Gen3LinkGameE,
    Gen3LinkGameFRLG,
} Gen3LinkGame;

/* By version & 0xFF, the one byte FRLG reads (fr: trade.c: GetGameProgressForLinkTrade): 1 and 2
 * are R/S, 3 is Emerald, 4 and 5 are FRLG, and anything else is Unknown. Only the byte read is
 * FRLG's: FRLG itself puts every value but 1, 2, 4 and 5 in one "Emerald or other" class. The
 * Flipper keeps 3 apart as E, and E and Unknown both act as R/S. */
Gen3LinkGame gen3_link_game(uint16_t version);

typedef struct {
    Gen3LinkGame game;
    /* FRLG: progressFlags & 0x0F. Every other game: true (Ruby never refuses, §B2.5). */
    bool national_dex;
} Gen3PartnerRules;

/* The rules of the GBA whose LinkPlayer is gba. gba NULL gives {Unknown, true}. */
void gen3_partner_rules(const Gen3LinkPlayer* gba, Gen3PartnerRules* out);

/* fr: include/constants/species.h: KANTO_SPECIES_END (SPECIES_MEW) */
#define GEN3_NATIONAL_KANTO_LAST 151

/* Offer traits: what a partner's rules look at, as bits of a uint8_t */
typedef enum {
    Gen3OfferNonKanto = 1u << 0, /* National Dex number above 151, or no valid species */
    Gen3OfferUnfateful = 1u << 1, /* Mew or Deoxys with the fateful-encounter bit 0 */
} Gen3OfferTrait;
/* For an offer whose game form doesn't decrypt: never usable by FRLG */
#define GEN3_OFFER_TRAITS_UNKNOWN (Gen3OfferNonKanto | Gen3OfferUnfateful)

/* The traits of p (decrypted). The checksum isn't read. */
uint8_t gen3_offer_traits(const Gen3Pkm* p);

/* Only an FRLG partner refuses, in this order:
 * - NonKanto without the National Dex -> NationalDex. FRLG's receiver takes such a Pokemon, but
 *   its own sender never sends one to a partner without the National Dex, so the Flipper doesn't
 *   either (link-protocol-trade.md T14).
 * - Unfateful -> Fateful (fr: trade.c: CheckValidityOfTradeMons).
 * Anything else, and every other game: None. */
Gen3Refuse gen3_partner_refuse(const Gen3PartnerRules* rules, uint8_t traits);
