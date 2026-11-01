#include <src/gen3/gen3_partner.h>

#include <src/gen3/gen3_data.h>

Gen3LinkGame gen3_link_game(uint16_t version) {
    switch(version & 0xFF) {
    case GEN3_VERSION_SAPPHIRE & 0xFF:
    case GEN3_VERSION_RUBY & 0xFF:
        return Gen3LinkGameRS;
    case GEN3_VERSION_EMERALD & 0xFF:
        return Gen3LinkGameE;
    case GEN3_VERSION_FIRERED & 0xFF:
    case GEN3_VERSION_LEAFGREEN & 0xFF:
        return Gen3LinkGameFRLG;
    default:
        return Gen3LinkGameUnknown;
    }
}

void gen3_partner_rules(const Gen3LinkPlayer* gba, Gen3PartnerRules* out) {
    out->game = gba ? gen3_link_game(gba->version) : Gen3LinkGameUnknown;
    out->national_dex = true;
    switch(out->game) {
    case Gen3LinkGameFRLG:
        /* FRLG tests its partner's progressFlagsCopy, a receiver's copy of block 0x20
         * (fr: link.c: ConvertLinkPlayerName), so the Flipper reads block 0x20 itself */
        out->national_dex =
            (gen3_link_player_progress(gba) & GEN3_FRLG_PROGRESS_NATIONAL_DEX_MASK) != 0;
        break;
    case Gen3LinkGameRS:
    case Gen3LinkGameE:
    case Gen3LinkGameUnknown:
        break;
    }
}

uint8_t gen3_offer_traits(const Gen3Pkm* p) {
    const uint16_t national =
        gen3_national_from_internal((uint16_t)gen3_pkm_get(p, Gen3FieldSpecies));
    uint8_t traits = 0;
    if(national == 0 || national > GEN3_NATIONAL_KANTO_LAST) traits |= Gen3OfferNonKanto;
    if((national == GEN3_NATIONAL_MEW || national == GEN3_NATIONAL_DEOXYS) &&
       gen3_pkm_get(p, Gen3FieldFateful) == 0) {
        traits |= Gen3OfferUnfateful;
    }
    return traits;
}

Gen3Refuse gen3_partner_refuse(const Gen3PartnerRules* rules, uint8_t traits) {
    switch(rules->game) {
    case Gen3LinkGameFRLG:
        if((traits & Gen3OfferNonKanto) && !rules->national_dex) return Gen3RefuseNationalDex;
        if(traits & Gen3OfferUnfateful) return Gen3RefuseFateful;
        return Gen3RefuseNone;
    case Gen3LinkGameRS:
    case Gen3LinkGameE:
    case Gen3LinkGameUnknown:
        return Gen3RefuseNone;
    }
    return Gen3RefuseNone;
}
