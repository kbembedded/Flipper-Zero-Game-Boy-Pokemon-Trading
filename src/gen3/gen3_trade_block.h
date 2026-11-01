#pragma once

#include <stdint.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_pkm.h>

/* Part B of docs/gen3/link-protocol-trade.md: the 20-byte trade-menu blocks (§B3), the five
 * party-exchange rounds (§B2) and the rules for what the Flipper may offer or take (§B2.5, D8).
 * Pure C, no heap. */

#define GEN3_TRADE_BLOCK_SIZE 20 /* §B3.1 */
#define GEN3_TRADE_ROUNDS     5 /* §B2.1 */
#define GEN3_TRADE_ROUND_MAX  220 /* largest round block */

/* Trade-menu codes: word 0 of a 20-byte block, never word 0 of a burst (§B3) */
#define GEN3_TRADE_READY          0xAABB /* GBA: TRADE on its slot arg */
#define GEN3_TRADE_CANCEL         0xEEAA /* GBA: Cancel trade? YES */
#define GEN3_TRADE_CONFIRM        0xBBBB /* GBA: Is this trade okay? YES */
#define GEN3_TRADE_CONFIRM_NO     0xBBCC /* GBA: NO, or B */
#define GEN3_TRADE_SET_MONS       0xDDDD /* Flipper: offer its slot arg */
#define GEN3_TRADE_PARTNER_CANCEL 0xEECC
#define GEN3_TRADE_PLAYER_CANCEL  0xDDEE /* "The trade has been canceled." */
#define GEN3_TRADE_BOTH_CANCEL    0xEEBB /* back to the room */
#define GEN3_TRADE_START          0xCCDD
#define GEN3_TRADE_ANIM_DONE      0xABCD /* GBA, end of the animation (§B4.3) */
#define GEN3_TRADE_ANIM_ACK       0xDCBA /* Flipper, after the GBA's ABCD */

/* Mail is GEN3_ITEM_MAIL_FIRST..GEN3_ITEM_MAIL_LAST (gen3_data.h). */
#define GEN3_ITEM_ENIGMA_BERRY 175
#define GEN3_RUBY_ITEMS_COUNT  349 /* ITEMS_COUNT in Ruby; 349..376 are FRLG/Emerald items */

/* Words: 0 = code, 1 = arg, 2..9 = 0; little-endian. */
void gen3_trade_block_build(uint16_t code, uint16_t arg, uint8_t out[GEN3_TRADE_BLOCK_SIZE]);
/* Word 0; word 1 into *arg unless arg is NULL. in holds GEN3_TRADE_BLOCK_SIZE bytes. */
uint16_t gen3_trade_block_code(const uint8_t* in, uint16_t* arg);

/* Round 1..5 of §B2.1: CCCC request type 1,1,1,3,4 and block size 200,200,200,220,40.
 * Any other round: 0xFFFF and 0. */
uint16_t gen3_trade_round_request(uint8_t round);
uint16_t gen3_trade_round_size(uint8_t round);
/* The Flipper's block for round 1..5, gen3_trade_round_size(round) bytes: round 1 is mon (game
 * form; NULL -> zeros) then 100 zero bytes (the empty slot 1, §B2.3); every other round zeros.
 * Any other round writes nothing. */
void gen3_trade_round_block(uint8_t round, const uint8_t* mon, uint8_t* out);

/* Why the Flipper won't offer, or take, a Pokemon. Only gen3_partner_refuse (gen3_partner.h)
 * returns the last two. gen3_trade_offer_check and gen3_trade_check_incoming never do, so the
 * club's refused never holds them. */
typedef enum {
    Gen3RefuseNone,
    Gen3RefuseEmpty, /* species 0 (check_incoming only) */
    Gen3RefuseBadEgg, /* checksum mismatch or Bad Egg flag */
    Gen3RefuseEgg,
    Gen3RefuseMail, /* held item 121..132, or mail byte != 0xFF */
    Gen3RefuseItem, /* held item 175, or >= 349 */
    Gen3RefuseBadData, /* species, move or item outside what gen3_pkm_check accepts */
    /* An FRLG partner without the National Dex, and an offer above #151: FRLG's own sender rule,
     * mirrored (link-protocol-trade.md §C3 and T14) */
    Gen3RefuseNationalDex,
    /* An FRLG partner, and Mew or Deoxys without the fateful-encounter bit (§C3) */
    Gen3RefuseFateful,
} Gen3Refuse;

/* Both checks run in the link ISR. They call only gen3_pkm_decrypt, gen3_pkm_check, gen3_pkm_get
 * and flash tables, and keep no large stack local. Keep it so: never gen3_normalise, gen3_edit_*
 * or a bulk accessor (gen3_data.h) here. */

/* May the Flipper offer p (decrypted)? In order: gen3_pkm_check (BadEgg -> BadEgg, Egg -> Egg,
 * any other failure -> BadData), mail, item. The checksum isn't checked. */
Gen3Refuse gen3_trade_offer_check(const Gen3Pkm* p);
/* May the Flipper take the GBA's mon (game form)? decrypt fails -> BadEgg; species 0 -> Empty;
 * else gen3_trade_offer_check. scratch receives the decrypted mon, and is unchanged when the
 * decrypt fails. */
Gen3Refuse gen3_trade_check_incoming(const uint8_t game[GEN3_PARTY_SIZE], Gen3Pkm* scratch);
