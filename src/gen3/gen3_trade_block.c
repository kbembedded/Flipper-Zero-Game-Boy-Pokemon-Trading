#include <src/gen3/gen3_trade_block.h>

#include <string.h>
#include <src/gen3/gen3_pk3.h>

/* §B2.1: what each round carries, and the CCCC request type that asks for it */
static const struct {
    uint16_t request;
    uint16_t size;
} gen3_trade_round_tab[GEN3_TRADE_ROUNDS] = {
    {1, 200}, /* party slots 0..1 */
    {1, 200}, /* party slots 2..3 */
    {1, 200}, /* party slots 4..5 */
    {3, 220}, /* mail[0..5] (36 bytes each) + 4 bytes */
    {4, 40}, /* giftRibbons[11], the rest stale */
};

void gen3_trade_block_build(uint16_t code, uint16_t arg, uint8_t out[GEN3_TRADE_BLOCK_SIZE]) {
    memset(out, 0, GEN3_TRADE_BLOCK_SIZE);
    out[0] = (uint8_t)(code & 0xFF);
    out[1] = (uint8_t)(code >> 8);
    out[2] = (uint8_t)(arg & 0xFF);
    out[3] = (uint8_t)(arg >> 8);
}

uint16_t gen3_trade_block_code(const uint8_t* in, uint16_t* arg) {
    if(arg) *arg = (uint16_t)(in[2] | (in[3] << 8));
    return (uint16_t)(in[0] | (in[1] << 8));
}

uint16_t gen3_trade_round_request(uint8_t round) {
    if(round < 1 || round > GEN3_TRADE_ROUNDS) return 0xFFFF;
    return gen3_trade_round_tab[round - 1].request;
}

uint16_t gen3_trade_round_size(uint8_t round) {
    if(round < 1 || round > GEN3_TRADE_ROUNDS) return 0;
    return gen3_trade_round_tab[round - 1].size;
}

void gen3_trade_round_block(uint8_t round, const uint8_t* mon, uint8_t* out) {
    memset(out, 0, gen3_trade_round_size(round));
    /* The Flipper's party is one Pokemon: slot 0, then empty slots (§B2.3, §B2.5) */
    if(round == 1 && mon) memcpy(out, mon, GEN3_PARTY_SIZE);
}

Gen3Refuse gen3_trade_offer_check(const Gen3Pkm* p) {
    Gen3Check c = gen3_pkm_check(p);
    if(c == Gen3CheckBadEgg) return Gen3RefuseBadEgg;
    if(c == Gen3CheckEgg) return Gen3RefuseEgg;
    if(c != Gen3CheckOk) return Gen3RefuseBadData;

    /* §B2.5 rule 4: a mail index makes Ruby replace the held item with the mail's (§B4.4) */
    uint32_t item = gen3_pkm_get(p, Gen3FieldHeldItem);
    if((item >= GEN3_ITEM_MAIL_FIRST && item <= GEN3_ITEM_MAIL_LAST) ||
       gen3_pkm_get(p, Gen3FieldMail) != GEN3_MAIL_NONE)
        return Gen3RefuseMail;
    /* §B2.5 rules 2 and 3 */
    if(item == GEN3_ITEM_ENIGMA_BERRY || item >= GEN3_RUBY_ITEMS_COUNT) return Gen3RefuseItem;
    return Gen3RefuseNone;
}

Gen3Refuse gen3_trade_check_incoming(const uint8_t game[GEN3_PARTY_SIZE], Gen3Pkm* scratch) {
    /* Ruby shows a checksum mismatch as a Bad Egg */
    if(!gen3_pkm_decrypt(game, GEN3_PARTY_SIZE, scratch)) return Gen3RefuseBadEgg;
    if(gen3_pkm_get(scratch, Gen3FieldSpecies) == 0) return Gen3RefuseEmpty;
    return gen3_trade_offer_check(scratch);
}
