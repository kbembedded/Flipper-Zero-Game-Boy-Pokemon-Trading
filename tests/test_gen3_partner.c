#include "test.h"
#include "bulk_file.h"
#include "trade_mons.h"
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_partner.h>

TEST(test_link_game) {
    ASSERT_EQ(Gen3LinkGameUnknown, 0); /* a zeroed status or club reads as Unknown */
    ASSERT_EQ(gen3_link_game(0x4001), Gen3LinkGameRS);
    ASSERT_EQ(gen3_link_game(0x4002), Gen3LinkGameRS);
    ASSERT_EQ(gen3_link_game(0x4003), Gen3LinkGameE);
    ASSERT_EQ(gen3_link_game(0x4004), Gen3LinkGameFRLG);
    ASSERT_EQ(gen3_link_game(0x4005), Gen3LinkGameFRLG);
    ASSERT_EQ(gen3_link_game(0x0104), Gen3LinkGameFRLG); /* the low byte only */
    ASSERT_EQ(gen3_link_game(0x4000), Gen3LinkGameUnknown);
    ASSERT_EQ(gen3_link_game(0x4006), Gen3LinkGameUnknown);
    ASSERT_EQ(gen3_link_game(0x40FF), Gen3LinkGameUnknown);
    ASSERT_EQ(gen3_link_game(0), Gen3LinkGameUnknown);
    ASSERT_EQ(gen3_link_game(GEN3_VERSION_SAPPHIRE), Gen3LinkGameRS);
    ASSERT_EQ(gen3_link_game(GEN3_VERSION_RUBY), Gen3LinkGameRS);
    ASSERT_EQ(gen3_link_game(GEN3_VERSION_EMERALD), Gen3LinkGameE);
    ASSERT_EQ(gen3_link_game(GEN3_VERSION_FIRERED), Gen3LinkGameFRLG);
    ASSERT_EQ(gen3_link_game(GEN3_VERSION_LEAFGREEN), Gen3LinkGameFRLG);
}

/* A GBA's LinkPlayer, by hand: the version, and name[8] (block 0x20) and name[10] (block 0x22)
 * as raw bytes. The rest of the name is EOS. */
static void gba_lp(Gen3LinkPlayer* lp, uint16_t version, uint8_t name8, uint8_t name10) {
    memset(lp, 0, sizeof(*lp));
    lp->version = version;
    memset(lp->name, 0xFF, sizeof(lp->name));
    lp->name[8] = name8;
    lp->name[10] = name10;
    lp->link_type = 0x1133;
    lp->language = 2;
}

/* gen3_partner_rules on that LinkPlayer is {game, natdex}; out starts as neither, so every
 * field must be written */
static bool
    rules_are(uint16_t version, uint8_t name8, uint8_t name10, Gen3LinkGame game, bool natdex) {
    Gen3LinkPlayer lp;
    gba_lp(&lp, version, name8, name10);
    Gen3PartnerRules r = {.game = (Gen3LinkGame)0x55, .national_dex = !natdex};
    gen3_partner_rules(&lp, &r);
    if(r.game == game && r.national_dex == natdex) return true;
    printf(
        "  rules of 0x%04X [8]=0x%02X [10]=0x%02X: {%d, %d}, want {%d, %d}\n",
        version,
        name8,
        name10,
        r.game,
        r.national_dex,
        game,
        natdex);
    return false;
}

TEST(test_partner_rules) {
    /* FRLG: progressFlags & 0x0F */
    ASSERT_TRUE(rules_are(0x4004, 0x01, 0x01, Gen3LinkGameFRLG, true));
    ASSERT_TRUE(rules_are(0x4004, 0x0F, 0x0F, Gen3LinkGameFRLG, true));
    ASSERT_TRUE(rules_are(0x4004, 0x11, 0x11, Gen3LinkGameFRLG, true));
    /* Any bit of the low nibble: FRLG tests progressFlagsCopy & 0xF, not bit 0 alone */
    ASSERT_TRUE(rules_are(0x4004, 0x02, 0x02, Gen3LinkGameFRLG, true));
    ASSERT_TRUE(rules_are(0x4004, 0x08, 0x08, Gen3LinkGameFRLG, true));
    ASSERT_EQ(GEN3_FRLG_PROGRESS_NATIONAL_DEX_MASK, 0x0F);
    ASSERT_TRUE(rules_are(0x4004, 0x10, 0x10, Gen3LinkGameFRLG, false));
    ASSERT_TRUE(rules_are(0x4004, 0x00, 0x00, Gen3LinkGameFRLG, false));
    ASSERT_TRUE(rules_are(0x4005, 0x01, 0x01, Gen3LinkGameFRLG, true));
    ASSERT_TRUE(rules_are(0x4005, 0x00, 0x00, Gen3LinkGameFRLG, false));
    /* Block 0x22 is never read: a real GBA never writes its own progressFlagsCopy */
    ASSERT_TRUE(rules_are(0x4004, 0x01, 0x00, Gen3LinkGameFRLG, true));
    ASSERT_TRUE(rules_are(0x4004, 0x00, 0x01, Gen3LinkGameFRLG, false));
    /* Every other game: true, whatever name[8] holds */
    ASSERT_TRUE(rules_are(0x4002, 0xFF, 0xFF, Gen3LinkGameRS, true));
    ASSERT_TRUE(rules_are(0x4002, 0x00, 0x00, Gen3LinkGameRS, true));
    ASSERT_TRUE(rules_are(0x4001, 0x00, 0x00, Gen3LinkGameRS, true));
    ASSERT_TRUE(rules_are(0x4003, 0x00, 0x00, Gen3LinkGameE, true));
    ASSERT_TRUE(rules_are(0x4000, 0x00, 0x00, Gen3LinkGameUnknown, true));
    /* No GBA LinkPlayer yet */
    Gen3PartnerRules r = {.game = Gen3LinkGameFRLG, .national_dex = false};
    gen3_partner_rules(NULL, &r);
    ASSERT_EQ(r.game, Gen3LinkGameUnknown);
    ASSERT_TRUE(r.national_dex);
}

/* An editor Pokemon of that species (seed 7) */
static bool edit_mon(uint16_t national, Gen3Pkm* out) {
    Gen3Editor ed;
    if(!gen3_edit_init(&ed, 7)) return false;
    if(!gen3_edit_set_species(&ed, national)) return false;
    *out = ed.pkm;
    return true;
}

/* The fateful-encounter bit 0, checksum fixed */
static void clear_fateful(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldFateful, 0);
    gen3_pkm_fix_checksum(p);
}

TEST(test_offer_traits) {
    ASSERT_EQ(GEN3_NATIONAL_KANTO_LAST, 151);
    ASSERT_EQ(Gen3OfferNonKanto, 1);
    ASSERT_EQ(Gen3OfferUnfateful, 2);
    ASSERT_EQ(GEN3_OFFER_TRAITS_UNKNOWN, Gen3OfferNonKanto | Gen3OfferUnfateful);
    Gen3Pkm p;

    ASSERT_TRUE(edit_mon(1, &p));
    ASSERT_EQ(gen3_offer_traits(&p), 0);

    /* The editor sets the fateful bit for Mew and Deoxys */
    ASSERT_TRUE(edit_mon(GEN3_NATIONAL_MEW, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldFateful), 1);
    ASSERT_EQ(gen3_offer_traits(&p), 0);
    clear_fateful(&p);
    ASSERT_EQ(gen3_offer_traits(&p), Gen3OfferUnfateful);

    ASSERT_TRUE(edit_mon(152, &p));
    ASSERT_EQ(gen3_offer_traits(&p), Gen3OfferNonKanto);

    /* Treecko: national 252, internal 277 */
    ASSERT_TRUE(edit_mon(252, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldSpecies), 277);
    ASSERT_EQ(gen3_offer_traits(&p), Gen3OfferNonKanto);

    ASSERT_TRUE(edit_mon(GEN3_NATIONAL_DEOXYS, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldFateful), 1);
    ASSERT_EQ(gen3_offer_traits(&p), Gen3OfferNonKanto);
    clear_fateful(&p);
    ASSERT_EQ(gen3_offer_traits(&p), Gen3OfferNonKanto | Gen3OfferUnfateful);

    /* No valid species counts as NonKanto. The checksum isn't read, so none is fixed. */
    ASSERT_TRUE(edit_mon(1, &p));
    gen3_pkm_set(&p, Gen3FieldSpecies, 0);
    ASSERT_EQ(gen3_offer_traits(&p), Gen3OfferNonKanto);
    ASSERT_TRUE(edit_mon(1, &p));
    gen3_pkm_set(&p, Gen3FieldSpecies, 252); /* internal: an old Unown placeholder */
    ASSERT_EQ(gen3_offer_traits(&p), Gen3OfferNonKanto);
}

/* gen3_partner_refuse for every traits value 0..3, in order */
static bool refuses(Gen3LinkGame game, bool natdex, const Gen3Refuse want[4]) {
    const Gen3PartnerRules rules = {.game = game, .national_dex = natdex};
    for(uint8_t traits = 0; traits < 4; traits++) {
        const Gen3Refuse r = gen3_partner_refuse(&rules, traits);
        if(r != want[traits]) {
            printf(
                "  refuse {%d, %d} traits %u: %d, want %d\n",
                game,
                natdex,
                (unsigned)traits,
                r,
                want[traits]);
            return false;
        }
    }
    return true;
}

TEST(test_partner_refuse) {
    /* traits: 0, NonKanto, Unfateful, both */
    static const Gen3Refuse never[4] = {
        Gen3RefuseNone, Gen3RefuseNone, Gen3RefuseNone, Gen3RefuseNone};
    static const Gen3Refuse frlg_natdex[4] = {
        Gen3RefuseNone, Gen3RefuseNone, Gen3RefuseFateful, Gen3RefuseFateful};
    static const Gen3Refuse frlg_no_natdex[4] = {
        Gen3RefuseNone, Gen3RefuseNationalDex, Gen3RefuseFateful, Gen3RefuseNationalDex};
    ASSERT_EQ(Gen3OfferNonKanto | Gen3OfferUnfateful, 3);

    /* Only FRLG refuses */
    for(int natdex = 0; natdex <= 1; natdex++) {
        ASSERT_TRUE(refuses(Gen3LinkGameRS, natdex, never));
        ASSERT_TRUE(refuses(Gen3LinkGameE, natdex, never));
        ASSERT_TRUE(refuses(Gen3LinkGameUnknown, natdex, never));
    }
    ASSERT_TRUE(refuses(Gen3LinkGameFRLG, true, frlg_natdex));
    ASSERT_TRUE(refuses(Gen3LinkGameFRLG, false, frlg_no_natdex));

    /* An offer whose traits are unknown is never usable for FRLG */
    const Gen3PartnerRules frlg = {.game = Gen3LinkGameFRLG, .national_dex = true};
    ASSERT_EQ(gen3_partner_refuse(&frlg, GEN3_OFFER_TRAITS_UNKNOWN), Gen3RefuseFateful);
    const Gen3PartnerRules ruby = {.game = Gen3LinkGameRS, .national_dex = true};
    ASSERT_EQ(gen3_partner_refuse(&ruby, GEN3_OFFER_TRAITS_UNKNOWN), Gen3RefuseNone);
}

TEST(test_tm_make_unfateful) {
    uint8_t game[GEN3_PARTY_SIZE];
    Gen3Pkm p;
    tm_make_unfateful(GEN3_NATIONAL_MEW, 9, game);
    ASSERT_TRUE(gen3_pkm_decrypt(game, GEN3_PARTY_SIZE, &p)); /* the checksum is valid */
    ASSERT_EQ(gen3_national_from_internal(gen3_pkm_get(&p, Gen3FieldSpecies)), GEN3_NATIONAL_MEW);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldLevel), 5);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldFateful), 0);
    ASSERT_EQ(gen3_offer_traits(&p), Gen3OfferUnfateful);
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_link_game);
    RUN(test_partner_rules);
    RUN(test_offer_traits);
    RUN(test_partner_refuse);
    RUN(test_tm_make_unfateful);
    TEST_EXIT();
}
