#include "test.h"
#include "fake_gba.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <hp_cart.h>
#include <hp_core.h>
#include <hp_flash.h>

/* The helper's cart identification on the fake GBA (multiboot-and-save.md §3.2, §3.3 step 1,
 * §3.7), and the CART_INFO and SAVE_PROBE commands (multiboot-and-save.md §6.3). */

#define MIB 0x100000u

static FakeGba gba;
static Hp hp;
static uint8_t hdr[0xC0];

static void make_header(const char* title, const char* code, uint8_t version) {
    helper_cmd_header(hdr, gba.own, title, code, version);
}

/* Ruby: 16 MiB, FLASH1M_V103 at 0x3B0000, an MX chip. */
static void setup_ruby(void) {
    fake_gba_init(&gba);
    make_header("POKEMON RUBY", "AXVE", 0);
    fake_gba_rom(&gba, hdr, 16 * MIB);
    fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    fake_gba_save(&gba, FakeSaveFlash, 0x09C2);
    hp_init(&hp);
}

static HpCart scan(void) {
    HpCart c;
    memset(&c, 0xA5, sizeof(c));
    hp_cart_scan(&c);
    return c;
}

TEST(test_hp_cart_ruby) {
    setup_ruby();
    const HpCart c = scan();
    ASSERT_TRUE(c.logo_ok);
    ASSERT_TRUE(c.fixed_ok);
    ASSERT_TRUE(c.complement_ok);
    ASSERT_EQ(c.rom_size, 0x1000000);
    ASSERT_EQ(c.lib, HpLibFlash1M);
    ASSERT_EQ(c.lib_off, 0x3B0000);
    ASSERT_TRUE(!c.other_lib);
    ASSERT_TRUE(c.write_game);
    ASSERT_MEM_EQ(c.code, "AXVE", 4);
}

/* A ROM patched to save to SRAM still holds FLASH1M_V, wherever the other string is. */
TEST(test_hp_cart_patched) {
    setup_ruby();
    fake_gba_rom_string(&gba, 0x500000, "SRAM_V113");
    HpCart c = scan();
    ASSERT_EQ(c.lib, HpLibFlash1M);
    ASSERT_EQ(c.lib_off, 0x3B0000);
    ASSERT_TRUE(c.other_lib);
    ASSERT_TRUE(c.write_game);
    setup_ruby();
    fake_gba_rom_string(&gba, 0x100000, "SRAM_F_V100");
    c = scan();
    ASSERT_EQ(c.lib, HpLibFlash1M);
    ASSERT_EQ(c.lib_off, 0x3B0000);
    ASSERT_TRUE(c.other_lib);
}

TEST(test_hp_cart_eeprom_only) {
    fake_gba_init(&gba);
    make_header("POKEMON RUBY", "AXVE", 0);
    fake_gba_rom(&gba, hdr, 16 * MIB);
    fake_gba_rom_string(&gba, 0x7FF0, "EEPROM_V124");
    const HpCart c = scan();
    ASSERT_EQ(c.lib, HpLibEeprom);
    ASSERT_EQ(c.lib_off, 0x7FF0);
    ASSERT_TRUE(c.other_lib);
}

/* Each library string; the first one found when there is no FLASH1M_V; strings only at 32-bit
 * steps, whole, and inside the ROM. */
TEST(test_hp_cart_lib_kinds) {
    static const struct {
        const char* s;
        HpLib lib;
    } kinds[] = {
        {"FLASH_V126", HpLibFlash},
        {"FLASH512_V131", HpLibFlash512},
        {"SRAM_V110", HpLibSram},
        {"SRAM_F_V102", HpLibSram},
        {"EEPROM_V122", HpLibEeprom},
        {"FLASH1M_V102", HpLibFlash1M},
    };
    for(size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        fake_gba_init(&gba);
        make_header("TEST", "AXVE", 0);
        fake_gba_rom(&gba, hdr, 1 * MIB);
        fake_gba_rom_string(&gba, 0x1234, kinds[i].s);
        const HpCart c = scan();
        ASSERT_EQ(c.lib, kinds[i].lib);
        ASSERT_EQ(c.lib_off, 0x1234);
        ASSERT_EQ(c.other_lib, kinds[i].lib != HpLibFlash1M);
    }
    fake_gba_init(&gba);
    make_header("TEST", "AXVE", 0);
    fake_gba_rom(&gba, hdr, 1 * MIB);
    fake_gba_rom_string(&gba, 0x2000, "SRAM_V113");
    fake_gba_rom_string(&gba, 0x3000, "FLASH_V126");
    HpCart c = scan();
    ASSERT_EQ(c.lib, HpLibSram);
    ASSERT_EQ(c.lib_off, 0x2000);
    ASSERT_TRUE(c.other_lib);
    /* the first FLASH1M_V, whatever comes after it */
    fake_gba_rom_string(&gba, 0x4000, "FLASH1M_V103");
    fake_gba_rom_string(&gba, 0x5000, "EEPROM_V124");
    fake_gba_rom_string(&gba, 0x6000, "FLASH1M_V102");
    c = scan();
    ASSERT_EQ(c.lib, HpLibFlash1M);
    ASSERT_EQ(c.lib_off, 0x4000);
    ASSERT_TRUE(c.other_lib);
    /* not at a 32-bit step, not whole, or past the end */
    fake_gba_init(&gba);
    make_header("TEST", "AXVE", 0);
    fake_gba_rom(&gba, hdr, 1 * MIB);
    fake_gba_rom_string(&gba, 0x1002, "FLASH1M_V103");
    fake_gba_rom_string(&gba, 0x2000, "FLASH1M_X103");
    fake_gba_rom_string(&gba, 0x3000, "SRAM_X");
    fake_gba_rom_string(&gba, 0x4000, "EEPROM_X");
    fake_gba_rom_string(&gba, MIB - 4, "FLASH1M_V103");
    c = scan();
    ASSERT_EQ(c.lib, HpLibNone);
    ASSERT_EQ(c.lib_off, 0);
    ASSERT_TRUE(!c.other_lib);
    /* the last whole string in the ROM is found */
    fake_gba_init(&gba);
    fake_gba_rom(&gba, hdr, 1 * MIB);
    fake_gba_rom_string(&gba, MIB - 12, "FLASH1M_V103");
    c = scan();
    ASSERT_EQ(c.lib, HpLibFlash1M);
    ASSERT_EQ(c.lib_off, MIB - 12);
}

TEST(test_hp_cart_size_8m) {
    fake_gba_init(&gba);
    make_header("POKEMON RUBY", "AXVE", 0);
    fake_gba_rom(&gba, hdr, 8 * MIB);
    fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    const HpCart c = scan();
    ASSERT_EQ(c.rom_size, 0x800000);
    ASSERT_EQ(c.lib, HpLibFlash1M);
}

/* 1 MiB is the smallest; with no end below 32 MiB, 32 MiB; a ROM that mirrors ends where the
 * mirror starts. */
TEST(test_hp_cart_sizes) {
    static const struct {
        uint32_t size;
        bool mirror;
        uint32_t want;
    } cases[] = {
        {1 * MIB, false, 1 * MIB},
        {2 * MIB, false, 2 * MIB},
        {4 * MIB, true, 4 * MIB},
        {1 * MIB, true, 1 * MIB},
        {32 * MIB, false, 32 * MIB},
    };
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fake_gba_init(&gba);
        make_header("TEST", "AXVE", 0);
        fake_gba_rom(&gba, hdr, cases[i].size);
        gba.rom_mirror = cases[i].mirror;
        ASSERT_EQ(scan().rom_size, cases[i].want);
    }
}

/* A string that starts in the last word of a 32 MiB ROM is cut off by its end: never matched, and
 * nothing past the ROM window is read (the fake would stop the test). */
TEST(test_hp_cart_rom_window_end) {
    fake_gba_init(&gba);
    make_header("TEST", "AXVE", 0);
    fake_gba_rom(&gba, hdr, 32 * MIB);
    fake_gba_rom_string(&gba, 32 * MIB - 4, "FLASH1M_V103");
    const HpCart c = scan();
    ASSERT_EQ(c.rom_size, 32 * MIB);
    ASSERT_EQ(c.lib, HpLibNone);
    ASSERT_TRUE(!c.other_lib);
}

TEST(test_hp_cart_bad_logo) {
    setup_ruby();
    hdr[0x10] ^= 0xFF;
    fake_gba_rom(&gba, hdr, 16 * MIB);
    fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    HpCart c = scan();
    ASSERT_TRUE(!c.logo_ok);
    ASSERT_EQ(c.rom_size, 0x1000000);
    ASSERT_EQ(c.lib, HpLibFlash1M);
    ASSERT_TRUE(c.fixed_ok);
    ASSERT_TRUE(c.complement_ok);
    /* the last logo byte */
    setup_ruby();
    hdr[0x9F] ^= 0x01;
    fake_gba_rom(&gba, hdr, 16 * MIB);
    ASSERT_TRUE(!scan().logo_ok);
    /* without the logo's first word there is no ROM size, so no scan */
    setup_ruby();
    hdr[0x04] ^= 0x01;
    fake_gba_rom(&gba, hdr, 16 * MIB);
    fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    c = scan();
    ASSERT_TRUE(!c.logo_ok);
    ASSERT_EQ(c.rom_size, 0);
    ASSERT_EQ(c.lib, HpLibNone);
    ASSERT_EQ(c.lib_off, 0);
}

TEST(test_hp_cart_header_checks) {
    setup_ruby();
    hdr[0xB2] = 0x00;
    fake_gba_rom(&gba, hdr, 16 * MIB);
    HpCart c = scan();
    ASSERT_TRUE(!c.fixed_ok);
    ASSERT_TRUE(!c.complement_ok); /* it covers 0xB2 too */
    ASSERT_TRUE(c.logo_ok);
    setup_ruby();
    hdr[0xBD] ^= 0x01;
    fake_gba_rom(&gba, hdr, 16 * MIB);
    c = scan();
    ASSERT_TRUE(c.fixed_ok);
    ASSERT_TRUE(!c.complement_ok);
}

/* No cart: open bus everywhere. */
TEST(test_hp_cart_none) {
    fake_gba_init(&gba);
    const HpCart c = scan();
    ASSERT_TRUE(!c.logo_ok);
    ASSERT_TRUE(!c.fixed_ok);
    ASSERT_TRUE(!c.complement_ok);
    ASSERT_EQ(c.rom_size, 0);
    ASSERT_EQ(c.lib, HpLibNone);
    ASSERT_TRUE(!c.other_lib);
    ASSERT_TRUE(!c.write_game);
}

TEST(test_hp_cart_codes) {
    static const struct {
        const char* code;
        bool write;
    } codes[] = {
        {"AXVE", true},
        {"AXPE", true},
        {"BPRE", true},
        {"BPGE", true},
        {"BPEE", false},
        {"AXVJ", false},
        {"AXVD", false},
        {"AXPD", false},
        {"BPRF", false},
        {"ZFGE", false},
    };
    for(size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        fake_gba_init(&gba);
        make_header("POKEMON", codes[i].code, 0);
        fake_gba_rom(&gba, hdr, 1 * MIB);
        ASSERT_EQ(scan().write_game, codes[i].write);
    }
}

/* A command's response: seq << 8 | status, n, the payload (from resp + 2), the CRC */
static uint16_t resp[HELPER_CMD_RESP_MAX];
static const uint16_t* const p = resp + 2;

TEST(test_hp_cart_info_cmd) {
    setup_ruby();
    gba.trace_len = 0;
    ASSERT_EQ(helper_cmd_run(&hp, 1, HP_CMD_CART_INFO, NULL, 0, resp), HP_ST_OK);
    ASSERT_EQ(resp[1], 102);
    for(uint32_t k = 0; k < 96; k++) ASSERT_EQ(p[k], hdr[2 * k] | hdr[2 * k + 1] << 8);
    ASSERT_EQ(p[96], 0x17); /* logo, fixed, complement, write_game */
    ASSERT_EQ(p[97], 0x0000);
    ASSERT_EQ(p[98], 0x0100);
    ASSERT_EQ(p[99], HpLibFlash1M);
    ASSERT_EQ(p[100], 0x0000);
    ASSERT_EQ(p[101], 0x003B);
    /* the save chip is never touched */
    ASSERT_EQ(gba.trace_len, 0);
    /* each CART_INFO scans again: a patched, German cart */
    make_header("POKEMON RUBY", "AXVD", 0);
    fake_gba_rom(&gba, hdr, 16 * MIB);
    fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    fake_gba_rom_string(&gba, 0x500000, "SRAM_V113");
    ASSERT_EQ(helper_cmd_run(&hp, 2, HP_CMD_CART_INFO, NULL, 0, resp), HP_ST_OK);
    ASSERT_EQ(resp[1], 102);
    ASSERT_EQ(p[0x56], 'A' | 'X' << 8);
    ASSERT_EQ(p[0x57], 'V' | 'D' << 8);
    ASSERT_EQ(p[96], 0x0F); /* logo, fixed, complement, other_lib */
    ASSERT_EQ(p[99], HpLibFlash1M);
}

TEST(test_hp_save_probe_cmd) {
    setup_ruby();
    gba.flash[0] = 0x12;
    gba.flash[1] = 0x34;
    for(uint8_t seq = 1; seq <= 2; seq++) {
        ASSERT_EQ(helper_cmd_run(&hp, seq, HP_CMD_SAVE_PROBE, NULL, 0, resp), HP_ST_OK);
        ASSERT_EQ(resp[1], 5);
        ASSERT_EQ(p[0], 0x09C2);
        ASSERT_EQ(p[1], HpChipFlash128);
        ASSERT_EQ(p[2], 0x3412);
        ASSERT_EQ(p[3], 0x09C2);
        ASSERT_EQ(p[4], 0);
        FakeGbaCounts c;
        fake_gba_counts(&gba, &c);
        ASSERT_EQ(c.id_entries, 1);
        ASSERT_EQ(c.erases + c.programs + c.chip_erases + c.bank_selects, 0);
    }
}

/* SAVE_PROBE takes the game code and version from the cart header itself: 0xD4BF is a 128 KB chip
 * on German Sapphire version 0 only. */
TEST(test_hp_save_probe_header) {
    static const struct {
        const char* code;
        uint8_t version;
        uint16_t cls;
    } cases[] = {
        {"AXPD", 0, HpChipFlash128}, {"AXPD", 1, HpChipFlash64}, {"AXPE", 0, HpChipFlash64}};
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        fake_gba_init(&gba);
        make_header("POKEMON SAPP", cases[i].code, cases[i].version);
        fake_gba_rom(&gba, hdr, 16 * MIB);
        fake_gba_save(&gba, FakeSaveFlash, 0xD4BF);
        hp_init(&hp);
        ASSERT_EQ(helper_cmd_run(&hp, 1, HP_CMD_SAVE_PROBE, NULL, 0, resp), HP_ST_OK);
        ASSERT_EQ(resp[1], 5);
        ASSERT_EQ(p[0], 0xD4BF);
        ASSERT_EQ(p[1], cases[i].cls);
    }
    /* an SRAM: class 2 and the SRAM flag, nothing else */
    setup_ruby();
    fake_gba_save(&gba, FakeSaveSram, 0);
    ASSERT_EQ(helper_cmd_run(&hp, 3, HP_CMD_SAVE_PROBE, NULL, 0, resp), HP_ST_OK);
    ASSERT_EQ(resp[1], 5);
    ASSERT_EQ(p[1], HpChipSram);
    ASSERT_EQ(p[4], 1);
}

int main(void) {
    RUN(test_hp_cart_ruby);
    RUN(test_hp_cart_patched);
    RUN(test_hp_cart_eeprom_only);
    RUN(test_hp_cart_lib_kinds);
    RUN(test_hp_cart_size_8m);
    RUN(test_hp_cart_sizes);
    RUN(test_hp_cart_rom_window_end);
    RUN(test_hp_cart_bad_logo);
    RUN(test_hp_cart_header_checks);
    RUN(test_hp_cart_none);
    RUN(test_hp_cart_codes);
    RUN(test_hp_cart_info_cmd);
    RUN(test_hp_save_probe_cmd);
    RUN(test_hp_save_probe_header);
    TEST_EXIT();
}
