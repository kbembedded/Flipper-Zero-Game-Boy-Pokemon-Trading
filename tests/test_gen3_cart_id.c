#include "test.h"
#include "bulk_file.h"
#include "fake_gba.h"
#include "fake_helper.h"
#include "fake_save.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/gen3_link_player.h>
#include <src/gen3/gen3_partner.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_rules.h>

/* Cart identification from CART_INFO and SAVE_PROBE as the client returns them; then the same
 * carts end to end against the helper's real core on the fake GBA, which must refuse exactly the
 * writes the Flipper refuses; and the new-Pokemon helper. */

#define MIB 0x100000u
#define MX 0x09C2u
#define SANYO 0x1362u
#define SST 0xD4BFu
#define MX64 0x1CC2u
#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

static Gen3HCartInfo info;
static Gen3HProbe probe;
static Gen3CartId id;

/* A cart as the client reports it: the header with title, code and version, its logo, fixed byte
 * and complement ok; FLASH1M_V and no other library; an MX chip that answered its ID. */
static void cart(const char* title, const char* code, uint8_t version) {
    memset(&info, 0, sizeof(info));
    memcpy(info.header + 0xA0, title, strlen(title));
    memcpy(info.header + 0xAC, code, 4);
    info.header[0xB0] = '0';
    info.header[0xB1] = '1';
    info.header[0xB2] = 0x96;
    info.header[0xBC] = version;
    info.logo_ok = true;
    info.fixed_ok = true;
    info.complement_ok = true;
    info.rom_size = 16 * MIB;
    info.lib = GEN3_HLIB_FLASH1M;
    info.lib_off = 0x3B0000;
    memset(&probe, 0, sizeof(probe));
    probe.flash_id = MX;
    probe.cls = GEN3_HCHIP_FLASH128;
    probe.id0 = 0xC2;
    probe.id1 = 0x09;
}

static void identify(void) {
    memset(&id, 0xA5, sizeof(id));
    gen3_cart_identify(&info, &probe, &id);
}

static bool allows(bool read, bool browse, bool write) {
    return id.can_read_save == read && id.can_browse == browse && id.can_write == write;
}

TEST(test_cart_identify_ruby) {
    cart("POKEMON RUBY", "AXVE", 0);
    identify();
    ASSERT_TRUE(allows(true, true, true));
    ASSERT_EQ(id.why, Gen3CartRefuseNone);
    ASSERT_EQ(id.game, Gen3LinkGameRS);
    ASSERT_EQ(id.met_game, 2);
    ASSERT_STR_EQ(id.title, "POKEMON RUBY");
    ASSERT_STR_EQ(id.code, "AXVE");
    ASSERT_EQ(id.version, 0);
    ASSERT_EQ(id.flash_id, MX);
    /* the Sanyo chip, and a later revision */
    cart("POKEMON RUBY", "AXVE", 2);
    probe.flash_id = SANYO;
    identify();
    ASSERT_TRUE(allows(true, true, true));
    ASSERT_EQ(id.version, 2);
    ASSERT_EQ(id.flash_id, SANYO);
}

TEST(test_cart_identify_codes) {
    cart("POKEMON SAPP", "AXPE", 1);
    identify();
    ASSERT_TRUE(allows(true, true, true));
    ASSERT_EQ(id.game, Gen3LinkGameRS);
    ASSERT_EQ(id.met_game, 1);
    ASSERT_STR_EQ(id.title, "POKEMON SAPP");
    cart("POKEMON FIRE", "BPRE", 1);
    identify();
    ASSERT_TRUE(allows(true, true, true));
    ASSERT_EQ(id.game, Gen3LinkGameFRLG);
    ASSERT_EQ(id.met_game, 4);
    cart("POKEMON LEAF", "BPGE", 0);
    identify();
    ASSERT_TRUE(allows(true, true, true));
    ASSERT_EQ(id.game, Gen3LinkGameFRLG);
    ASSERT_EQ(id.met_game, 5);
    cart("POKEMON EMER", "BPEE", 0);
    identify();
    ASSERT_TRUE(allows(true, true, false));
    ASSERT_EQ(id.why, Gen3CartRefuseEmerald);
    ASSERT_EQ(id.game, Gen3LinkGameE);
    ASSERT_EQ(id.met_game, 3);
}

/* met_game is the game's own version byte, which the link code reads the same way */
TEST(test_cart_identify_game_matches_link_game) {
    static const struct {
        const char* code;
        uint16_t version;
    } games[5] = {
        {"AXVE", GEN3_VERSION_RUBY},
        {"AXPE", GEN3_VERSION_SAPPHIRE},
        {"BPEE", GEN3_VERSION_EMERALD},
        {"BPRE", GEN3_VERSION_FIRERED},
        {"BPGE", GEN3_VERSION_LEAFGREEN},
    };
    for(uint32_t i = 0; i < 5; i++) {
        cart("POKEMON", games[i].code, 0);
        identify();
        ASSERT_EQ(id.met_game, games[i].version & 0xFF);
        ASSERT_EQ(gen3_link_game(GEN3_LP_VERSION_BASE | id.met_game), id.game);
    }
}

TEST(test_cart_identify_regions) {
    cart("POKEMON RUBY", "AXVJ", 0);
    identify();
    ASSERT_TRUE(allows(true, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseJapanese);
    cart("POKEMON FIRE", "BPRJ", 0);
    identify();
    ASSERT_TRUE(allows(true, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseJapanese);
    static const char* const others[] = {"AXVD", "AXVF", "AXVI", "AXVS", "BPGD", "AXPS"};
    for(uint32_t i = 0; i < sizeof(others) / sizeof(others[0]); i++) {
        cart("POKEMON", others[i], 0);
        identify();
        ASSERT_TRUE(allows(true, true, false));
        ASSERT_EQ(id.why, Gen3CartRefuseRegion);
    }
    /* Emerald from elsewhere: the region comes first */
    cart("POKEMON EMER", "BPED", 0);
    identify();
    ASSERT_TRUE(allows(true, true, false));
    ASSERT_EQ(id.why, Gen3CartRefuseRegion);
    cart("POKEMON EMER", "BPEJ", 0);
    identify();
    ASSERT_TRUE(allows(true, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseJapanese);
}

TEST(test_cart_identify_refusals) {
    /* the header */
    cart("POKEMON RUBY", "AXVE", 0);
    info.logo_ok = false;
    identify();
    ASSERT_TRUE(allows(false, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseHeader);
    cart("POKEMON RUBY", "AXVE", 0);
    info.fixed_ok = false;
    identify();
    ASSERT_TRUE(allows(false, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseHeader);
    cart("POKEMON RUBY", "AXVE", 0);
    info.complement_ok = false;
    identify();
    ASSERT_TRUE(allows(false, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseHeader);
    /* not one of the five games */
    static const char* const codes[] = {"AGBE", "AXXE", "BPXE", "axve", "XAVE", "BPZE"};
    for(uint32_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        cart("GAME", codes[i], 0);
        identify();
        ASSERT_TRUE(allows(false, false, false));
        ASSERT_EQ(id.why, Gen3CartRefuseNotPokemon);
        ASSERT_EQ(id.game, Gen3LinkGameUnknown);
        ASSERT_EQ(id.met_game, 0);
    }
    /* the save chip */
    cart("POKEMON RUBY", "AXVE", 0);
    probe.sram = true;
    probe.cls = GEN3_HCHIP_SRAM;
    probe.flash_id = 0;
    identify();
    ASSERT_TRUE(allows(false, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseSram);
    /* either the flag or the class says SRAM */
    cart("POKEMON RUBY", "AXVE", 0);
    probe.cls = GEN3_HCHIP_SRAM;
    identify();
    ASSERT_TRUE(allows(false, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseSram);
    cart("POKEMON RUBY", "AXVE", 0);
    probe.sram = true;
    identify();
    ASSERT_TRUE(allows(false, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseSram);
    static const uint8_t no_read[2] = {GEN3_HCHIP_FLASH64, GEN3_HCHIP_NONE};
    for(uint32_t i = 0; i < 2; i++) {
        cart("POKEMON RUBY", "AXVE", 0);
        probe.cls = no_read[i];
        probe.flash_id = i == 0 ? MX64 : 0;
        identify();
        ASSERT_TRUE(allows(false, false, false));
        ASSERT_EQ(id.why, Gen3CartRefuseChip);
    }
    /* a 128 KB flash the helper reads, but not one of the two chips the games accept */
    cart("POKEMON RUBY", "AXVE", 0);
    probe.flash_id = SST;
    identify();
    ASSERT_TRUE(allows(true, true, false));
    ASSERT_EQ(id.why, Gen3CartRefuseChip);
    /* German Sapphire version 0 carries an MX29L010 that answers 0xD4BF: its chip passes, and
     * its region refuses the write */
    cart("POKEMON SAPP", "AXPD", 0);
    probe.flash_id = SST;
    identify();
    ASSERT_TRUE(allows(true, true, false));
    ASSERT_EQ(id.why, Gen3CartRefuseRegion);
    static const struct {
        const char* code;
        uint8_t version;
    } not_de0[] = {{"AXPD", 1}, {"AXPE", 0}, {"AXVD", 0}, {"BPGD", 0}};
    for(uint32_t i = 0; i < sizeof(not_de0) / sizeof(not_de0[0]); i++) {
        cart("POKEMON", not_de0[i].code, not_de0[i].version);
        probe.flash_id = SST;
        identify();
        ASSERT_TRUE(allows(true, true, false));
        ASSERT_EQ(id.why, Gen3CartRefuseChip);
    }
    /* the ROM's save library */
    cart("POKEMON RUBY", "AXVE", 0);
    info.lib = GEN3_HLIB_EEPROM;
    info.other_lib = true;
    identify();
    ASSERT_TRUE(allows(true, true, false));
    ASSERT_EQ(id.why, Gen3CartRefuseLibrary);
    cart("POKEMON RUBY", "AXVE", 0);
    info.other_lib = true;
    identify();
    ASSERT_TRUE(allows(true, true, false));
    ASSERT_EQ(id.why, Gen3CartRefuseLibrary);
    cart("POKEMON RUBY", "AXVE", 0);
    info.lib = GEN3_HLIB_NONE;
    identify();
    ASSERT_TRUE(allows(true, true, false));
    ASSERT_EQ(id.why, Gen3CartRefuseLibrary);
    /* the first failed check names the reason */
    cart("POKEMON RUBY", "AXVJ", 0);
    probe.sram = true;
    probe.cls = GEN3_HCHIP_SRAM;
    identify();
    ASSERT_TRUE(allows(false, false, false));
    ASSERT_EQ(id.why, Gen3CartRefuseJapanese);
    cart("POKEMON RUBY", "AGBJ", 0);
    info.complement_ok = false;
    identify();
    ASSERT_EQ(id.why, Gen3CartRefuseHeader);
    cart("POKEMON RUBY", "AXVD", 0);
    info.lib = GEN3_HLIB_SRAM;
    identify();
    ASSERT_TRUE(allows(true, true, false));
    ASSERT_EQ(id.why, Gen3CartRefuseLibrary);
}

/* The code and title as a screen and a file name can carry them */
TEST(test_cart_identify_text_fields) {
    cart("POKEMON RUBY", "AXVE", 0);
    info.header[0xAF] = '/';
    info.header[0xA3] = 0x01;
    identify();
    ASSERT_STR_EQ(id.code, "AXV_");
    ASSERT_STR_EQ(id.title, "POK?MON RUBY");
    ASSERT_EQ(id.why, Gen3CartRefuseRegion);
    cart("SHORT", "AXVE", 0);
    identify();
    ASSERT_STR_EQ(id.title, "SHORT");
    cart("POKEMON RUBY", "AXVE", 0);
    memset(info.header + 0xA0, 0xFF, 12);
    identify();
    ASSERT_STR_EQ(id.title, "????????????");
}

TEST(test_cart_refuse_text) {
    static const char* const want[] = {
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
    for(int r = Gen3CartRefuseNone; r <= Gen3CartRefuseSave; r++) {
        const char* t = gen3_cart_refuse_text((Gen3CartRefuse)r);
        ASSERT_TRUE(t != NULL);
        ASSERT_STR_EQ(t, want[r]);
        ASSERT_TRUE(strlen(t) > 0);
        ASSERT_TRUE(strlen(t) <= 21);
        ASSERT_TRUE(strchr(t, '\n') == NULL);
    }
    ASSERT_TRUE(strstr(gen3_cart_refuse_text(Gen3CartRefuseEmerald), "Emerald") != NULL);
    ASSERT_STR_EQ(gen3_cart_refuse_text((Gen3CartRefuse)(Gen3CartRefuseSave + 1)), "");
}

/* ---- The same carts against the helper ---- */

static FakeGba gba;
static FakeHelper fh;
static Gen3Hlink pump;
static Gen3Hcli cli;
static FakeSaveSpec spec;
static uint8_t hdr[0xC0];
static Gen3HFooter footers[32];

typedef struct {
    const char* code;
    uint8_t version;
    FakeSaveKind save;
    uint16_t chip;
    bool flash1m; /* FLASH1M_V103 in the ROM */
    const char* extra; /* another library string, or NULL */
    bool bad_complement;
    FakeGame layout;
} HelperCart;

/* Inserts the cart with a save in its layout (slot A, counter 4) and runs what the Flipper runs:
 * connect, CART_INFO, SAVE_PROBE, then gen3_cart_identify. */
static bool helper_identify(const HelperCart* c) {
    fake_gba_init(&gba);
    helper_cmd_header(hdr, gba.own, "POKEMON", c->code, c->version);
    if(c->bad_complement) hdr[0xBD] ^= 1;
    fake_gba_rom(&gba, hdr, 16 * MIB);
    if(c->flash1m) fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    if(c->extra) fake_gba_rom_string(&gba, 0x500000, c->extra);
    fake_gba_save(&gba, c->save, c->chip);
    spec = (FakeSaveSpec){
        .game = c->layout,
        .slot_present = {true, false},
        .counter = {4, 0},
        .first = {0, 0},
        .seed = 0x5EED0108u,
    };
    fake_save_build(fake_gba_flash(&gba), &spec);
    fake_helper_init(&fh, &gba);
    const Gen3HTransport t = fake_helper_transport(&fh, &pump);
    gen3_hcli_init(&cli, &t);
    if(gen3_hcli_connect(&cli, NULL) != Gen3HcliOk) return false;
    if(gen3_hcli_cart_info(&cli, &info) != Gen3HcliOk) return false;
    if(gen3_hcli_save_probe(&cli, &probe) != Gen3HcliOk) return false;
    gen3_cart_identify(&info, &probe, &id);
    return true;
}

/* can_read_save: the helper reads the save. can_write: the helper arms a write of slot B over the
 * slot the game loads, and only then. */
TEST(test_cart_identify_agrees_with_helper) {
    static const HelperCart carts[] = {
        {"AXVE", 0, FakeSaveFlash, MX, true, NULL, false, FakeGameRS},
        {"AXPE", 2, FakeSaveFlash, SANYO, true, NULL, false, FakeGameRS},
        {"BPRE", 1, FakeSaveFlash, MX, true, NULL, false, FakeGameFRLG},
        {"BPGE", 0, FakeSaveFlash, SANYO, true, NULL, false, FakeGameFRLG},
        {"BPEE", 0, FakeSaveFlash, MX, true, NULL, false, FakeGameE},
        {"AXVJ", 0, FakeSaveFlash, MX, true, NULL, false, FakeGameRS},
        {"AXVD", 0, FakeSaveFlash, MX, true, NULL, false, FakeGameRS},
        {"BPRF", 0, FakeSaveFlash, MX, true, NULL, false, FakeGameFRLG},
        {"AXPD", 0, FakeSaveFlash, SST, true, NULL, false, FakeGameRS},
        {"AXVE", 0, FakeSaveFlash, SST, true, NULL, false, FakeGameRS},
        {"AXVE", 0, FakeSaveFlash, MX64, true, NULL, false, FakeGameRS},
        {"AXVE", 0, FakeSaveSram, 0, true, NULL, false, FakeGameRS},
        {"AXVE", 0, FakeSaveNone, 0, true, NULL, false, FakeGameRS},
        {"AXVE", 0, FakeSaveRomLike, 0, true, NULL, false, FakeGameRS},
        {"AXVE", 0, FakeSaveFlash, MX, true, "SRAM_V113", false, FakeGameRS},
        {"AXVE", 0, FakeSaveFlash, MX, true, "EEPROM_V124", false, FakeGameRS},
        {"AXVE", 0, FakeSaveFlash, MX, false, "SRAM_F_V103", false, FakeGameRS},
        {"AXVE", 0, FakeSaveFlash, MX, false, NULL, false, FakeGameRS},
        {"AXVE", 0, FakeSaveFlash, MX, true, NULL, true, FakeGameRS},
        {"AGBE", 0, FakeSaveFlash, MX, true, NULL, false, FakeGameRS},
    };
    uint32_t writable = 0;
    for(size_t i = 0; i < sizeof(carts) / sizeof(carts[0]); i++) {
        ASSERT_TRUE(helper_identify(&carts[i]));
        const Gen3HcliResult read = gen3_hcli_read_footers(&cli, footers);
        if(id.can_read_save) ASSERT_EQ(read, Gen3HcliOk);
        if(read != Gen3HcliOk) ASSERT_FALSE(id.can_read_save);
        const Gen3HcliResult arm = gen3_hcli_arm_write(&cli, probe.flash_id, 1, 0, 4);
        ASSERT_EQ(arm == Gen3HcliOk, id.can_write);
        if(arm != Gen3HcliOk) ASSERT_EQ(cli.last_status, GEN3_HST_REFUSED);
        ASSERT_EQ(id.why == Gen3CartRefuseNone, id.can_write);
        if(id.can_write) writable++;
    }
    ASSERT_EQ(writable, 4);
}

/* ---- A new Pokemon for the save ---- */

TEST(test_cart_trainer_ot) {
    const Gen3SaveTrainer t = {
        .name = "MAY",
        .name_raw = {0xC7, 0xBB, 0xD3, 0xFF, 0xFF, 0x00, 0x01, 0xFF},
        .gender = 1,
        .tid = 1234,
        .sid = 4321,
    };
    Gen3Trainer o;
    gen3_cart_trainer_ot(&t, &o);
    ASSERT_MEM_EQ(o.name, t.name_raw, GEN3_OT_NAME_LEN);
    ASSERT_EQ(o.tid, 1234);
    ASSERT_EQ(o.sid, 4321);
    ASSERT_EQ(o.gender, 1);
    Gen3SaveTrainer u = t;
    u.gender = 0;
    gen3_cart_trainer_ot(&u, &o);
    ASSERT_EQ(o.gender, 0);
    u.gender = 0x80;
    gen3_cart_trainer_ot(&u, &o);
    ASSERT_EQ(o.gender, 1);
}

TEST(test_cart_new_mon) {
    const Gen3SaveTrainer t = {
        .name = "MAY",
        .name_raw = {0xC7, 0xBB, 0xD3, 0xFF, 0xFF, 0x00, 0x01, 0xFF},
        .gender = 1,
        .tid = 1234,
        .sid = 4321,
    };
    static Gen3Editor ed;
    ASSERT_TRUE(gen3_cart_new_mon(&ed, &t, 4, 0x12345678u));
    ASSERT_MEM_EQ(ed.pkm.raw + 0x14, t.name_raw, 7);
    ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldOtId), 0x10E104D2u);
    ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldOtGender), 1);
    ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldMetGame), 4);
    ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldChecksum), gen3_pkm_checksum(&ed.pkm));
    ASSERT_EQ(gen3_pkm_check(&ed.pkm), Gen3CheckOk);
    ASSERT_FALSE(
        gen3_is_shiny(gen3_pkm_get(&ed.pkm, Gen3FieldOtId), gen3_pkm_get(&ed.pkm, Gen3FieldPid)));
    ASSERT_EQ(gen3_edit_national(&ed), GEN3_EDIT_DEFAULT_NATIONAL);
    /* the editor has the save's trainer, as the OT of this Pokemon */
    ASSERT_TRUE(gen3_edit_trainer(&ed) != NULL);
    ASSERT_TRUE(gen3_edit_ot_is_trainer(&ed));
    /* FireRed's met game, then Ruby's */
    ASSERT_TRUE(gen3_cart_new_mon(&ed, &t, 2, 7));
    ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldMetGame), 2);
    /* without the bulk file: false */
    bulk_file_failing = true;
    ASSERT_FALSE(gen3_cart_new_mon(&ed, &t, 4, 0x12345678u));
    ASSERT_TRUE(bulk_file_open());
    ASSERT_TRUE(gen3_cart_new_mon(&ed, &t, 4, 0x12345678u));
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_cart_identify_ruby);
    RUN(test_cart_identify_codes);
    RUN(test_cart_identify_game_matches_link_game);
    RUN(test_cart_identify_regions);
    RUN(test_cart_identify_refusals);
    RUN(test_cart_identify_text_fields);
    RUN(test_cart_refuse_text);
    RUN(test_cart_identify_agrees_with_helper);
    RUN(test_cart_trainer_ot);
    RUN(test_cart_new_mon);
    TEST_EXIT();
}
