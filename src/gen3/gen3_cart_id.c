#include <string.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_link_player.h>

/* The cart header (multiboot-and-save.md §3.2) */
#define CART_TITLE 0xA0u
#define CART_TITLE_LEN 12u
#define CART_CODE 0xACu
#define CART_REGION 0xAFu /* the code's last letter */
#define CART_VERSION 0xBCu

/* The five Gen III Pokemon games, by the first three letters of their code (pokeruby: config.mk;
 * fr: config.mk; em: Makefile) */
static const struct {
    char code[3];
    Gen3LinkGame game;
    uint16_t version;
} cart_games[] = {
    {{'A', 'X', 'V'}, Gen3LinkGameRS, GEN3_VERSION_RUBY},
    {{'A', 'X', 'P'}, Gen3LinkGameRS, GEN3_VERSION_SAPPHIRE},
    {{'B', 'P', 'R'}, Gen3LinkGameFRLG, GEN3_VERSION_FIRERED},
    {{'B', 'P', 'G'}, Gen3LinkGameFRLG, GEN3_VERSION_LEAFGREEN},
    {{'B', 'P', 'E'}, Gen3LinkGameE, GEN3_VERSION_EMERALD},
};

/* German Sapphire version 0's MX29L010 entry carries this ID (pokeruby: src/libs/agb_flash_mx.c:
 * MX29L010, for GERMAN, SAPPHIRE and REVISION 0); on any other cart it is SST's 64 KB part. */
#define CART_ID_SAPPHIRE_DE0 0xD4BFu

/* One of the chips the cart's own game accepts */
static bool cart_chip_known(const uint8_t* h, uint16_t flash_id) {
    if(flash_id == GEN3_FLASH_ID_MX || flash_id == GEN3_FLASH_ID_SANYO) return true;
    return flash_id == CART_ID_SAPPHIRE_DE0 && memcmp(h + CART_CODE, "AXPD", 4) == 0 &&
           h[CART_VERSION] == 0;
}

static char cart_code_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ? (char)c : '_';
}

static char cart_title_char(uint8_t c) {
    return c >= 0x20 && c <= 0x7E ? (char)c : '?';
}

void gen3_cart_identify(const Gen3HCartInfo* info, const Gen3HProbe* probe, Gen3CartId* out) {
    memset(out, 0, sizeof(*out));
    const uint8_t* h = info->header;
    for(uint32_t i = 0; i < 4; i++) out->code[i] = cart_code_char(h[CART_CODE + i]);
    for(uint32_t i = 0; i < CART_TITLE_LEN && h[CART_TITLE + i]; i++) {
        out->title[i] = cart_title_char(h[CART_TITLE + i]);
    }
    out->version = h[CART_VERSION];
    out->flash_id = probe->flash_id;
    out->game = Gen3LinkGameUnknown;
    for(uint32_t g = 0; g < sizeof(cart_games) / sizeof(cart_games[0]); g++) {
        if(memcmp(h + CART_CODE, cart_games[g].code, 3) == 0) {
            out->game = cart_games[g].game;
            out->met_game = (uint8_t)(cart_games[g].version & 0xFF);
        }
    }
    const uint8_t region = h[CART_REGION];

    /* Each check in Gen3CartRefuse's order; the first that fails is why */
    Gen3CartRefuse why = Gen3CartRefuseNone;
    if(!info->logo_ok || !info->fixed_ok || !info->complement_ok) {
        why = Gen3CartRefuseHeader;
    } else if(out->game == Gen3LinkGameUnknown) {
        why = Gen3CartRefuseNotPokemon;
    } else if(region == 'J') {
        /* The Japanese layouts haven't been checked against pret */
        why = Gen3CartRefuseJapanese;
    } else if(probe->sram || probe->cls == GEN3_HCHIP_SRAM) {
        why = Gen3CartRefuseSram;
    } else if(probe->cls != GEN3_HCHIP_FLASH128 || !cart_chip_known(h, probe->flash_id)) {
        why = Gen3CartRefuseChip;
    } else if(info->lib != GEN3_HLIB_FLASH1M || info->other_lib) {
        why = Gen3CartRefuseLibrary;
    } else if(region != 'E') {
        why = Gen3CartRefuseRegion;
    } else if(out->game == Gen3LinkGameE) {
        why = Gen3CartRefuseEmerald;
    }
    out->why = why;

    /* The helper reads the save of a 128 KB flash that isn't SRAM (multiboot-and-save.md §3.6) */
    out->can_read_save = info->logo_ok && info->fixed_ok && info->complement_ok &&
                         out->game != Gen3LinkGameUnknown && !probe->sram &&
                         probe->cls == GEN3_HCHIP_FLASH128;
    out->can_browse = out->can_read_save && region != 'J';
    out->can_write = why == Gen3CartRefuseNone;
}

const char* gen3_cart_refuse_text(Gen3CartRefuse r) {
    /* One line each on the 128-pixel screen */
    static const char* const texts[] = {
        [Gen3CartRefuseNone] = "Writing allowed",
        [Gen3CartRefuseHeader] = "Bad cartridge header",
        [Gen3CartRefuseNotPokemon] = "Not a Gen III Pokemon",
        [Gen3CartRefuseJapanese] = "Japanese: backup only",
        [Gen3CartRefuseSram] = "SRAM save (repro)",
        [Gen3CartRefuseChip] = "Unknown save chip",
        [Gen3CartRefuseLibrary] = "ROM patched save type",
        [Gen3CartRefuseRegion] = "English carts only",
        [Gen3CartRefuseEmerald] = "Emerald: read only",
        [Gen3CartRefuseSave] = "Save not recognised",
    };
    return (unsigned)r < sizeof(texts) / sizeof(texts[0]) ? texts[r] : "";
}

void gen3_cart_trainer_ot(const Gen3SaveTrainer* t, Gen3Trainer* out) {
    /* The game copies 7 bytes of the player's name raw into a new Pokemon's OT name (pokeruby:
     * src/pokemon_1.c: CreateBoxMon) */
    memcpy(out->name, t->name_raw, GEN3_OT_NAME_LEN);
    out->tid = t->tid;
    out->sid = t->sid;
    out->gender = t->gender ? 1 : 0;
}

bool gen3_cart_new_mon(Gen3Editor* ed, const Gen3SaveTrainer* t, uint8_t met_game, uint32_t seed) {
    if(!gen3_edit_init(ed, seed)) return false;
    Gen3Trainer trainer;
    gen3_cart_trainer_ot(t, &trainer);
    gen3_edit_set_trainer(ed, &trainer);
    gen3_pkm_set(&ed->pkm, Gen3FieldMetGame, met_game);
    gen3_pkm_fix_checksum(&ed->pkm);
    return true;
}
