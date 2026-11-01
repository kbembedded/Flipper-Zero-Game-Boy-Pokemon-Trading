#include "test.h"
#include "fake_gba.h"
#include "fake_save.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <hp_core.h>
#include <hp_flash.h>
#include <hp_save.h>

/* The helper's save and ROM commands on the fake GBA (multiboot-and-save.md §6.3-§6.5): the flash
 * reads and when they are allowed, the slot checks, ARM_WRITE and its refusals, the write commands
 * and their gates, duplicates (plan 5 D3), and the ROM reads. A refused command must leave the chip
 * byte-identical. */

#define MIB 0x100000u
#define SECTOR_LEN 4096u
#define ARM_MAGIC 0x5752u
#define DEFER 1u
#define MX 0x09C2u
#define SANYO 0x1362u

static FakeGba gba;
static Hp hp;
static uint8_t hdr[0xC0];
static FakeSaveSpec spec;
static uint16_t resp[HELPER_CMD_RESP_MAX];
static const uint16_t* const p = resp + 2; /* the response payload; resp[1] is its n */
static uint16_t args[2 + 2048];
static uint16_t raw[2 + 2048 + 5];
static uint8_t img[SECTOR_LEN];
static uint8_t before[FAKE_GBA_FLASH_LEN];
static uint8_t seq;

static uint8_t* sector(uint32_t n) {
    return fake_gba_flash(&gba) + n * SECTOR_LEN;
}

static FakeGbaCounts counts(void) {
    FakeGbaCounts c;
    fake_gba_counts(&gba, &c);
    return c;
}

/* Nothing was erased or programmed */
static bool no_writes(void) {
    const FakeGbaCounts c = counts();
    return c.erases == 0 && c.programs == 0 && c.commits == 0 && c.chip_erases == 0;
}

static void snapshot(void) {
    memcpy(before, fake_gba_flash(&gba), sizeof(before));
}

static bool unchanged(void) {
    return memcmp(before, fake_gba_flash(&gba), sizeof(before)) == 0;
}

/* Every sector but n is as it was at the snapshot */
static bool others_unchanged(uint32_t n) {
    const uint32_t lo = n * SECTOR_LEN;
    const uint32_t hi = lo + SECTOR_LEN;
    return memcmp(fake_gba_flash(&gba), before, lo) == 0 &&
           memcmp(fake_gba_flash(&gba) + hi, before + hi, FAKE_GBA_FLASH_LEN - hi) == 0;
}

/* One command with the next seq: its status (HELPER_CMD_BAD if the core misbehaved). */
static uint16_t cmd(uint8_t code, const uint16_t* a, uint16_t n) {
    return helper_cmd_run(&hp, ++seq, code, a, n, resp);
}

static uint16_t cmd1(uint8_t code, uint16_t a0) {
    return cmd(code, &a0, 1);
}

static uint16_t arm(uint16_t id, uint16_t t, uint16_t mode, uint32_t n) {
    const uint16_t a[6] = {ARM_MAGIC, id, t, mode, (uint16_t)n, (uint16_t)(n >> 16)};
    return cmd(HP_CMD_ARM_WRITE, a, 6);
}

static void pack(uint16_t s, uint16_t flags, const uint8_t* image) {
    args[0] = s;
    args[1] = flags;
    for(uint32_t k = 0; k < 2048; k++)
        args[2 + k] = (uint16_t)(image[2 * k] | image[2 * k + 1] << 8);
}

static uint16_t write_sector(uint16_t s, uint16_t flags, const uint8_t* image) {
    pack(s, flags, image);
    return cmd(HP_CMD_WRITE_SECTOR, args, 2050);
}

static uint16_t copy_sector(uint16_t src, uint16_t dst, uint16_t flags, uint32_t counter) {
    const uint16_t a[5] = {src, dst, flags, (uint16_t)counter, (uint16_t)(counter >> 16)};
    return cmd(HP_CMD_COPY_SECTOR, a, 5);
}

/* The default save: Ruby, slot A with counter 4 and id 0 at its first sector, slot B absent. */
static void default_spec(void) {
    spec = (FakeSaveSpec){
        .game = FakeGameRS,
        .slot_present = {true, false},
        .counter = {4, 0},
        .first = {0, 0},
        .seed = 0x5EED0001u,
    };
}

/* Inserts a cart: its header, 16 MiB of ROM with FLASH1M_V103 at 0x3B0000 (and extra, if any, at
 * 0x500000), a flash chip with that id, and spec's save in it. A fresh helper. */
static void insert(const char* code, uint8_t version, uint16_t chip, const char* extra) {
    fake_gba_init(&gba);
    helper_cmd_header(hdr, gba.own, "POKEMON RUBY", code, version);
    fake_gba_rom(&gba, hdr, 16 * MIB);
    fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    if(extra) fake_gba_rom_string(&gba, 0x500000, extra);
    fake_gba_save(&gba, FakeSaveFlash, chip);
    fake_save_build(fake_gba_flash(&gba), &spec);
    hp_init(&hp);
    seq = 0;
}

/* CART_INFO, then SAVE_PROBE, as the Flipper starts */
static bool identify(void) {
    return cmd(HP_CMD_CART_INFO, NULL, 0) == HP_ST_OK &&
           cmd(HP_CMD_SAVE_PROBE, NULL, 0) == HP_ST_OK;
}

/* The default cart: Ruby AXVE, an MX chip, the default save (or spec as the test set it), identified;
 * the counts start at 0. */
static bool setup_spec(void) {
    insert("AXVE", 0, MX, NULL);
    const bool ok = identify();
    memset(&gba.counts, 0, sizeof(gba.counts));
    snapshot();
    return ok;
}

static bool setup(void) {
    default_spec();
    return setup_spec();
}

/* Slot A's sector of id k with its counter set: an image as a game save would carry it. */
static void make_img(uint8_t id, uint32_t counter) {
    memcpy(img, before + fake_save_phys(&spec, 0, id) * SECTOR_LEN, SECTOR_LEN);
    for(uint32_t i = 0; i < 4; i++) img[0xFFC + i] = (uint8_t)(counter >> (8 * i));
}

static uint16_t le16(const uint8_t* b) {
    return (uint16_t)(b[0] | b[1] << 8);
}

/* multiboot-and-save.md §5.3 over len bytes: the test's own */
static uint16_t sum_ref(const uint8_t* b, uint32_t len) {
    uint32_t s = 0;
    for(uint32_t i = 0; i < len; i += 4) {
        s += (uint32_t)b[i] | (uint32_t)b[i + 1] << 8 | (uint32_t)b[i + 2] << 16 |
             (uint32_t)b[i + 3] << 24;
    }
    return (uint16_t)((s >> 16) + s);
}

/* CRC-32 (IEEE, reflected 0xEDB88320), bit by bit: the test's own */
static uint32_t crc32_ref(const uint8_t* b, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for(uint32_t i = 0; i < n; i++) {
        c ^= b[i];
        for(int k = 0; k < 8; k++) c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

/* ---- Flash reads ---- */

/* Bank 1, then bank 0 again: the words are the sector's bytes, little-endian. */
TEST(test_hp_read_sector) {
    ASSERT_TRUE(setup());
    for(uint32_t i = 0; i < SECTOR_LEN; i++) sector(17)[i] = (uint8_t)(i * 29u + 3u);
    static const uint8_t order[2] = {17, 3};
    for(uint32_t r = 0; r < 2; r++) {
        const uint8_t s = order[r];
        ASSERT_EQ(cmd1(HP_CMD_READ_SECTOR, s), HP_ST_OK);
        ASSERT_EQ(resp[1], 2048);
        for(uint32_t k = 0; k < 2048; k++) ASSERT_EQ(p[k], le16(sector(s) + 2 * k));
    }
    ASSERT_TRUE(no_writes());
}

TEST(test_hp_read_footers) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(cmd(HP_CMD_READ_FOOTERS, NULL, 0), HP_ST_OK);
    ASSERT_EQ(resp[1], 256);
    const uint8_t* s5 = sector(5);
    const uint16_t sum5 = le16(s5 + 0xFF6);
    /* the builder's data, not zeros: a reply of zeros can't pass */
    ASSERT_EQ(sum5, sum_ref(s5, 0xF80));
    ASSERT_TRUE(sum5 != 0);
    const uint16_t e5[8] = {5, sum5, 0x2025, 0x0801, 4, 0, sum5, 0};
    ASSERT_MEM_EQ(p + 5 * 8, e5, sizeof(e5));
    /* id 0 (0x890 bytes in Ruby): the zero padding past them changes nothing */
    ASSERT_EQ(p[0], 0);
    ASSERT_EQ(p[6], p[1]);
    /* an erased sector: 992 words of 0xFFFFFFFF sum to 0xFFFFFC20, which folds to 0xFC1F */
    const uint16_t e20[8] = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFC1F, 0};
    ASSERT_MEM_EQ(p + 20 * 8, e20, sizeof(e20));
    /* a non-zero byte past id 0's 0x890 bytes: the game's checksum still holds, the full one not */
    sector(0)[0x890] = 0x5A;
    ASSERT_EQ(cmd(HP_CMD_READ_FOOTERS, NULL, 0), HP_ST_OK);
    ASSERT_EQ(p[1], le16(sector(0) + 0xFF6));
    ASSERT_EQ(p[1], sum_ref(sector(0), 0x890));
    ASSERT_EQ(p[6], sum_ref(sector(0), 0xF80));
    ASSERT_TRUE(p[6] != p[1]);
    ASSERT_TRUE(no_writes());
}

TEST(test_hp_sector_crc32) {
    ASSERT_TRUE(setup());
    static const uint8_t which[3] = {9, 25, 31};
    for(uint32_t i = 0; i < 3; i++) {
        ASSERT_EQ(cmd1(HP_CMD_SECTOR_CRC32, which[i]), HP_ST_OK);
        ASSERT_EQ(resp[1], 2);
        ASSERT_EQ(p[0] | (uint32_t)p[1] << 16, crc32_ref(sector(which[i]), SECTOR_LEN));
    }
    ASSERT_EQ(cmd1(HP_CMD_READ_SECTOR, 32), HP_ST_BAD_ARG);
    ASSERT_EQ(resp[1], 2);
    ASSERT_EQ(cmd1(HP_CMD_SECTOR_CRC32, 32), HP_ST_BAD_ARG);
    ASSERT_TRUE(no_writes());
}

/* No bank command may reach an SRAM (it would land as data), nor a chip the probe hasn't classed
 * as a 128 KB flash, nor any chip before the probe. */
TEST(test_hp_read_refused_unless_flash128) {
    static const uint8_t reads[3] = {HP_CMD_READ_SECTOR, HP_CMD_READ_FOOTERS, HP_CMD_SECTOR_CRC32};
    /* SRAM */
    default_spec();
    insert("AXVE", 0, MX, NULL);
    fake_gba_save(&gba, FakeSaveSram, 0);
    for(uint32_t i = 0; i < FAKE_GBA_SRAM_LEN; i++) gba.flash[i] = (uint8_t)(i * 7u + 1u);
    snapshot();
    ASSERT_TRUE(identify());
    for(uint32_t i = 0; i < 3; i++) {
        const uint16_t a = 3;
        ASSERT_EQ(cmd(reads[i], &a, reads[i] == HP_CMD_READ_FOOTERS ? 0 : 1), HP_ST_UNSUPPORTED);
        ASSERT_EQ(resp[1], 2);
    }
    ASSERT_TRUE(unchanged());
    ASSERT_EQ(counts().bank_selects, 0);
    ASSERT_EQ(counts().sram_writes, 2); /* the probe's one write and its undo */
    /* a 64 KB flash */
    insert("AXVE", 0, 0x1CC2, NULL);
    ASSERT_TRUE(identify());
    gba.trace_len = 0;
    for(uint32_t i = 0; i < 3; i++) {
        const uint16_t a = 3;
        ASSERT_EQ(cmd(reads[i], &a, reads[i] == HP_CMD_READ_FOOTERS ? 0 : 1), HP_ST_UNSUPPORTED);
    }
    ASSERT_EQ(gba.trace_len, 0);
    /* the right chip, but SAVE_PROBE hasn't run: no access to the chip at all */
    insert("AXVE", 0, MX, NULL);
    ASSERT_EQ(cmd(HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    gba.trace_len = 0;
    for(uint32_t i = 0; i < 3; i++) {
        const uint16_t a = 3;
        ASSERT_EQ(cmd(reads[i], &a, reads[i] == HP_CMD_READ_FOOTERS ? 0 : 1), HP_ST_UNSUPPORTED);
    }
    ASSERT_EQ(gba.trace_len, 0);
}

/* ---- The slots, as the game picks them (multiboot-and-save.md §5.5) ---- */

TEST(test_hp_save_slots) {
    static const struct {
        bool a, b;
        uint32_t ca, cb;
        int active;
    } cases[] = {
        {true, false, 4, 0, 0},
        {false, true, 0, 5, 1},
        {true, true, 4, 5, 1},
        {true, true, 6, 5, 0},
        {true, true, 0, 0xFFFFFFFFu, 0}, /* wrapped: 0 is newer than 0xFFFFFFFF */
        {true, true, 0xFFFFFFFFu, 0, 0},
        {true, true, 0xFFFFFFFEu, 0xFFFFFFFFu, 1},
        {true, false, 5, 0, 1}, /* the counter's parity, not where it was found */
        {false, false, 0, 0, -1},
    };
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        spec = (FakeSaveSpec){
            .game = FakeGameRS,
            .slot_present = {cases[i].a, cases[i].b},
            .counter = {cases[i].ca, cases[i].cb},
            .first = {3, 11},
            .seed = (uint32_t)i,
        };
        ASSERT_TRUE(setup_spec());
        ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), cases[i].active);
        uint32_t n = 0x1234;
        ASSERT_EQ(hp_save_slot_ok(0, &n), cases[i].a);
        if(cases[i].a) ASSERT_EQ(n, cases[i].ca);
        ASSERT_EQ(hp_save_slot_ok(1, &n), cases[i].b);
        if(cases[i].b) ASSERT_EQ(n, cases[i].cb);
        ASSERT_TRUE(no_writes());
    }
    uint32_t n;
    /* one counter that differs: not ok, but the game still takes the slot, with the counter of its
     * last valid sector in physical order */
    default_spec();
    spec.first[0] = 5;
    ASSERT_TRUE(setup_spec());
    fake_save_set_counter(gba.flash, 2, 3);
    ASSERT_TRUE(!hp_save_slot_ok(0, &n));
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 0);
    fake_save_set_counter(gba.flash, 13, 7);
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 1);
    /* a broken checksum, a duplicated id, an id over 13: not ok, and the game doesn't take the slot
     * either */
    for(int c = 0; c < 3; c++) {
        ASSERT_TRUE(setup());
        const uint32_t s = fake_save_phys(&spec, 0, 7);
        if(c == 0) fake_save_break_checksum(gba.flash, (uint8_t)s);
        if(c == 1) fake_save_set_id(gba.flash, (uint8_t)s, 6);
        if(c == 2) fake_save_set_id(gba.flash, (uint8_t)s, 14);
        ASSERT_TRUE(!hp_save_slot_ok(0, &n));
        ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), -1);
    }
    /* a non-zero byte past id 0's 0x890 bytes: not a clean save, but the game still takes it */
    ASSERT_TRUE(setup());
    sector(fake_save_phys(&spec, 0, 0))[0x890] = 0x5A;
    ASSERT_TRUE(!hp_save_slot_ok(0, &n));
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 0);
    /* a slot of the wrong number; no layout */
    ASSERT_TRUE(setup());
    ASSERT_TRUE(!hp_save_slot_ok(2, &n));
    ASSERT_EQ(hp_save_active_slot(HpSaveGameUnknown), HP_SAVE_NO_LAYOUT);
}

/* The layout comes from the game code; its sizes are the game's (multiboot-and-save.md §5.4, here
 * fake_save's own table). */
TEST(test_hp_save_layouts) {
    static const struct {
        const char* code;
        HpSaveGame game;
    } codes[] = {
        {"AXVE", HpSaveGameRS},
        {"AXPE", HpSaveGameRS},
        {"AXVD", HpSaveGameRS},
        {"BPRE", HpSaveGameFRLG},
        {"BPGE", HpSaveGameFRLG},
        {"BPEE", HpSaveGameUnknown},
        {"ZFGE", HpSaveGameUnknown},
        {"AXXE", HpSaveGameUnknown},
    };
    for(size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); i++) {
        ASSERT_EQ(hp_save_game((const uint8_t*)codes[i].code), codes[i].game);
    }
    for(uint8_t id = 0; id < 14; id++) {
        ASSERT_EQ(hp_save_size(HpSaveGameRS, id), fake_save_size(FakeGameRS, id));
        ASSERT_EQ(hp_save_size(HpSaveGameFRLG, id), fake_save_size(FakeGameFRLG, id));
        ASSERT_EQ(hp_save_size(HpSaveGameUnknown, id), 0);
    }
    ASSERT_EQ(hp_save_size(HpSaveGameRS, 14), 0);
    ASSERT_EQ(hp_save_size(HpSaveGameFRLG, 0xFFFF), 0);
    /* A 4 and B 5, B's id 0 with a non-zero byte just past its section in the save's own game:
     * that game loads B; read with the other game's sizes, B's id 0 is broken (in FRLG's longer
     * section), or both slots are (an FRLG save's ids 0 and 4 under RS's shorter ones) */
    static const struct {
        FakeGame save;
        uint16_t pad;
        int as_rs, as_frlg;
    } saves[] = {{FakeGameRS, 0x890, 1, 0}, {FakeGameFRLG, 0xF24, -1, 1}};
    for(size_t i = 0; i < 2; i++) {
        default_spec();
        spec.game = saves[i].save;
        spec.slot_present[1] = true;
        spec.counter[1] = 5;
        ASSERT_TRUE(setup_spec());
        sector(fake_save_phys(&spec, 1, 0))[saves[i].pad] = 0x5A;
        ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), saves[i].as_rs);
        ASSERT_EQ(hp_save_active_slot(HpSaveGameFRLG), saves[i].as_frlg);
    }
}

/* ---- ARM_WRITE ---- */

TEST(test_hp_arm_ok) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(resp[1], 0);
    ASSERT_TRUE(no_writes());
    ASSERT_TRUE(unchanged());
    /* the Sanyo chip too; and slot B active, so slot A is the target */
    default_spec();
    spec.slot_present[1] = true;
    spec.counter[1] = 5;
    insert("AXVE", 0, SANYO, NULL);
    ASSERT_TRUE(identify());
    ASSERT_EQ(arm(SANYO, 0, 0, 5), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 0), HP_ST_OK);
}

/* After a refusal nothing is armed (an earlier arming is over too), nothing was written. */
static bool refused_cleanly(void) {
    return cmd1(HP_CMD_ERASE_SECTOR, 14) == HP_ST_NOT_ARMED &&
           cmd1(HP_CMD_ERASE_SECTOR, 0) == HP_ST_NOT_ARMED && no_writes() && unchanged();
}

TEST(test_hp_arm_refused_matrix) {
    /* the target is the slot the game loads, or slot A doesn't hold N; the wrong chip id */
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 0, 0, 4), HP_ST_REFUSED);
    ASSERT_EQ(resp[1], 2);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 1, 0, 5), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 1, 0, 0x10004), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(SANYO, 1, 0, 4), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    /* slot A damaged: a broken checksum, one counter 3, id 7 replaced by 6, a padded section (a
     * non-zero byte past id 0's 0x890 bytes: the game's checksum holds, the full one not) */
    for(int c = 0; c < 4; c++) {
        default_spec();
        insert("AXVE", 0, MX, NULL);
        const uint8_t s = (uint8_t)fake_save_phys(&spec, 0, 7);
        if(c == 0) fake_save_break_checksum(gba.flash, s);
        if(c == 1) fake_save_set_counter(gba.flash, s, 3);
        if(c == 2) fake_save_set_id(gba.flash, s, 6);
        if(c == 3) sector(fake_save_phys(&spec, 0, 0))[0x890] = 0x5A;
        ASSERT_TRUE(identify());
        memset(&gba.counts, 0, sizeof(gba.counts));
        snapshot();
        ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_REFUSED);
        ASSERT_TRUE(refused_cleanly());
    }
    /* A 4 and B 5 both valid: B is what the game loads, so only A may be written */
    default_spec();
    spec.slot_present[1] = true;
    spec.counter[1] = 5;
    ASSERT_TRUE(setup_spec());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 1, 0, 5), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 0, 0, 5), HP_ST_OK);
    /* a refused ARM_WRITE ends the earlier arming */
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    /* A holds 5, the wrong parity for slot A, and B 6: the game loads slot A (6 is even), but A's
     * counter isn't one the game would have written there */
    default_spec();
    spec.slot_present[1] = true;
    spec.counter[0] = 5;
    spec.counter[1] = 6;
    ASSERT_TRUE(setup_spec());
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 0);
    ASSERT_EQ(arm(MX, 1, 0, 5), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    /* mode 1 while a slot is valid */
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 1, 4), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 0, 1, 0), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    /* mode 1 while slot A is what the game loads, though not a clean save (one counter differs):
     * neither slot passes hp_save_slot_ok, yet A must not be written */
    default_spec();
    insert("AXVE", 0, MX, NULL);
    fake_save_set_counter(gba.flash, 3, 2);
    ASSERT_TRUE(identify());
    memset(&gba.counts, 0, sizeof(gba.counts));
    snapshot();
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 0);
    ASSERT_EQ(arm(MX, 0, 1, 4), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 1, 1, 4), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    /* malformed: magic, mode, target */
    const uint16_t bad_magic[6] = {0x1234, MX, 1, 0, 4, 0};
    ASSERT_EQ(cmd(HP_CMD_ARM_WRITE, bad_magic, 6), HP_ST_BAD_ARG);
    ASSERT_EQ(resp[1], 2);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 1, 2, 4), HP_ST_BAD_ARG);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 2, 0, 4), HP_ST_BAD_ARG);
    ASSERT_TRUE(refused_cleanly());
}

/* A 4 is clean, B 5 has a non-zero byte just past its id 0's section. Over the game's own sizes
 * B is valid and newer, so the game loads B: neither slot may be written (B isn't a clean save to
 * keep, and A isn't the slot the game loads). Ruby's layout, then FireRed's. */
TEST(test_hp_arm_padded_loaded) {
    static const struct {
        const char* code;
        FakeGame game;
        uint16_t pad;
    } carts[] = {{"AXVE", FakeGameRS, 0x890}, {"BPRE", FakeGameFRLG, 0xF24}};
    for(size_t i = 0; i < 2; i++) {
        default_spec();
        spec.game = carts[i].game;
        spec.slot_present[1] = true;
        spec.counter[1] = 5;
        insert(carts[i].code, 0, MX, NULL);
        sector(fake_save_phys(&spec, 1, 0))[carts[i].pad] = 0x5A;
        ASSERT_TRUE(identify());
        memset(&gba.counts, 0, sizeof(gba.counts));
        snapshot();
        ASSERT_EQ(hp_save_active_slot(hp_save_game((const uint8_t*)carts[i].code)), 1);
        ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_REFUSED);
        ASSERT_EQ(p[0], HP_REFUSE_SLOT);
        ASSERT_TRUE(refused_cleanly());
        ASSERT_EQ(arm(MX, 0, 0, 5), HP_ST_REFUSED);
        ASSERT_TRUE(refused_cleanly());
        ASSERT_EQ(arm(MX, 0, 1, 5), HP_ST_REFUSED);
        ASSERT_TRUE(refused_cleanly());
        ASSERT_EQ(arm(MX, 1, 1, 5), HP_ST_REFUSED);
        ASSERT_TRUE(refused_cleanly());
    }
    /* the only save, padded: the game loads it, so mode 1 may not write over it */
    ASSERT_TRUE(setup());
    sector(fake_save_phys(&spec, 0, 0))[0x890] = 0x5A;
    snapshot();
    ASSERT_EQ(arm(MX, 0, 1, 0), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 1, 1, 0), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    /* the reverse: id 0's stored checksum matches its whole data area (a padding word 0x00010000
     * past 0x890) but not the game's 0x890 bytes. The game loads nothing, yet slot A is a whole
     * save by the full-length check: mode 1 still refuses to write over it */
    ASSERT_TRUE(setup());
    uint8_t* s0 = sector(fake_save_phys(&spec, 0, 0));
    s0[0x892] = 0x01;
    const uint16_t full = sum_ref(s0, 0xF80);
    s0[0xFF6] = (uint8_t)full;
    s0[0xFF7] = (uint8_t)(full >> 8);
    snapshot();
    uint32_t n = 0;
    ASSERT_TRUE(hp_save_slot_ok(0, &n));
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), -1);
    ASSERT_EQ(arm(MX, 1, 1, 4), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 0, 1, 4), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    /* a clean FireRed save on a FireRed cart arms */
    default_spec();
    spec.game = FakeGameFRLG;
    insert("BPRE", 0, MX, NULL);
    ASSERT_TRUE(identify());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_OK);
}

/* Without a known save layout the helper can't tell which slot the game loads: nothing is armed or
 * written. CART_INFO's list of writable games implies a layout, so this pins the second check on
 * its own, with the scanned code changed behind CART_INFO's back. */
TEST(test_hp_arm_unknown_layout) {
    ASSERT_TRUE(setup());
    memcpy(hp.cart.code, "BPEE", 4);
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_LAYOUT);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_LAYOUT);
    ASSERT_TRUE(no_writes());
    ASSERT_TRUE(unchanged());
    /* armed, then the layout is gone: every write is refused */
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    memcpy(hp.cart.code, "ZFGE", 4);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_LAYOUT);
    ASSERT_TRUE(no_writes());
    /* a real Emerald cart stops at the cart check */
    default_spec();
    insert("BPEE", 0, MX, NULL);
    ASSERT_TRUE(identify());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_CART);
}

/* A malformed ARM_WRITE ends the arming, and forgets the erases, before it looks at its
 * arguments. */
TEST(test_hp_arm_bad_args_end_arming) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_OK);
    ASSERT_EQ(hp.flash.erased_ok, 1u << 14);
    const uint16_t bad_magic[6] = {0x1234, MX, 1, 0, 4, 0};
    ASSERT_EQ(cmd(HP_CMD_ARM_WRITE, bad_magic, 6), HP_ST_BAD_ARG);
    ASSERT_EQ(hp.flash.erased_ok, 0);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 15), HP_ST_NOT_ARMED);
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(arm(MX, 1, 2, 4), HP_ST_BAD_ARG);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 15), HP_ST_NOT_ARMED);
    ASSERT_EQ(counts().erases, 1);
}

/* Carts that may never be written: Emerald, Japanese, German, a ROM that links another save
 * library, a 64 KB chip, a bad logo, no SAVE_PROBE, no CART_INFO. Every write command is refused
 * before it touches the chip. */
TEST(test_hp_arm_refused_carts) {
    static const struct {
        const char* code;
        uint16_t chip;
        const char* extra;
        bool bad_logo;
        bool info;
        bool probe;
    } carts[] = {
        {"BPEE", MX, NULL, false, true, true},
        {"AXVJ", MX, NULL, false, true, true},
        {"AXVD", MX, NULL, false, true, true},
        {"AXVE", MX, "SRAM_V113", false, true, true},
        {"AXVE", 0x1CC2, NULL, false, true, true},
        {"AXVE", MX, NULL, true, true, true},
        {"AXVE", MX, NULL, false, true, false},
        {"AXVE", MX, NULL, false, false, true},
    };
    for(size_t i = 0; i < sizeof(carts) / sizeof(carts[0]); i++) {
        default_spec();
        insert(carts[i].code, 0, carts[i].chip, carts[i].extra);
        if(carts[i].bad_logo) {
            hdr[0x10] ^= 0xFF;
            fake_gba_rom(&gba, hdr, 16 * MIB);
            fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
        }
        if(carts[i].info) ASSERT_EQ(cmd(HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
        if(carts[i].probe) ASSERT_EQ(cmd(HP_CMD_SAVE_PROBE, NULL, 0), HP_ST_OK);
        memset(&gba.counts, 0, sizeof(gba.counts));
        snapshot();
        ASSERT_EQ(arm(carts[i].chip, 1, 0, 4), HP_ST_REFUSED);
        ASSERT_EQ(arm(MX, 0, 1, 0), HP_ST_REFUSED);
        ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_REFUSED);
        make_img(1, 5);
        ASSERT_EQ(write_sector(14, DEFER, img), HP_ST_REFUSED);
        ASSERT_EQ(copy_sector(3, 14, DEFER, 5), HP_ST_REFUSED);
        ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 14), HP_ST_REFUSED);
        ASSERT_TRUE(no_writes());
        ASSERT_TRUE(unchanged());
    }
}

/* 0xD4BF is a 128 KB chip only on German Sapphire version 0, which may never be written: a cart
 * whose header changed after the probe can't carry that class into a write. */
TEST(test_hp_arm_refused_id_d4bf) {
    default_spec();
    insert("AXPD", 0, 0xD4BF, NULL);
    ASSERT_EQ(cmd(HP_CMD_SAVE_PROBE, NULL, 0), HP_ST_OK);
    ASSERT_EQ(p[0], 0xD4BF);
    ASSERT_EQ(p[1], HpChipFlash128);
    helper_cmd_header(hdr, gba.own, "POKEMON SAPP", "AXPE", 0);
    fake_gba_rom(&gba, hdr, 16 * MIB);
    fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    ASSERT_EQ(cmd(HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_TRUE(p[96] & HP_CART_WRITE_GAME);
    memset(&gba.counts, 0, sizeof(gba.counts));
    snapshot();
    ASSERT_EQ(arm(0xD4BF, 1, 0, 4), HP_ST_REFUSED);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_REFUSED);
    ASSERT_TRUE(no_writes());
    ASSERT_TRUE(unchanged());
}

/* ---- The write commands ---- */

TEST(test_hp_write_gating) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    static const uint16_t outside[6] = {3, 0, 13, 28, 31, 0xFFFF};
    make_img(3, 5);
    for(uint32_t i = 0; i < 6; i++) {
        ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, outside[i]), HP_ST_REFUSED);
        ASSERT_EQ(resp[1], 2);
        ASSERT_EQ(write_sector(outside[i], DEFER, img), HP_ST_REFUSED);
        ASSERT_EQ(write_sector(outside[i], 0, img), HP_ST_REFUSED);
        ASSERT_EQ(copy_sector(14, outside[i], DEFER, 5), HP_ST_REFUSED);
        ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, outside[i]), HP_ST_REFUSED);
    }
    ASSERT_TRUE(no_writes());
    ASSERT_TRUE(unchanged());
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_OK);
    ASSERT_EQ(resp[1], 0);
    ASSERT_EQ(counts().erases, 1);
    ASSERT_EQ(cmd(HP_CMD_DISARM, NULL, 0), HP_ST_OK);
    ASSERT_EQ(resp[1], 0);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 15), HP_ST_NOT_ARMED);
    ASSERT_EQ(resp[1], 2);
    ASSERT_EQ(write_sector(15, DEFER, img), HP_ST_NOT_ARMED);
    ASSERT_EQ(copy_sector(3, 15, DEFER, 5), HP_ST_NOT_ARMED);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 14), HP_ST_NOT_ARMED);
    ASSERT_EQ(counts().erases, 1);
    ASSERT_EQ(counts().programs, 0);
    ASSERT_TRUE(unchanged());
}

/* Malformed write commands: flags other than DEFER_COMMIT alone (a write without it would program
 * the signature before the counter after it); a copy whose source isn't in the slot the game
 * loads. */
TEST(test_hp_write_args) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    make_img(2, 5);
    ASSERT_EQ(write_sector(14, 0, img), HP_ST_BAD_ARG);
    ASSERT_EQ(write_sector(14, 2, img), HP_ST_BAD_ARG);
    ASSERT_EQ(write_sector(14, 0x8001, img), HP_ST_BAD_ARG);
    ASSERT_EQ(copy_sector(3, 14, 0, 5), HP_ST_BAD_ARG);
    ASSERT_EQ(copy_sector(3, 14, 2, 5), HP_ST_BAD_ARG);
    ASSERT_EQ(copy_sector(20, 14, DEFER, 5), HP_ST_BAD_ARG);
    ASSERT_EQ(copy_sector(28, 14, DEFER, 5), HP_ST_BAD_ARG);
    ASSERT_EQ(copy_sector(0x100, 14, DEFER, 5), HP_ST_BAD_ARG);
    ASSERT_TRUE(no_writes());
    ASSERT_TRUE(unchanged());
    /* still armed */
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_OK);
}

TEST(test_hp_write_full_path) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    make_img(6, 5);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 20), HP_ST_OK);
    ASSERT_EQ(write_sector(20, DEFER, img), HP_ST_OK);
    ASSERT_EQ(resp[1], 0);
    ASSERT_MEM_EQ(sector(20), img, 0xFF8);
    ASSERT_EQ(sector(20)[0xFF8], 0xFF);
    ASSERT_MEM_EQ(sector(20) + 0xFF9, img + 0xFF9, 7);
    ASSERT_EQ(counts().erases, 1); /* this arming's own erase: the write needn't erase again */
    ASSERT_EQ(counts().commits, 0);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 20), HP_ST_OK);
    ASSERT_EQ(resp[1], 0);
    ASSERT_MEM_EQ(sector(20), img, SECTOR_LEN);
    ASSERT_EQ(counts().commits, 1);
    ASSERT_TRUE(others_unchanged(20));
}

/* COPY reads the source into the buffer, patches the counter, and writes like WRITE_SECTOR. */
TEST(test_hp_copy_sector) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(copy_sector(3, 17, DEFER, 0x12345679u), HP_ST_OK);
    ASSERT_EQ(resp[1], 0);
    ASSERT_MEM_EQ(sector(17), sector(3), 0xFF8);
    ASSERT_EQ(sector(17)[0xFF8], 0xFF);
    ASSERT_MEM_EQ(sector(17) + 0xFF9, sector(3) + 0xFF9, 3);
    static const uint8_t ctr[4] = {0x79, 0x56, 0x34, 0x12};
    ASSERT_MEM_EQ(sector(17) + 0xFFC, ctr, 4);
    ASSERT_EQ(counts().erases, 1); /* not erased by this arming: the copy erased first */
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 17), HP_ST_OK);
    ASSERT_EQ(sector(17)[0xFF8], 0x25);
    ASSERT_TRUE(others_unchanged(17));
}

/* The plan's three passes over slot B: erase, program (deferred), commit. The 14th commit
 * completes the new slot, which the game now loads, so the helper disarms. */
TEST(test_hp_disarm_after_14_commits) {
    static uint8_t images[14][SECTOR_LEN];
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    for(uint8_t k = 0; k < 14; k++) ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14 + k), HP_ST_OK);
    for(uint8_t k = 0; k < 14; k++) {
        make_img(k, 5);
        memcpy(images[(k + 1) % 14], img, SECTOR_LEN);
        ASSERT_EQ(write_sector(14 + (k + 1) % 14, DEFER, img), HP_ST_OK);
    }
    for(uint8_t k = 0; k < 13; k++) ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 14 + k), HP_ST_OK);
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 0);
    ASSERT_EQ(
        helper_cmd_run(&hp, 0x77, HP_CMD_COMMIT_SECTOR, (const uint16_t[]){27}, 1, resp),
        HP_ST_OK);
    ASSERT_EQ(counts().commits, 14);
    /* its response went missing: the resend is answered, not executed */
    ASSERT_EQ(
        helper_cmd_run(&hp, 0x77, HP_CMD_COMMIT_SECTOR, (const uint16_t[]){27}, 1, resp),
        HP_ST_OK);
    ASSERT_EQ(counts().commits, 14);
    ASSERT_EQ(counts().erases, 14);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_NOT_ARMED);
    ASSERT_EQ(counts().erases, 14);
    uint32_t n = 0;
    ASSERT_TRUE(hp_save_slot_ok(1, &n));
    ASSERT_EQ(n, 5);
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 1);
    /* slot B is exactly the images; slot A and sectors 28..31 are as they were */
    for(uint32_t k = 0; k < 14; k++) ASSERT_MEM_EQ(sector(14 + k), images[k], SECTOR_LEN);
    for(uint32_t s = 0; s < 14; s++) ASSERT_MEM_EQ(sector(s), before + s * SECTOR_LEN, SECTOR_LEN);
    for(uint32_t s = 28; s < 32; s++) {
        ASSERT_MEM_EQ(sector(s), before + s * SECTOR_LEN, SECTOR_LEN);
    }
}

/* A write that would program the signature with the rest of the sector, before the counter after
 * it, is refused: a power cut between them would leave a valid signature over a torn counter. */
TEST(test_hp_write_needs_defer) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    make_img(0, 5);
    ASSERT_EQ(write_sector(14, 0, img), HP_ST_BAD_ARG);
    ASSERT_EQ(resp[1], 2);
    ASSERT_EQ(copy_sector(0, 14, 0, 5), HP_ST_BAD_ARG);
    ASSERT_TRUE(no_writes());
    ASSERT_TRUE(unchanged());
    ASSERT_EQ(write_sector(14, DEFER, img), HP_ST_OK);
}

/* COMMIT_SECTOR completes only what this arming programmed: images a link drop or power cut left
 * in slot B from an earlier arming can't be committed into the save the game loads. */
TEST(test_hp_commit_needs_program) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    /* the first arming programs sectors 15..27 */
    for(uint8_t k = 0; k < 13; k++) {
        ASSERT_EQ(copy_sector(k, 15 + k, DEFER, 5), HP_ST_OK);
    }
    /* a new session and a new arming, which programs sector 14 only: the commits of 15..27 are
     * all refused, so what the arming programmed is known per sector */
    ASSERT_EQ(cmd(HP_CMD_HELLO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(copy_sector(13, 14, DEFER, 5), HP_ST_OK);
    memset(&gba.counts, 0, sizeof(gba.counts));
    snapshot();
    for(uint8_t s = 15; s < 28; s++) {
        ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, s), HP_ST_REFUSED);
        ASSERT_EQ(p[0], HP_REFUSE_NOT_PROGRAMMED);
        ASSERT_EQ(p[1], s);
    }
    ASSERT_TRUE(no_writes());
    ASSERT_TRUE(unchanged());
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 0);
    /* an erase ends what the arming programmed there; so does a new arming without a HELLO */
    ASSERT_EQ(copy_sector(3, 17, DEFER, 5), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 17), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 17), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_NOT_PROGRAMMED);
    ASSERT_EQ(copy_sector(3, 17, DEFER, 5), HP_ST_OK);
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 17), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_NOT_PROGRAMMED);
    /* what this arming programmed commits: the earlier copies left no valid signature in the way */
    ASSERT_EQ(copy_sector(3, 18, DEFER, 5), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 18), HP_ST_OK);
    ASSERT_EQ(sector(18)[0xFF8], 0x25);
    for(uint32_t s = 0; s < 14; s++) ASSERT_MEM_EQ(sector(s), before + s * SECTOR_LEN, SECTOR_LEN);
}

/* CART_INFO ends the arming, and the probe: until SAVE_PROBE runs again, the chip check refuses */
TEST(test_hp_cart_info_disarms) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(cmd(HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_CHIP);
    ASSERT_EQ(cmd(HP_CMD_SAVE_PROBE, NULL, 0), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_NOT_ARMED);
    ASSERT_TRUE(no_writes());
}

/* A cart swapped while the helper runs, a genuine MX Ruby for an SRAM repro with the same header,
 * then the commands a reconnect sends: CART_INFO forgets the last probe, so SAVE_PROBE probes the
 * new chip and finds the SRAM, and no flash command (a bank select, say) lands in it. Only the
 * probe's own write and its undo reach the SRAM, which keeps every byte. */
TEST(test_hp_cart_info_reprobes) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(cmd(HP_CMD_READ_FOOTERS, NULL, 0), HP_ST_OK);
    fake_gba_rom_string(&gba, 0x500000, "SRAM_V113");
    fake_gba_save(&gba, FakeSaveSram, 0);
    for(uint32_t i = 0; i < FAKE_GBA_SRAM_LEN; i++) gba.flash[i] = (uint8_t)(i * 7u + 1u);
    snapshot();
    memset(&gba.counts, 0, sizeof(gba.counts));
    ASSERT_EQ(cmd(HP_CMD_HELLO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(cmd(HP_CMD_CART_INFO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(cmd(HP_CMD_SAVE_PROBE, NULL, 0), HP_ST_OK);
    ASSERT_EQ(p[1], HpChipSram);
    ASSERT_EQ(p[4], 1);
    static const uint8_t reads[3] = {HP_CMD_READ_SECTOR, HP_CMD_READ_FOOTERS, HP_CMD_SECTOR_CRC32};
    for(uint32_t i = 0; i < 3; i++) {
        const uint16_t a = 3;
        ASSERT_EQ(cmd(reads[i], &a, reads[i] == HP_CMD_READ_FOOTERS ? 0 : 1), HP_ST_UNSUPPORTED);
    }
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_REFUSED);
    ASSERT_TRUE(unchanged());
    ASSERT_EQ(counts().bank_selects, 0);
    ASSERT_EQ(counts().sram_writes, 2); /* the probe's one write and its undo */
}

/* An erase or a new deferred write of a completed sector un-completes it, so the 14th completion,
 * which disarms, counts only sectors that are complete now. */
TEST(test_hp_completed_forgets) {
    for(int undo = 0; undo < 2; undo++) {
        ASSERT_TRUE(setup());
        ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
        for(uint8_t k = 0; k < 14; k++) {
            make_img(k, 5);
            ASSERT_EQ(write_sector(14 + k, DEFER, img), HP_ST_OK);
        }
        for(uint8_t s = 14; s < 27; s++) ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, s), HP_ST_OK);
        ASSERT_EQ(hp.completed, 0x1FFF);
        make_img(0, 5);
        if(undo == 0) {
            ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_OK);
        } else {
            ASSERT_EQ(write_sector(14, DEFER, img), HP_ST_OK);
        }
        ASSERT_EQ(hp.completed, 0x1FFE);
        ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 27), HP_ST_OK);
        ASSERT_TRUE(hp.armed); /* 13 complete: sector 14 isn't */
        if(undo == 0) ASSERT_EQ(write_sector(14, DEFER, img), HP_ST_OK);
        ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 14), HP_ST_OK);
        ASSERT_TRUE(!hp.armed);
        ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 1);
    }
}

/* Slot B holds the previous save (counter 3), which the game ignores while A is newer. A sector of
 * B may become valid only once every other sector of B either became valid in this arming or
 * holds no valid signature: otherwise one new sector among 13 old ones could make B a mixed slot
 * that the game loads, while the helper is still armed for it. */
TEST(test_hp_complete_needs_clear_slot) {
    default_spec();
    spec.slot_present[1] = true;
    spec.counter[1] = 3;
    ASSERT_TRUE(setup_spec());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    make_img(0, 5);
    ASSERT_EQ(write_sector(14, DEFER, img), HP_ST_OK);
    const FakeGbaCounts c0 = counts();
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 14), HP_ST_REFUSED);
    ASSERT_EQ(resp[1], 2);
    ASSERT_EQ(p[0], HP_REFUSE_VALID);
    ASSERT_EQ(p[1], 15); /* the first sector still valid */
    ASSERT_EQ(counts().erases, c0.erases);
    ASSERT_EQ(counts().programs, c0.programs);
    ASSERT_EQ(sector(14)[0xFF8], 0xFF);
    /* still armed: clear the rest of B, then the commit goes through */
    for(uint8_t s = 15; s < 27; s++) ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, s), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 14), HP_ST_REFUSED);
    ASSERT_EQ(p[1], 27);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 27), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 14), HP_ST_OK);
    /* a sector this arming completed doesn't block the next */
    make_img(1, 5);
    ASSERT_EQ(write_sector(15, DEFER, img), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 15), HP_ST_OK);
    /* a new arming doesn't count what an earlier one completed */
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(write_sector(17, DEFER, img), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 17), HP_ST_REFUSED);
    ASSERT_EQ(p[1], 14);
    for(uint32_t s = 0; s < 14; s++) ASSERT_MEM_EQ(sector(s), before + s * SECTOR_LEN, SECTOR_LEN);
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 0);
    /* slot A armed, its sector 0 in the way */
    default_spec();
    spec.slot_present[1] = true;
    spec.counter[1] = 5;
    ASSERT_TRUE(setup_spec());
    ASSERT_EQ(arm(MX, 0, 0, 5), HP_ST_OK);
    make_img(1, 6);
    ASSERT_EQ(write_sector(1, DEFER, img), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 1), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_VALID);
    ASSERT_EQ(p[1], 0);
    ASSERT_EQ(counts().commits, 0);
}

/* ---- Session state ---- */

/* A new arming, like a new session, forgets the erases before it: a write erases again. */
TEST(test_hp_arm_clears_erased_ok) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_OK);
    ASSERT_EQ(hp.flash.erased_ok, 1u << 14);
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(hp.flash.erased_ok, 0);
    make_img(0, 5);
    ASSERT_EQ(write_sector(14, DEFER, img), HP_ST_OK);
    ASSERT_EQ(counts().erases, 2);
    /* a refused one as well */
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 15), HP_ST_OK);
    ASSERT_EQ(arm(MX, 1, 0, 5), HP_ST_REFUSED);
    ASSERT_EQ(hp.flash.erased_ok, 0);
}

TEST(test_hp_hello_disarms) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(cmd(HP_CMD_HELLO, NULL, 0), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_NOT_ARMED);
    ASSERT_TRUE(no_writes());
}

/* Any flash error ends the arming. */
TEST(test_hp_flash_error_disarms) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    fake_gba_fail_program(&gba, 21, 0, 99);
    make_img(7, 5);
    ASSERT_EQ(write_sector(21, DEFER, img), HP_ST_PROGRAM_FAIL);
    ASSERT_EQ(resp[1], 2);
    ASSERT_EQ(p[0], 0xA001); /* DQ5 while programming */
    ASSERT_EQ(p[1], 0);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 22), HP_ST_NOT_ARMED);
    /* an erase failure; a commit onto a byte already programmed */
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    fake_gba_fail_erase(&gba, 22, 5);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 22), HP_ST_ERASE_FAIL);
    ASSERT_EQ(p[0], 0xA002);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 23), HP_ST_NOT_ARMED);
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    ASSERT_EQ(write_sector(23, DEFER, img), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 23), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 23), HP_ST_NOT_ERASED);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 24), HP_ST_NOT_ARMED);
    for(uint32_t s = 0; s < 14; s++) ASSERT_MEM_EQ(sector(s), before + s * SECTOR_LEN, SECTOR_LEN);
}

/* A blank cart, or one with no valid slot: mode 1 restores onto it. */
TEST(test_hp_mode1_blank) {
    default_spec();
    spec.slot_present[0] = false;
    ASSERT_TRUE(setup_spec());
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), -1);
    ASSERT_EQ(arm(MX, 0, 0, 0), HP_ST_REFUSED);
    ASSERT_EQ(arm(MX, 1, 0, 0), HP_ST_REFUSED);
    ASSERT_TRUE(refused_cleanly());
    ASSERT_EQ(arm(MX, 0, 1, 0), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 0), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_REFUSED);
    ASSERT_TRUE(others_unchanged(0));
    /* both slots present, both damaged: a restore into B leaves A as it was */
    default_spec();
    spec.slot_present[1] = true;
    spec.counter[1] = 5;
    insert("AXVE", 0, MX, NULL);
    fake_save_break_checksum(gba.flash, 2);
    fake_save_break_checksum(gba.flash, 20);
    ASSERT_TRUE(identify());
    snapshot();
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_REFUSED);
    ASSERT_EQ(arm(MX, 1, 1, 5), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_OK);
    make_img(1, 5);
    ASSERT_EQ(write_sector(15, DEFER, img), HP_ST_OK);
    ASSERT_EQ(cmd1(HP_CMD_COMMIT_SECTOR, 15), HP_ST_REFUSED); /* B's other sectors still valid */
    for(uint32_t s = 0; s < 14; s++) ASSERT_MEM_EQ(sector(s), before + s * SECTOR_LEN, SECTOR_LEN);
    for(uint32_t s = 28; s < 32; s++) {
        ASSERT_MEM_EQ(sector(s), before + s * SECTOR_LEN, SECTOR_LEN);
    }
}

/* ---- Duplicates (plan 5 D3) ---- */

/* The CRC a request carries: over w1, n and its arguments, each word low byte first */
static uint32_t request_crc(uint16_t w1, const uint16_t* a, uint16_t n) {
    uint8_t b[4 + 2 * 8];
    b[0] = (uint8_t)w1;
    b[1] = (uint8_t)(w1 >> 8);
    b[2] = (uint8_t)n;
    b[3] = (uint8_t)(n >> 8);
    for(uint16_t i = 0; i < n; i++) {
        b[4 + 2 * i] = (uint8_t)a[i];
        b[5 + 2 * i] = (uint8_t)(a[i] >> 8);
    }
    return crc32_ref(b, 4u + 2u * n);
}

/* x with the XOR of g[i] over x's set bits equal to t (Gaussian elimination over GF(2)) */
static bool solve32(const uint32_t g[32], uint32_t t, uint32_t* x) {
    uint32_t row[32];
    uint32_t comb[32];
    bool have[32] = {false};
    for(uint32_t i = 0; i < 32; i++) {
        uint32_t v = g[i];
        uint32_t c = 1u << i;
        for(int b = 31; b >= 0 && v; b--) {
            if(!((v >> b) & 1u)) continue;
            if(!have[b]) {
                have[b] = true;
                row[b] = v;
                comb[b] = c;
                break;
            }
            v ^= row[b];
            c ^= comb[b];
        }
    }
    uint32_t c = 0;
    for(int b = 31; b >= 0; b--) {
        if(!((t >> b) & 1u)) continue;
        if(!have[b]) return false;
        t ^= row[b];
        c ^= comb[b];
    }
    *x = c;
    return true;
}

/* A request with the last one's code and CRC but another seq is a new request: CRC-32 is linear,
 * so changing the counter words (the last 32 bits) can cancel the seq's change exactly. Executed,
 * this ARM_WRITE is refused (its N is wrong) and ends the arming; answered from memory it would
 * say OK. */
TEST(test_hp_dedupe_seq) {
    ASSERT_TRUE(setup());
    const uint16_t a[6] = {ARM_MAGIC, MX, 1, 0, 4, 0};
    const uint16_t w1a = 0x60 << 8 | HP_CMD_ARM_WRITE;
    const uint16_t w1b = 0x61 << 8 | HP_CMD_ARM_WRITE;
    const uint32_t base = request_crc(w1a, a, 6);
    uint32_t g[32];
    for(uint32_t i = 0; i < 32; i++) {
        uint16_t d[6];
        memcpy(d, a, sizeof(d));
        d[4 + i / 16] ^= (uint16_t)(1u << (i % 16));
        g[i] = request_crc(w1a, d, 6) ^ base;
    }
    uint32_t x = 0;
    ASSERT_TRUE(solve32(g, request_crc(w1b, a, 6) ^ base, &x));
    uint16_t b[6];
    memcpy(b, a, sizeof(b));
    b[4] ^= (uint16_t)x;
    b[5] ^= (uint16_t)(x >> 16);
    ASSERT_EQ(request_crc(w1b, b, 6), base);
    ASSERT_TRUE(x != 0);
    ASSERT_EQ(helper_cmd_run(&hp, 0x60, HP_CMD_ARM_WRITE, a, 6, resp), HP_ST_OK);
    ASSERT_EQ(helper_cmd_run(&hp, 0x61, HP_CMD_ARM_WRITE, b, 6, resp), HP_ST_REFUSED);
    ASSERT_EQ(p[0], HP_REFUSE_SLOT);
    ASSERT_EQ(cmd1(HP_CMD_ERASE_SECTOR, 14), HP_ST_NOT_ARMED);
}

TEST(test_hp_dedupe_write_resend) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    const uint16_t s14 = 14;
    const uint16_t s15 = 15;
    ASSERT_EQ(helper_cmd_run(&hp, 0x40, HP_CMD_ERASE_SECTOR, &s14, 1, resp), HP_ST_OK);
    ASSERT_EQ(counts().erases, 1);
    ASSERT_EQ(helper_cmd_run(&hp, 0x40, HP_CMD_ERASE_SECTOR, &s14, 1, resp), HP_ST_OK);
    ASSERT_EQ(counts().erases, 1);
    ASSERT_EQ(helper_cmd_run(&hp, 0x41, HP_CMD_ERASE_SECTOR, &s15, 1, resp), HP_ST_OK);
    ASSERT_EQ(counts().erases, 2);
    ASSERT_EQ(helper_cmd_run(&hp, 0x40, HP_CMD_ERASE_SECTOR, &s14, 1, resp), HP_ST_OK);
    ASSERT_EQ(counts().erases, 3);
}

TEST(test_hp_dedupe_rules) {
    ASSERT_TRUE(setup());
    ASSERT_EQ(arm(MX, 1, 0, 4), HP_ST_OK);
    const uint16_t s14 = 14;
    /* a packet with a bad CRC isn't a packet: the memory stays */
    ASSERT_EQ(helper_cmd_run(&hp, 0x50, HP_CMD_ERASE_SECTOR, &s14, 1, resp), HP_ST_OK);
    const uint32_t len = helper_cmd_make(raw, 0x50, HP_CMD_ERASE_SECTOR, &s14, 1);
    raw[len - 1] ^= 0x0100;
    ASSERT_EQ(helper_cmd_feed(&hp, raw, len), HP_READY);
    ASSERT_EQ(helper_cmd_run(&hp, 0x50, HP_CMD_ERASE_SECTOR, &s14, 1, resp), HP_ST_OK);
    ASSERT_EQ(counts().erases, 1);
    /* a WRITE_SECTOR resent whole: one program */
    make_img(0, 5);
    pack(14, DEFER, img);
    ASSERT_EQ(helper_cmd_run(&hp, 0x51, HP_CMD_WRITE_SECTOR, args, 2050, resp), HP_ST_OK);
    const uint32_t programs = counts().programs;
    ASSERT_EQ(helper_cmd_run(&hp, 0x51, HP_CMD_WRITE_SECTOR, args, 2050, resp), HP_ST_OK);
    ASSERT_EQ(counts().programs, programs);
    /* a failure is answered again, the same, without another try */
    fake_gba_fail_erase(&gba, 15, 5);
    const uint16_t s15 = 15;
    ASSERT_EQ(helper_cmd_run(&hp, 0x52, HP_CMD_ERASE_SECTOR, &s15, 1, resp), HP_ST_ERASE_FAIL);
    const uint16_t code = p[0];
    const uint32_t erases = counts().erases;
    ASSERT_EQ(helper_cmd_run(&hp, 0x52, HP_CMD_ERASE_SECTOR, &s15, 1, resp), HP_ST_ERASE_FAIL);
    ASSERT_EQ(p[0], code);
    ASSERT_EQ(counts().erases, erases);
    /* reads run again */
    const uint16_t s30 = 30;
    ASSERT_EQ(helper_cmd_run(&hp, 0x53, HP_CMD_SECTOR_CRC32, &s30, 1, resp), HP_ST_OK);
    const uint32_t crc = p[0] | (uint32_t)p[1] << 16;
    sector(30)[100] ^= 0x01;
    ASSERT_EQ(helper_cmd_run(&hp, 0x53, HP_CMD_SECTOR_CRC32, &s30, 1, resp), HP_ST_OK);
    ASSERT_TRUE((p[0] | (uint32_t)p[1] << 16) != crc);
    ASSERT_EQ(p[0] | (uint32_t)p[1] << 16, crc32_ref(sector(30), SECTOR_LEN));
    /* an ARM_WRITE resent is answered from memory: no slot is read again */
    const uint16_t arm_args[6] = {ARM_MAGIC, MX, 1, 0, 4, 0};
    ASSERT_EQ(helper_cmd_run(&hp, 0x54, HP_CMD_ARM_WRITE, arm_args, 6, resp), HP_ST_OK);
    gba.trace_len = 0;
    ASSERT_EQ(helper_cmd_run(&hp, 0x54, HP_CMD_ARM_WRITE, arm_args, 6, resp), HP_ST_OK);
    ASSERT_EQ(gba.trace_len, 0);
    ASSERT_EQ(helper_cmd_run(&hp, 0x55, HP_CMD_ERASE_SECTOR, &s14, 1, resp), HP_ST_OK);
}

/* ---- ROM reads ---- */

TEST(test_hp_rom_read) {
    ASSERT_TRUE(setup());
    gba.trace_len = 0;
    const uint16_t a[3] = {0x0100, 0x0800, 4};
    ASSERT_EQ(cmd(HP_CMD_ROM_READ, a, 3), HP_ST_OK);
    ASSERT_EQ(resp[1], 4);
    for(uint32_t k = 0; k < 4; k++) ASSERT_EQ(p[k], hp_hal_rom16(0x100 + 2 * k));
    const uint16_t code[3] = {0x00AC, 0x0800, 2};
    ASSERT_EQ(cmd(HP_CMD_ROM_READ, code, 3), HP_ST_OK);
    ASSERT_EQ(p[0], 'A' | 'X' << 8);
    ASSERT_EQ(p[1], 'V' | 'E' << 8);
    const uint16_t whole[3] = {0x0000, 0x0800, 2048};
    ASSERT_EQ(cmd(HP_CMD_ROM_READ, whole, 3), HP_ST_OK);
    ASSERT_EQ(resp[1], 2048);
    for(uint32_t k = 0; k < 0x60; k++) ASSERT_EQ(p[k], le16(hdr + 2 * k));
    for(uint32_t k = 0x60; k < 2048; k++) ASSERT_EQ(p[k], hp_hal_rom16(2 * k));
    const uint16_t last[3] = {0xFFFC, 0x09FF, 2};
    ASSERT_EQ(cmd(HP_CMD_ROM_READ, last, 3), HP_ST_OK);
    static const uint16_t bad[][3] = {
        {0x0000, 0x0A00, 1},
        {0xFFFE, 0x09FF, 2},
        {0xFFFE, 0x07FF, 1},
        {0x0001, 0x0800, 1},
        {0x0000, 0x0800, 0},
        {0x0000, 0x0800, 2049},
        {0xF002, 0x09FF, 2048},
    };
    for(size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT_EQ(cmd(HP_CMD_ROM_READ, bad[i], 3), HP_ST_BAD_ARG);
        ASSERT_EQ(resp[1], 2);
    }
    ASSERT_EQ(gba.trace_len, 0);
}

TEST(test_hp_rom_crc32) {
    ASSERT_TRUE(setup());
    static uint8_t rom[0x40000];
    for(uint32_t i = 0; i < sizeof(rom); i += 2) {
        const uint16_t hw = hp_hal_rom16(i);
        rom[i] = (uint8_t)hw;
        rom[i + 1] = (uint8_t)(hw >> 8);
    }
    const uint16_t a[4] = {0x0000, 0x0800, 0x0000, 0x0004};
    ASSERT_EQ(cmd(HP_CMD_ROM_CRC32, a, 4), HP_ST_OK);
    ASSERT_EQ(resp[1], 2);
    ASSERT_EQ(p[0] | (uint32_t)p[1] << 16, crc32_ref(rom, sizeof(rom)));
    const uint16_t small[4] = {0x0010, 0x0800, 6, 0};
    ASSERT_EQ(cmd(HP_CMD_ROM_CRC32, small, 4), HP_ST_OK);
    ASSERT_EQ(p[0] | (uint32_t)p[1] << 16, crc32_ref(rom + 0x10, 6));
    const uint16_t end[4] = {0xFFFE, 0x09FF, 2, 0};
    ASSERT_EQ(cmd(HP_CMD_ROM_CRC32, end, 4), HP_ST_OK);
    static const uint16_t bad[][4] = {
        {0x0000, 0x0800, 0x0002, 0x0004},
        {0x0000, 0x0800, 0x0000, 0x0000},
        {0x0000, 0x0800, 0x0003, 0x0000},
        {0xFFFE, 0x09FF, 0x0004, 0x0000},
        {0x0000, 0x0A00, 0x0002, 0x0000},
        {0x0001, 0x0800, 0x0002, 0x0000},
        {0x0000, 0x0800, 0x0000, 0x0100},
    };
    for(size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ASSERT_EQ(cmd(HP_CMD_ROM_CRC32, bad[i], 4), HP_ST_BAD_ARG);
    }
}

int main(void) {
    RUN(test_hp_read_sector);
    RUN(test_hp_read_footers);
    RUN(test_hp_sector_crc32);
    RUN(test_hp_read_refused_unless_flash128);
    RUN(test_hp_save_slots);
    RUN(test_hp_save_layouts);
    RUN(test_hp_arm_ok);
    RUN(test_hp_arm_refused_matrix);
    RUN(test_hp_arm_padded_loaded);
    RUN(test_hp_arm_unknown_layout);
    RUN(test_hp_arm_bad_args_end_arming);
    RUN(test_hp_arm_refused_carts);
    RUN(test_hp_arm_refused_id_d4bf);
    RUN(test_hp_write_gating);
    RUN(test_hp_write_args);
    RUN(test_hp_write_full_path);
    RUN(test_hp_copy_sector);
    RUN(test_hp_disarm_after_14_commits);
    RUN(test_hp_write_needs_defer);
    RUN(test_hp_commit_needs_program);
    RUN(test_hp_cart_info_disarms);
    RUN(test_hp_cart_info_reprobes);
    RUN(test_hp_completed_forgets);
    RUN(test_hp_complete_needs_clear_slot);
    RUN(test_hp_arm_clears_erased_ok);
    RUN(test_hp_hello_disarms);
    RUN(test_hp_flash_error_disarms);
    RUN(test_hp_mode1_blank);
    RUN(test_hp_dedupe_write_resend);
    RUN(test_hp_dedupe_seq);
    RUN(test_hp_dedupe_rules);
    RUN(test_hp_rom_read);
    RUN(test_hp_rom_crc32);
    TEST_EXIT();
}
