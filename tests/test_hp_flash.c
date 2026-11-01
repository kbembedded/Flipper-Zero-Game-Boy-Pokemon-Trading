#include "test.h"
#include "fake_gba.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <hp_core.h>
#include <hp_flash.h>

/* The helper's save-chip code on the fake GBA: the probe (multiboot-and-save.md §3.3 steps 2-3,
 * §3.5) and the 128 KB flash routines (multiboot-and-save.md §3.4, §4.8), against the fake's
 * JEDEC state machine. */

static FakeGba gba;
static HpFlash fl; /* the session's flash state, as Hp keeps it */
static Hp hp;
static uint8_t img[HP_FLASH_SECTOR_LEN];
static uint8_t before[FAKE_GBA_FLASH_LEN];

static const uint8_t code_axve[4] = {'A', 'X', 'V', 'E'};
static const uint8_t code_axpd[4] = {'A', 'X', 'P', 'D'};

static void setup(FakeSaveKind kind, uint16_t id) {
    fake_gba_init(&gba);
    fake_gba_save(&gba, kind, id);
    memset(&fl, 0, sizeof(fl));
}

static uint8_t* sector(uint8_t n) {
    return fake_gba_flash(&gba) + (uint32_t)n * HP_FLASH_SECTOR_LEN;
}

static bool all_are(const uint8_t* p, uint32_t n, uint8_t v) {
    for(uint32_t i = 0; i < n; i++) {
        if(p[i] != v) return false;
    }
    return true;
}

/* Every byte of the chip but those of one sector is as it was before. */
static bool others_unchanged(uint8_t n) {
    const uint32_t lo = (uint32_t)n * HP_FLASH_SECTOR_LEN;
    const uint32_t hi = lo + HP_FLASH_SECTOR_LEN;
    return memcmp(gba.flash, before, lo) == 0 &&
           memcmp(gba.flash + hi, before + hi, FAKE_GBA_FLASH_LEN - hi) == 0;
}

static void fill_pattern(void) {
    for(uint32_t i = 0; i < FAKE_GBA_FLASH_LEN; i++) gba.flash[i] = (uint8_t)(i * 13u + 5u);
}

/* img[i] = i * 7, with the signature 0x08012025 at 0xFF8, as a saved sector carries it. */
static void make_img(void) {
    for(uint32_t i = 0; i < HP_FLASH_SECTOR_LEN; i++) img[i] = (uint8_t)(i * 7u);
    img[0xFF8] = 0x25;
    img[0xFF9] = 0x20;
    img[0xFFA] = 0x01;
    img[0xFFB] = 0x08;
}

static FakeGbaCounts counts(void) {
    FakeGbaCounts c;
    fake_gba_counts(&gba, &c);
    return c;
}

static bool op_is(uint32_t i, FakeGbaOpKind kind, uint16_t off, uint8_t val) {
    const FakeGbaOp* op = &gba.trace[i];
    return op->kind == kind && op->off == off && (kind == FakeGbaOpDelay || op->val == val);
}

TEST(test_hp_probe_mx) {
    setup(FakeSaveFlash, 0x09C2);
    gba.flash[0] = 0x12;
    gba.flash[1] = 0x34;
    memcpy(before, gba.flash, sizeof(before));
    HpProbe p;
    hp_flash_probe(&p, code_axve, 1);
    ASSERT_EQ(p.cls, HpChipFlash128);
    ASSERT_EQ(p.id, 0x09C2);
    ASSERT_EQ(p.pre0, 0x12);
    ASSERT_EQ(p.pre1, 0x34);
    ASSERT_EQ(p.id0, 0xC2);
    ASSERT_EQ(p.id1, 0x09);
    ASSERT_TRUE(!p.sram);
    ASSERT_EQ(hp_hal_flash_read(0), 0x12); /* back in read mode */
    ASSERT_EQ(hp_hal_flash_read(1), 0x34);
    const FakeGbaCounts c = counts();
    ASSERT_EQ(c.chip_erases, 0);
    ASSERT_EQ(c.id_entries, 1);
    ASSERT_EQ(c.erases, 0);
    ASSERT_EQ(c.programs, 0);
    ASSERT_EQ(c.bank_selects, 0);
    ASSERT_MEM_EQ(gba.flash, before, sizeof(before));
}

/* The bus, word for word: the SRAM probe's one lone write, the array bytes, then pret's ReadFlashId
 * (pokeruby: src/libs/agb_flash.c: ReadFlashId): enter ID mode, delay, device then maker, leave
 * with F0 twice, delay. */
TEST(test_hp_probe_sequence) {
    setup(FakeSaveFlash, 0x09C2);
    gba.flash[0] = 0x12;
    gba.flash[1] = 0x34;
    gba.flash[0x5555] = 0x3C;
    gba.trace_len = 0;
    HpProbe p;
    hp_flash_probe(&p, code_axve, 1);
    ASSERT_EQ(gba.trace_len, 16);
    ASSERT_TRUE(op_is(0, FakeGbaOpRead, 0x5555, 0x3C));
    ASSERT_TRUE(op_is(1, FakeGbaOpWrite, 0x5555, 0xC3)); /* 0x3C ^ 0xFF, ignored by the chip */
    ASSERT_TRUE(op_is(2, FakeGbaOpRead, 0x5555, 0x3C));
    ASSERT_TRUE(op_is(3, FakeGbaOpRead, 0x0000, 0x12));
    ASSERT_TRUE(op_is(4, FakeGbaOpRead, 0x0001, 0x34));
    ASSERT_TRUE(op_is(5, FakeGbaOpWrite, 0x5555, 0xAA));
    ASSERT_TRUE(op_is(6, FakeGbaOpWrite, 0x2AAA, 0x55));
    ASSERT_TRUE(op_is(7, FakeGbaOpWrite, 0x5555, 0x90));
    ASSERT_TRUE(op_is(8, FakeGbaOpDelay, 0, 0));
    ASSERT_TRUE(op_is(9, FakeGbaOpRead, 0x0001, 0x09));
    ASSERT_TRUE(op_is(10, FakeGbaOpRead, 0x0000, 0xC2));
    ASSERT_TRUE(op_is(11, FakeGbaOpWrite, 0x5555, 0xAA));
    ASSERT_TRUE(op_is(12, FakeGbaOpWrite, 0x2AAA, 0x55));
    ASSERT_TRUE(op_is(13, FakeGbaOpWrite, 0x5555, 0xF0));
    ASSERT_TRUE(op_is(14, FakeGbaOpWrite, 0x5555, 0xF0));
    ASSERT_TRUE(op_is(15, FakeGbaOpDelay, 0, 0));
    ASSERT_EQ(p.cls, HpChipFlash128);
}

/* The lone write is never 0xAA, which would start a command sequence: with 0x55 at 0x5555 it
 * writes 0x5A instead of 0xAA. With 0x0F there it writes 0xF0, a reset, which changes nothing. */
TEST(test_hp_probe_lone_write) {
    static const uint8_t at5555[] = {0x55, 0x0F, 0x00, 0xFF, 0xAA};
    for(size_t i = 0; i < sizeof(at5555); i++) {
        setup(FakeSaveFlash, 0x09C2);
        gba.flash[0x5555] = at5555[i];
        memcpy(before, gba.flash, sizeof(before));
        gba.trace_len = 0;
        HpProbe p;
        hp_flash_probe(&p, code_axve, 1);
        ASSERT_TRUE(gba.trace[1].kind == FakeGbaOpWrite && gba.trace[1].off == 0x5555);
        ASSERT_TRUE(gba.trace[1].val != 0xAA && gba.trace[1].val != at5555[i]);
        ASSERT_EQ(p.cls, HpChipFlash128);
        ASSERT_MEM_EQ(gba.flash, before, sizeof(before));
        ASSERT_EQ(counts().id_entries, 1);
    }
}

TEST(test_hp_probe_sanyo) {
    setup(FakeSaveFlash, 0x1362);
    HpProbe p;
    hp_flash_probe(&p, code_axve, 1);
    ASSERT_EQ(p.cls, HpChipFlash128);
    ASSERT_EQ(p.id, 0x1362);
    ASSERT_EQ(p.id0, 0x62);
    ASSERT_EQ(p.id1, 0x13);
    ASSERT_EQ(p.pre0, 0xFF);
    ASSERT_EQ(p.pre1, 0xFF);
    ASSERT_TRUE(!p.sram);
    ASSERT_EQ(counts().chip_erases, 0);
}

/* An SRAM reproduction: the probe's write lands, so it is put back and no flash command is ever
 * sent. Every byte of the SRAM is as it was, whatever 0x5555 held. */
TEST(test_hp_probe_sram_repro) {
    static const uint8_t at5555[] = {0x3C, 0x55, 0x0F, 0xAA, 0x00, 0xFF};
    for(size_t i = 0; i < sizeof(at5555); i++) {
        setup(FakeSaveSram, 0);
        for(uint32_t k = 0; k < FAKE_GBA_SRAM_LEN; k++) gba.flash[k] = (uint8_t)(k * 29u + 1u);
        gba.flash[0x5555] = at5555[i];
        memcpy(before, gba.flash, FAKE_GBA_SRAM_LEN);
        HpProbe p;
        hp_flash_probe(&p, code_axve, 1);
        ASSERT_EQ(p.cls, HpChipSram);
        ASSERT_TRUE(p.sram);
        ASSERT_MEM_EQ(gba.flash, before, FAKE_GBA_SRAM_LEN);
        const FakeGbaCounts c = counts();
        ASSERT_EQ(c.sram_writes, 2);
        ASSERT_EQ(c.id_entries, 0);
        ASSERT_EQ(c.bank_selects, 0);
        ASSERT_EQ(c.erases, 0);
        ASSERT_EQ(c.programs, 0);
        ASSERT_EQ(c.chip_erases, 0);
    }
}

/* A chip that ignores the ID command: the "ID" bytes are the array's, even when they spell a
 * genuine ID. */
TEST(test_hp_probe_no_id_change) {
    setup(FakeSaveRomLike, 0);
    gba.flash[0] = 0xC2;
    gba.flash[1] = 0x09;
    HpProbe p;
    hp_flash_probe(&p, code_axve, 1);
    ASSERT_EQ(p.cls, HpChipNone);
    ASSERT_EQ(p.id, 0x09C2);
    ASSERT_EQ(p.pre0, 0xC2);
    ASSERT_EQ(p.pre1, 0x09);
    ASSERT_TRUE(!p.sram);
    /* a real MX chip whose array starts C2 09 can't be told from that either */
    setup(FakeSaveFlash, 0x09C2);
    gba.flash[0] = 0xC2;
    gba.flash[1] = 0x09;
    hp_flash_probe(&p, code_axve, 1);
    ASSERT_EQ(p.cls, HpChipNone);
    /* one byte changed is a change */
    setup(FakeSaveFlash, 0x09C2);
    gba.flash[0] = 0xC2;
    gba.flash[1] = 0x00;
    hp_flash_probe(&p, code_axve, 1);
    ASSERT_EQ(p.cls, HpChipFlash128);
}

TEST(test_hp_probe_64k) {
    static const struct {
        uint16_t id;
        const uint8_t* code;
        uint8_t version;
        HpChip cls;
    } cases[] = {
        {0x1CC2, code_axve, 0, HpChipFlash64},
        {0x1B32, code_axve, 0, HpChipFlash64},
        {0x3D1F, code_axve, 0, HpChipFlash64},
        {0xD4BF, code_axve, 0, HpChipFlash64},
        {0xD4BF, code_axpd, 0, HpChipFlash128}, /* German Sapphire version 0 */
        {0xD4BF, code_axpd, 1, HpChipFlash64},
        {0x09C2, code_axpd, 0, HpChipFlash128},
        {0x1234, code_axve, 0, HpChipNone},
        {0xC209, code_axve, 0, HpChipNone}, /* the bytes the wrong way round */
    };
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        setup(FakeSaveFlash, cases[i].id);
        HpProbe p;
        hp_flash_probe(&p, cases[i].code, cases[i].version);
        ASSERT_EQ(p.cls, cases[i].cls);
        ASSERT_EQ(p.id, cases[i].id);
        ASSERT_TRUE(!p.sram);
    }
}

TEST(test_hp_probe_open_bus) {
    setup(FakeSaveNone, 0);
    HpProbe p;
    hp_flash_probe(&p, code_axve, 1);
    ASSERT_EQ(p.cls, HpChipNone);
    ASSERT_TRUE(!p.sram);
    ASSERT_EQ(counts().erases, 0);
    ASSERT_EQ(counts().bank_selects, 0);
}

/* Sector 20 is bank 1, window 0x4000: the bank, then pret's sector erase (pokeruby:
 * src/libs/agb_flash_mx.c: EraseFlashSector_MX), polled at the sector's first byte. */
TEST(test_hp_erase) {
    setup(FakeSaveFlash, 0x09C2);
    fill_pattern();
    memset(sector(20), 0x00, HP_FLASH_SECTOR_LEN);
    memcpy(before, gba.flash, sizeof(before));
    gba.trace_len = 0;
    const HpFlashResult r = hp_flash_erase(&fl, 20);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_EQ(r.code, 0);
    ASSERT_TRUE(all_are(sector(20), HP_FLASH_SECTOR_LEN, 0xFF));
    ASSERT_TRUE(others_unchanged(20));
    ASSERT_EQ(gba.bank, 1);
    static const uint16_t offs[10] = {
        0x5555, 0x2AAA, 0x5555, 0x0000, 0x5555, 0x2AAA, 0x5555, 0x5555, 0x2AAA, 0x4000};
    static const uint8_t vals[10] = {0xAA, 0x55, 0xB0, 0x01, 0xAA, 0x55, 0x80, 0xAA, 0x55, 0x30};
    for(uint32_t i = 0; i < 10; i++) ASSERT_TRUE(op_is(i, FakeGbaOpWrite, offs[i], vals[i]));
    ASSERT_TRUE(op_is(10, FakeGbaOpRead, 0x4000, 0xFF));
    ASSERT_EQ(gba.trace_len, 11);
    const FakeGbaCounts c = counts();
    ASSERT_EQ(c.erases, 1);
    ASSERT_EQ(c.bank_selects, 1);
    ASSERT_EQ(c.chip_erases, 0);
    ASSERT_EQ(gba.ops_not_red, 0);
    ASSERT_EQ(gba.status, HpStatusBusy);
    ASSERT_EQ(fl.erased_ok, 1u << 20);
}

/* A deferred write leaves 0xFF8 erased; the commit then programs 0x25 there. */
TEST(test_hp_program_defer) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    HpFlashResult r = hp_flash_program(&fl, 3, img, true);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_MEM_EQ(sector(3), img, 0xFF8);
    ASSERT_EQ(sector(3)[0xFF8], 0xFF);
    ASSERT_MEM_EQ(sector(3) + 0xFF9, img + 0xFF9, HP_FLASH_SECTOR_LEN - 0xFF9);
    FakeGbaCounts c = counts();
    ASSERT_EQ(c.erases, 1); /* blank, but not erased by this session */
    ASSERT_EQ(c.programs, HP_FLASH_SECTOR_LEN - 1);
    ASSERT_EQ(c.commits, 0);
    ASSERT_EQ(gba.bank, 0);
    r = hp_flash_commit(&fl, 3);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_MEM_EQ(sector(3), img, HP_FLASH_SECTOR_LEN);
    c = counts();
    ASSERT_EQ(c.programs, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(c.commits, 1);
    ASSERT_EQ(c.chip_erases, 0);
    ASSERT_EQ(gba.ops_not_red, 0);
    ASSERT_EQ(gba.status, HpStatusBusy);
}

/* Sector 17 is bank 1, window 0x1000. Not erased by this session, so: the erase (the bank, then
 * pret's sector erase), the bank again, then pret's byte program (pokeruby:
 * src/libs/agb_flash_mx.c: ProgramByte) for every byte, 0xFF8 included, in ascending order. */
TEST(test_hp_program_order) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    gba.trace_no_reads = true;
    gba.trace_len = 0;
    ASSERT_EQ(hp_flash_program(&fl, 17, img, false).status, HP_ST_OK);
    static const uint16_t offs[14] = {
        0x5555,
        0x2AAA,
        0x5555,
        0x0000,
        0x5555,
        0x2AAA,
        0x5555,
        0x5555,
        0x2AAA,
        0x1000,
        0x5555,
        0x2AAA,
        0x5555,
        0x0000};
    static const uint8_t vals[14] = {
        0xAA, 0x55, 0xB0, 0x01, 0xAA, 0x55, 0x80, 0xAA, 0x55, 0x30, 0xAA, 0x55, 0xB0, 0x01};
    for(uint32_t i = 0; i < 14; i++) ASSERT_TRUE(op_is(i, FakeGbaOpWrite, offs[i], vals[i]));
    for(uint32_t i = 0; i < 12; i++) {
        ASSERT_TRUE(op_is(14 + 4 * i, FakeGbaOpWrite, 0x5555, 0xAA));
        ASSERT_TRUE(op_is(15 + 4 * i, FakeGbaOpWrite, 0x2AAA, 0x55));
        ASSERT_TRUE(op_is(16 + 4 * i, FakeGbaOpWrite, 0x5555, 0xA0));
        ASSERT_TRUE(op_is(17 + 4 * i, FakeGbaOpWrite, (uint16_t)(0x1000 + i), img[i]));
    }
    ASSERT_MEM_EQ(sector(17), img, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(counts().erases, 1);
    ASSERT_EQ(counts().commits, 1);
    ASSERT_EQ(counts().programs, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(counts().bank_selects, 2);
}

TEST(test_hp_commit_rules) {
    setup(FakeSaveFlash, 0x09C2);
    /* already committed */
    sector(4)[0xFF8] = 0x25;
    sector(4)[0xFF9] = 0x20;
    sector(4)[0xFFA] = 0x01;
    sector(4)[0xFFB] = 0x08;
    HpFlashResult r = hp_flash_commit(&fl, 4);
    ASSERT_EQ(r.status, HP_ST_NOT_ERASED);
    ASSERT_EQ(r.off, 0xFF8);
    /* no signature to complete */
    sector(5)[0xFF9] = 0x00;
    sector(5)[0xFFA] = 0x00;
    sector(5)[0xFFB] = 0x00;
    r = hp_flash_commit(&fl, 5);
    ASSERT_EQ(r.status, HP_ST_BAD_ARG);
    ASSERT_EQ(sector(5)[0xFF8], 0xFF);
    /* one wrong byte of the three is enough */
    sector(6)[0xFF9] = 0x20;
    sector(6)[0xFFA] = 0x01;
    sector(6)[0xFFB] = 0x09;
    ASSERT_EQ(hp_flash_commit(&fl, 6).status, HP_ST_BAD_ARG);
    const FakeGbaCounts c = counts();
    ASSERT_EQ(c.programs, 0);
    ASSERT_EQ(c.commits, 0);
    ASSERT_EQ(c.erases, 0);
}

/* A commit whose byte program fails reports it, and the chip is back in read mode. */
TEST(test_hp_commit_program_fail) {
    setup(FakeSaveFlash, 0x09C2);
    sector(9)[0xFF9] = 0x20;
    sector(9)[0xFFA] = 0x01;
    sector(9)[0xFFB] = 0x08;
    fake_gba_fail_program(&gba, 9, 0xFF8, 1);
    const HpFlashResult r = hp_flash_commit(&fl, 9);
    ASSERT_EQ(r.status, HP_ST_PROGRAM_FAIL);
    ASSERT_EQ(r.code, 0xA001);
    ASSERT_EQ(r.off, 0xFF8);
    ASSERT_EQ(sector(9)[0xFF8], 0xFF);
    ASSERT_EQ(gba.last_write_off, 0x5555);
    ASSERT_EQ(gba.last_write_val, 0xF0);
    ASSERT_EQ(hp_hal_flash_read(0x9FF9), 0x20);
    ASSERT_EQ(counts().programs, 1);
}

/* A write erases first unless this session erased the sector OK and hasn't programmed it since:
 * a sector that only reads blank is erased too. */
TEST(test_hp_program_erases_first) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    memset(sector(9), 0x00, HP_FLASH_SECTOR_LEN);
    HpFlashResult r = hp_flash_program(&fl, 9, img, false);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_MEM_EQ(sector(9), img, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(counts().erases, 1);
    /* blank, but not erased by this session */
    ASSERT_TRUE(all_are(sector(10), HP_FLASH_SECTOR_LEN, 0xFF));
    r = hp_flash_program(&fl, 10, img, true);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 2);
    ASSERT_EQ(sector(10)[0xFF8], 0xFF);
    ASSERT_EQ(sector(10)[0xFFF], img[0xFFF]);
    ASSERT_EQ(counts().chip_erases, 0);
    ASSERT_EQ(fl.erased_ok, 0);
}

/* The erased_ok mask: a successful erase sets the sector's bit; a write then needs no second
 * erase, and clears it; so does a commit; a failed erase or write leaves it clear; bits are per
 * sector. */
TEST(test_hp_erased_ok) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    ASSERT_EQ(hp_flash_erase(&fl, 3).status, HP_ST_OK);
    ASSERT_EQ(hp_flash_erase(&fl, 30).status, HP_ST_OK);
    ASSERT_EQ(fl.erased_ok, 1u << 3 | 1u << 30);
    ASSERT_EQ(hp_flash_program(&fl, 3, img, true).status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 2); /* no second erase */
    ASSERT_EQ(fl.erased_ok, 1u << 30);
    /* the same sector again: erased again */
    ASSERT_EQ(hp_flash_program(&fl, 3, img, true).status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 3);
    ASSERT_EQ(sector(3)[0xFF8], 0xFF);
    ASSERT_MEM_EQ(sector(3), img, 0xFF8);
    /* a commit clears the bit, even one it refuses */
    ASSERT_EQ(hp_flash_erase(&fl, 3).status, HP_ST_OK);
    ASSERT_EQ(fl.erased_ok, 1u << 3 | 1u << 30);
    ASSERT_EQ(hp_flash_commit(&fl, 3).status, HP_ST_BAD_ARG);
    ASSERT_EQ(fl.erased_ok, 1u << 30);
    /* a failed erase leaves the bit clear, though it was set */
    ASSERT_EQ(hp_flash_erase(&fl, 4).status, HP_ST_OK);
    fake_gba_fail_erase(&gba, 4, 5);
    ASSERT_EQ(hp_flash_erase(&fl, 4).status, HP_ST_ERASE_FAIL);
    ASSERT_EQ(fl.erased_ok, 1u << 30);
    /* a failed write too: its first try needs no erase, the two retries each erase */
    const uint32_t erases = counts().erases;
    fake_gba_fail_program(&gba, 30, 0x10, 3);
    ASSERT_EQ(hp_flash_program(&fl, 30, img, false).status, HP_ST_PROGRAM_FAIL);
    ASSERT_EQ(counts().erases, erases + 2);
    ASSERT_EQ(fl.erased_ok, 0);
}

/* A try that fails after this session's own erase has programmed the sector, whether its program
 * or its verify failed: the retry erases first. */
TEST(test_hp_erased_ok_retry) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    ASSERT_EQ(hp_flash_erase(&fl, 7).status, HP_ST_OK);
    fake_gba_fail_program(&gba, 7, 0x123, 1);
    HpFlashResult r = hp_flash_program(&fl, 7, img, false);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 2);
    ASSERT_MEM_EQ(sector(7), img, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(fl.erased_ok, 0);
    setup(FakeSaveFlash, 0x09C2);
    ASSERT_EQ(hp_flash_erase(&fl, 7).status, HP_ST_OK);
    fake_gba_weak_program(&gba, 7, 0x123, 1);
    r = hp_flash_program(&fl, 7, img, false);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 2);
    ASSERT_MEM_EQ(sector(7), img, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(fl.erased_ok, 0);
}

/* The mask is the session's: hp_init and HELLO clear it. */
TEST(test_hp_erased_ok_hello) {
    static uint16_t resp[HELPER_CMD_RESP_MAX];
    setup(FakeSaveFlash, 0x09C2);
    hp_init(&hp);
    ASSERT_EQ(hp.flash.erased_ok, 0);
    ASSERT_EQ(hp_flash_erase(&hp.flash, 14).status, HP_ST_OK);
    ASSERT_EQ(hp.flash.erased_ok, 1u << 14);
    ASSERT_EQ(helper_cmd_run(&hp, 1, HP_CMD_HELLO, NULL, 0, resp), HP_ST_OK);
    ASSERT_EQ(hp.flash.erased_ok, 0);
    ASSERT_EQ(hp_flash_erase(&hp.flash, 14).status, HP_ST_OK);
    hp_init(&hp);
    ASSERT_EQ(hp.flash.erased_ok, 0);
    /* so the next write erases again */
    make_img();
    ASSERT_EQ(hp_flash_program(&hp.flash, 14, img, true).status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 3);
}

/* Up to 5 tries, retried on either fail code (pokeruby: src/libs/agb_flash_mx.c:
 * EraseFlashSector_MX); the chip is reset with F0 after each failure. */
TEST(test_hp_erase_retry) {
    setup(FakeSaveFlash, 0x09C2);
    memset(sector(5), 0x00, HP_FLASH_SECTOR_LEN);
    fake_gba_fail_erase(&gba, 5, 2);
    gba.trace_no_reads = true;
    gba.trace_len = 0;
    HpFlashResult r = hp_flash_erase(&fl, 5);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 3);
    /* the bank before every try, whether or not F0 kept it: bank, erase, F0 (11 writes a try) */
    ASSERT_EQ(counts().bank_selects, 3);
    for(uint32_t t = 0; t < 3; t++) {
        ASSERT_TRUE(op_is(11 * t + 2, FakeGbaOpWrite, 0x5555, 0xB0));
        ASSERT_TRUE(op_is(11 * t + 3, FakeGbaOpWrite, 0x0000, 0x00));
        ASSERT_TRUE(op_is(11 * t + 9, FakeGbaOpWrite, 0x5000, 0x30));
        if(t < 2) ASSERT_TRUE(op_is(11 * t + 10, FakeGbaOpWrite, 0x5555, 0xF0));
    }
    ASSERT_EQ(gba.trace_len, 32);
    ASSERT_EQ(fl.erased_ok, 1u << 5);
    ASSERT_TRUE(all_are(sector(5), HP_FLASH_SECTOR_LEN, 0xFF));
    /* the fifth try is the last */
    setup(FakeSaveFlash, 0x09C2);
    memset(sector(5), 0x00, HP_FLASH_SECTOR_LEN);
    fake_gba_fail_erase(&gba, 5, 4);
    ASSERT_EQ(hp_flash_erase(&fl, 5).status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 5);
    setup(FakeSaveFlash, 0x09C2);
    memset(sector(5), 0x00, HP_FLASH_SECTOR_LEN);
    fake_gba_fail_erase(&gba, 5, 5);
    r = hp_flash_erase(&fl, 5);
    ASSERT_EQ(r.status, HP_ST_ERASE_FAIL);
    ASSERT_EQ(r.code, 0xA002);
    ASSERT_EQ(counts().erases, 5);
    ASSERT_EQ(gba.last_write_off, 0x5555);
    ASSERT_EQ(gba.last_write_val, 0xF0);
    ASSERT_TRUE(all_are(sector(5), HP_FLASH_SECTOR_LEN, 0x00));
    ASSERT_EQ(counts().bank_selects, 5);
    ASSERT_EQ(fl.erased_ok, 0);
    ASSERT_EQ(hp_hal_flash_read(0x5000), 0x00); /* read mode again */
    ASSERT_EQ(counts().chip_erases, 0);
    ASSERT_EQ(gba.status, HpStatusBusy);
}

/* A chip that never finishes: the timer ends each try with phase | 0xC000, and F0. */
TEST(test_hp_erase_timeout) {
    setup(FakeSaveFlash, 0x09C2);
    memset(sector(6), 0x00, HP_FLASH_SECTOR_LEN);
    fake_gba_power_cut_after(&gba, 0);
    const HpFlashResult r = hp_flash_erase(&fl, 6);
    ASSERT_EQ(r.status, HP_ST_ERASE_FAIL);
    ASSERT_EQ(r.code, 0xC002);
    ASSERT_EQ(counts().erases, 5);
    ASSERT_EQ(gba.last_write_off, 0x5555);
    ASSERT_EQ(gba.last_write_val, 0xF0);
    /* the last try: 2000 timer units (8 reads each) of polling, then one more read */
    ASSERT_EQ(gba.timer_reads, 2000u * 8u + 1u);
}

/* Up to 3 tries of erase, program and verify (pokeruby: src/libs/agb_flash.c:
 * ProgramFlashSectorAndVerify); a failed byte stops a try (ProgramFlashSector_MX). */
TEST(test_hp_program_fail) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    fake_gba_fail_program(&gba, 7, 0x123, 99);
    const HpFlashResult r = hp_flash_program(&fl, 7, img, false);
    ASSERT_EQ(r.status, HP_ST_PROGRAM_FAIL);
    ASSERT_EQ(r.code, 0xA001);
    ASSERT_EQ(r.off, 0x123);
    const FakeGbaCounts c = counts();
    ASSERT_EQ(c.programs, 3u * 0x124u);
    ASSERT_EQ(c.erases, 3); /* each try erases first */
    ASSERT_MEM_EQ(sector(7), img, 0x123); /* ascending: everything before the failed byte */
    ASSERT_TRUE(all_are(sector(7) + 0x123, HP_FLASH_SECTOR_LEN - 0x123, 0xFF));
    ASSERT_EQ(gba.last_write_off, 0x5555);
    ASSERT_EQ(gba.last_write_val, 0xF0);
    ASSERT_EQ(gba.status, HpStatusBusy);
}

TEST(test_hp_program_retry) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    fake_gba_fail_program(&gba, 7, 0x123, 2);
    const HpFlashResult r = hp_flash_program(&fl, 7, img, false);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_MEM_EQ(sector(7), img, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(counts().erases, 3);
}

/* A weak byte polls as written but reads back wrong: the verify finds it (pokeruby:
 * src/libs/agb_flash.c: VerifyFlashSector), and the write is tried again, 3 times in all, each try
 * erasing first. */
TEST(test_hp_program_verify_fail) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    fake_gba_weak_program(&gba, 7, 0x123, 99);
    HpFlashResult r = hp_flash_program(&fl, 7, img, false);
    ASSERT_EQ(r.status, HP_ST_VERIFY_FAIL);
    ASSERT_EQ(r.off, 0x123);
    ASSERT_EQ(r.code, 0);
    ASSERT_EQ(counts().erases, 3);
    ASSERT_EQ(counts().programs, 3u * HP_FLASH_SECTOR_LEN);
    ASSERT_TRUE(sector(7)[0x123] != img[0x123]);
    ASSERT_EQ(fl.erased_ok, 0);
    /* deferred: the same */
    setup(FakeSaveFlash, 0x09C2);
    fake_gba_weak_program(&gba, 7, 0xFFF, 99);
    r = hp_flash_program(&fl, 7, img, true);
    ASSERT_EQ(r.status, HP_ST_VERIFY_FAIL);
    ASSERT_EQ(r.off, 0xFFF);
    ASSERT_EQ(counts().erases, 3);
}

TEST(test_hp_program_verify_retry) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    fake_gba_weak_program(&gba, 7, 0x123, 1);
    const HpFlashResult r = hp_flash_program(&fl, 7, img, false);
    ASSERT_EQ(r.status, HP_ST_OK);
    ASSERT_MEM_EQ(sector(7), img, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(counts().erases, 2);
    ASSERT_EQ(counts().programs, 2u * HP_FLASH_SECTOR_LEN);
}

/* A weak commit byte: the poll sees 0x25, the check of 0xFF8..0xFFB doesn't. */
TEST(test_hp_commit_verify_fail) {
    setup(FakeSaveFlash, 0x09C2);
    sector(9)[0xFF9] = 0x20;
    sector(9)[0xFFA] = 0x01;
    sector(9)[0xFFB] = 0x08;
    fake_gba_weak_program(&gba, 9, 0xFF8, 1);
    const HpFlashResult r = hp_flash_commit(&fl, 9);
    ASSERT_EQ(r.status, HP_ST_VERIFY_FAIL);
    ASSERT_EQ(r.off, 0xFF8);
    ASSERT_TRUE(sector(9)[0xFF8] != 0x25);
    ASSERT_EQ(counts().programs, 1);
}

/* A byte program that never finishes (the power is cut) ends after pret's 10 timer units of
 * polling (mxMaxTime, phase 1), 8 fake reads each, plus the last read: 0xC001, then F0. */
TEST(test_hp_program_timeout) {
    setup(FakeSaveFlash, 0x09C2);
    sector(9)[0xFF9] = 0x20;
    sector(9)[0xFFA] = 0x01;
    sector(9)[0xFFB] = 0x08;
    fake_gba_power_cut_after(&gba, 0);
    const HpFlashResult r = hp_flash_commit(&fl, 9);
    ASSERT_EQ(r.status, HP_ST_PROGRAM_FAIL);
    ASSERT_EQ(r.code, 0xC001);
    ASSERT_EQ(r.off, 0xFF8);
    ASSERT_EQ(gba.timer_reads, 10u * 8u + 1u);
    ASSERT_EQ(gba.last_write_off, 0x5555);
    ASSERT_EQ(gba.last_write_val, 0xF0);
}

/* An erase that fails inside a write fails that try; each of the 3 tries erases up to 5 times. */
TEST(test_hp_program_erase_fail) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    memset(sector(8), 0x00, HP_FLASH_SECTOR_LEN);
    fake_gba_fail_erase(&gba, 8, 99);
    const HpFlashResult r = hp_flash_program(&fl, 8, img, true);
    ASSERT_EQ(r.status, HP_ST_ERASE_FAIL);
    ASSERT_EQ(r.code, 0xA002);
    ASSERT_EQ(counts().erases, 15);
    ASSERT_EQ(counts().programs, 0);
    ASSERT_TRUE(all_are(sector(8), HP_FLASH_SECTOR_LEN, 0x00));
}

/* An erase or program that ends just as DQ5 rises: the second read sees the data, so it succeeded
 * (pokeruby: src/libs/agb_flash_1m.c: WaitForFlashWrite_Common reads again before failing). Two
 * status reads in a row are a failure, and the erase is tried again. */
TEST(test_hp_dq5_late) {
    setup(FakeSaveFlash, 0x09C2);
    memset(sector(11), 0x00, HP_FLASH_SECTOR_LEN);
    gba.late_reads = 1;
    ASSERT_EQ(hp_flash_erase(&fl, 11).status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 1);
    ASSERT_TRUE(all_are(sector(11), HP_FLASH_SECTOR_LEN, 0xFF));
    memset(sector(12), 0x00, HP_FLASH_SECTOR_LEN);
    gba.late_reads = 2;
    ASSERT_EQ(hp_flash_erase(&fl, 12).status, HP_ST_OK);
    ASSERT_EQ(counts().erases, 3);
    sector(13)[0xFF9] = 0x20;
    sector(13)[0xFFA] = 0x01;
    sector(13)[0xFFB] = 0x08;
    gba.late_reads = 1;
    ASSERT_EQ(hp_flash_commit(&fl, 13).status, HP_ST_OK);
    ASSERT_EQ(sector(13)[0xFF8], 0x25);
    ASSERT_EQ(counts().programs, 1);
}

TEST(test_hp_crc32_sector) {
    setup(FakeSaveFlash, 0x09C2);
    memset(sector(2), 0x00, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(hp_flash_crc32(1), 0xF154670Au);
    ASSERT_EQ(hp_flash_crc32(2), 0xC71C0011u);
    ASSERT_EQ(hp_flash_crc32(32), 0);
    const FakeGbaCounts c = counts();
    ASSERT_EQ(c.erases + c.programs + c.chip_erases, 0);
}

/* The bank is selected before every access: the chip's power-on bank is unknown
 * (multiboot-and-save.md M21). */
TEST(test_hp_bank_explicit) {
    setup(FakeSaveFlash, 0x09C2);
    fill_pattern();
    uint8_t buf[32];
    gba.bank = 1;
    hp_flash_read(0, 0x100, buf, sizeof(buf));
    ASSERT_MEM_EQ(buf, gba.flash + 0x100, sizeof(buf));
    ASSERT_EQ(gba.bank, 0);
    hp_flash_read(17, 0x100, buf, sizeof(buf));
    ASSERT_MEM_EQ(buf, gba.flash + 17u * HP_FLASH_SECTOR_LEN + 0x100, sizeof(buf));
    ASSERT_EQ(gba.bank, 1);
    memset(sector(0), 0xFF, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(hp_flash_crc32(0), 0xF154670Au);
    ASSERT_EQ(gba.bank, 0);
    gba.bank = 0;
    memset(sector(31), 0x00, HP_FLASH_SECTOR_LEN);
    ASSERT_EQ(hp_flash_crc32(31), 0xC71C0011u);
    ASSERT_EQ(counts().bank_selects, 4);
}

/* A read past the sector, or of a sector over 31, reads nothing. */
TEST(test_hp_flash_read_bounds) {
    setup(FakeSaveFlash, 0x09C2);
    fill_pattern();
    uint8_t buf[16];
    memset(buf, 0xA5, sizeof(buf));
    hp_flash_read(3, 0xFF8, buf, sizeof(buf));
    ASSERT_TRUE(all_are(buf, sizeof(buf), 0xA5));
    hp_flash_read(32, 0, buf, sizeof(buf));
    ASSERT_TRUE(all_are(buf, sizeof(buf), 0xA5));
    hp_flash_read(3, 0xFF0, buf, sizeof(buf));
    ASSERT_MEM_EQ(buf, gba.flash + 3u * HP_FLASH_SECTOR_LEN + 0xFF0, sizeof(buf));
    ASSERT_EQ(counts().bank_selects, 1);
}

/* Sectors over 31 are refused before anything reaches the chip. */
TEST(test_hp_flash_bad_sector) {
    setup(FakeSaveFlash, 0x09C2);
    make_img();
    ASSERT_EQ(hp_flash_erase(&fl, 32).status, HP_ST_BAD_ARG);
    ASSERT_EQ(hp_flash_program(&fl, 32, img, false).status, HP_ST_BAD_ARG);
    ASSERT_EQ(hp_flash_commit(&fl, 32).status, HP_ST_BAD_ARG);
    ASSERT_EQ(gba.trace_len, 0);
}

int main(void) {
    RUN(test_hp_probe_mx);
    RUN(test_hp_probe_sequence);
    RUN(test_hp_probe_lone_write);
    RUN(test_hp_probe_sanyo);
    RUN(test_hp_probe_sram_repro);
    RUN(test_hp_probe_no_id_change);
    RUN(test_hp_probe_64k);
    RUN(test_hp_probe_open_bus);
    RUN(test_hp_erase);
    RUN(test_hp_program_defer);
    RUN(test_hp_program_order);
    RUN(test_hp_commit_rules);
    RUN(test_hp_commit_program_fail);
    RUN(test_hp_program_erases_first);
    RUN(test_hp_erased_ok);
    RUN(test_hp_erased_ok_retry);
    RUN(test_hp_erased_ok_hello);
    RUN(test_hp_erase_retry);
    RUN(test_hp_erase_timeout);
    RUN(test_hp_program_fail);
    RUN(test_hp_program_retry);
    RUN(test_hp_program_verify_fail);
    RUN(test_hp_program_verify_retry);
    RUN(test_hp_commit_verify_fail);
    RUN(test_hp_program_timeout);
    RUN(test_hp_program_erase_fail);
    RUN(test_hp_dq5_late);
    RUN(test_hp_crc32_sector);
    RUN(test_hp_bank_explicit);
    RUN(test_hp_flash_read_bounds);
    RUN(test_hp_flash_bad_sector);
    TEST_EXIT();
}
