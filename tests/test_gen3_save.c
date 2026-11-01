#include "test.h"
#include "fake_gba.h"
#include "fake_save.h"
#include "gen3_golden.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <hp_core.h>
#include <hp_save.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_save.h>

/* The Gen III save layout against fake_save, which is written from multiboot-and-save.md alone;
 * and, where the helper decides the same thing on its side (the section sizes, the slot the game
 * loads, the slot it agrees to keep), against the helper's own save code on the fake GBA. */

#define SECTOR_LEN 4096u
#define MX 0x09C2u
#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

static uint8_t img[FAKE_SAVE_LEN];
static Gen3SaveSectorInfo info[28];
static Gen3SaveSlots slots;
static FakeSaveSpec spec;
static FakeGba gba;
static Hp hp;
static uint8_t hdr[0xC0];
static uint16_t resp[HELPER_CMD_RESP_MAX];

static uint8_t* sector(uint32_t n) {
    return img + n * SECTOR_LEN;
}

static Gen3LinkGame link_game(FakeGame g) {
    switch(g) {
    case FakeGameRS:
        return Gen3LinkGameRS;
    case FakeGameFRLG:
        return Gen3LinkGameFRLG;
    case FakeGameE:
        return Gen3LinkGameE;
    }
    return Gen3LinkGameUnknown;
}

/* img = a save in `game` with no Pokemon, each slot present or not, with its counter and first */
static void
    build(FakeGame game, bool a, uint32_t ca, uint8_t fa, bool b, uint32_t cb, uint8_t fb) {
    spec = (FakeSaveSpec){
        .game = game,
        .slot_present = {a, b},
        .counter = {ca, cb},
        .first = {fa, fb},
        .seed = 0x5EED0008u,
        .no_mons = true,
    };
    fake_save_build(img, &spec);
}

static uint8_t phys(uint8_t slot, uint8_t id) {
    return (uint8_t)fake_save_phys(&spec, slot, id);
}

/* The 28 sector infos of img for game g, then gen3_save_analyze */
static void analyze_as(Gen3LinkGame g) {
    for(uint32_t s = 0; s < 28; s++) gen3_save_sector_info(g, sector(s), &info[s]);
    gen3_save_analyze(info, &slots);
}

static void analyze(void) {
    analyze_as(link_game(spec.game));
}

static bool phys_is_slot(uint8_t slot) {
    for(uint8_t k = 0; k < 14; k++) {
        if(slots.phys[k] != phys(slot, k)) return false;
    }
    return true;
}

static bool phys_none(void) {
    for(uint8_t k = 0; k < 14; k++) {
        if(slots.phys[k] != GEN3_SAVE_NO_SECTOR) return false;
    }
    return true;
}

/* ---- Checksum, sizes, footers ---- */

TEST(test_save_checksum) {
    static uint8_t d[0xF80];
    for(uint32_t i = 0; i < sizeof(d); i++) d[i] = (uint8_t)i;
    ASSERT_EQ(gen3_save_checksum(d, 0xF80), 0xCA26);
    ASSERT_EQ(gen3_save_checksum(d, 0x890), 0x5101);
    ASSERT_EQ(gen3_save_checksum(d, 0xF24), 0xFF90);
    memset(d, 0xFF, sizeof(d));
    ASSERT_EQ(gen3_save_checksum(d, 0xF80), 0xFC1F);
    ASSERT_EQ(gen3_save_checksum(d, 0x7D0), 0xFE0B);
    memset(d, 0, sizeof(d));
    ASSERT_EQ(gen3_save_checksum(d, 0xF80), 0);
    static const uint8_t w[17] = {
        0x00,
        0x78,
        0x56,
        0x34,
        0x12,
        0xF0,
        0xDE,
        0xBC,
        0x9A,
        0x0F,
        0x0F,
        0x0F,
        0x0F,
        0xFF,
        0xFF,
        0xFF,
        0xFF};
    /* little-endian words 12345678 9ABCDEF0 0F0F0F0F FFFFFFFF, from an odd address */
    ASSERT_EQ(gen3_save_checksum(w + 1, 16), 0x0076);
    /* only whole words count, as the game sums size / 4 of them */
    ASSERT_EQ(gen3_save_checksum(w + 1, 15), gen3_save_checksum(w + 1, 12));
    ASSERT_EQ(gen3_save_checksum(w + 1, 3), 0);
}

TEST(test_section_sizes) {
    ASSERT_EQ(gen3_save_section_size(Gen3LinkGameRS, 0), 0x890);
    ASSERT_EQ(gen3_save_section_size(Gen3LinkGameRS, 4), 0xC40);
    ASSERT_EQ(gen3_save_section_size(Gen3LinkGameFRLG, 0), 0xF24);
    ASSERT_EQ(gen3_save_section_size(Gen3LinkGameFRLG, 4), 0xEE8);
    ASSERT_EQ(gen3_save_section_size(Gen3LinkGameE, 0), 0xF2C);
    ASSERT_EQ(gen3_save_section_size(Gen3LinkGameE, 4), 0xF08);
    static const Gen3LinkGame games[3] = {Gen3LinkGameRS, Gen3LinkGameFRLG, Gen3LinkGameE};
    for(uint32_t g = 0; g < 3; g++) {
        for(uint16_t id = 1; id <= 12; id++) {
            if(id != 4) ASSERT_EQ(gen3_save_section_size(games[g], id), 0xF80);
        }
        ASSERT_EQ(gen3_save_section_size(games[g], 13), 0x7D0);
        ASSERT_EQ(gen3_save_section_size(games[g], 14), 0);
        ASSERT_EQ(gen3_save_section_size(games[g], 0xFFFF), 0);
    }
    for(uint16_t id = 0; id < 16; id++) {
        ASSERT_EQ(gen3_save_section_size(Gen3LinkGameUnknown, id), 0);
    }
}

/* The same table as fake_save's (from the doc) and the helper's (which picks the slot to keep with
 * it) */
TEST(test_section_sizes_match_fake_and_helper) {
    for(uint8_t id = 0; id < 14; id++) {
        ASSERT_EQ(gen3_save_section_size(Gen3LinkGameRS, id), fake_save_size(FakeGameRS, id));
        ASSERT_EQ(gen3_save_section_size(Gen3LinkGameFRLG, id), fake_save_size(FakeGameFRLG, id));
        ASSERT_EQ(gen3_save_section_size(Gen3LinkGameE, id), fake_save_size(FakeGameE, id));
    }
    for(uint16_t id = 0; id < 16; id++) {
        ASSERT_EQ(gen3_save_section_size(Gen3LinkGameRS, id), hp_save_size(HpSaveGameRS, id));
        ASSERT_EQ(gen3_save_section_size(Gen3LinkGameFRLG, id), hp_save_size(HpSaveGameFRLG, id));
        ASSERT_EQ(
            gen3_save_section_size(Gen3LinkGameUnknown, id), hp_save_size(HpSaveGameUnknown, id));
    }
}

TEST(test_footer_roundtrip) {
    static uint8_t s[SECTOR_LEN];
    memset(s, 0xAA, sizeof(s));
    const Gen3SaveFooter f = {
        .id = 7,
        .checksum = 0xBEEF,
        .signature = 0x08012025u,
        .counter = 0x01020304u,
    };
    gen3_save_footer_set(s, &f);
    static const uint8_t want[12] = {
        0x07, 0x00, 0xEF, 0xBE, 0x25, 0x20, 0x01, 0x08, 0x04, 0x03, 0x02, 0x01};
    ASSERT_MEM_EQ(s + 0xFF4, want, sizeof(want));
    for(uint32_t i = 0; i < 0xFF4; i++) ASSERT_EQ(s[i], 0xAA);
    Gen3SaveFooter g;
    gen3_save_footer_get(s, &g);
    ASSERT_EQ(g.id, 7);
    ASSERT_EQ(g.checksum, 0xBEEF);
    ASSERT_EQ(g.signature, 0x08012025u);
    ASSERT_EQ(g.counter, 0x01020304u);
}

TEST(test_sector_info) {
    build(FakeGameRS, true, 4, 0, false, 0, 0);
    Gen3SaveSectorInfo i;
    gen3_save_sector_info(Gen3LinkGameRS, sector(phys(0, 4)), &i);
    ASSERT_TRUE(i.valid);
    ASSERT_TRUE(i.full_ok);
    ASSERT_EQ(i.f.id, 4);
    ASSERT_EQ(i.f.signature, GEN3_SAVE_SIGNATURE);
    ASSERT_EQ(i.f.counter, 4);
    ASSERT_EQ(i.f.checksum, gen3_save_checksum(sector(phys(0, 4)), 0xC40));
    /* Unknown has no sizes: nothing is valid */
    gen3_save_sector_info(Gen3LinkGameUnknown, sector(phys(0, 4)), &i);
    ASSERT_FALSE(i.valid);
    ASSERT_TRUE(i.full_ok);
    /* a signature off by one bit */
    sector(phys(0, 4))[0xFF8] ^= 1;
    gen3_save_sector_info(Gen3LinkGameRS, sector(phys(0, 4)), &i);
    ASSERT_FALSE(i.valid);
    ASSERT_TRUE(i.full_ok);
    /* erased: the stored checksum 0xFFFF isn't the full one */
    gen3_save_sector_info(Gen3LinkGameRS, sector(28), &i);
    ASSERT_FALSE(i.valid);
    ASSERT_FALSE(i.full_ok);
    ASSERT_EQ(i.f.id, 0xFFFF);
}

/* ---- The slot the game loads ---- */

TEST(test_slots_both_ok_b_newer) {
    build(FakeGameRS, true, 4, 0, true, 5, 3);
    analyze();
    ASSERT_EQ(slots.slot[0], Gen3SlotOk);
    ASSERT_EQ(slots.slot[1], Gen3SlotOk);
    ASSERT_EQ(slots.counter[0], 4);
    ASSERT_EQ(slots.counter[1], 5);
    ASSERT_EQ(slots.result, Gen3SaveOk);
    ASSERT_EQ(slots.save_counter, 5);
    ASSERT_EQ(slots.active, 1);
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_EQ(slots.first, 3);
    ASSERT_EQ(slots.phys[0], 17);
    ASSERT_EQ(slots.phys[13], 16);
    ASSERT_TRUE(phys_is_slot(1));
}

TEST(test_slots_a_newer) {
    build(FakeGameRS, true, 6, 9, true, 5, 3);
    analyze();
    ASSERT_EQ(slots.result, Gen3SaveOk);
    ASSERT_EQ(slots.save_counter, 6);
    ASSERT_EQ(slots.active, 0);
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_EQ(slots.first, 9);
    ASSERT_TRUE(phys_is_slot(0));
}

/* GetSaveValidStatus: 0 is newer than 0xFFFFFFFF */
TEST(test_slots_wrap) {
    build(FakeGameRS, true, 0, 2, true, 0xFFFFFFFFu, 7);
    analyze();
    ASSERT_EQ(slots.result, Gen3SaveOk);
    ASSERT_EQ(slots.save_counter, 0);
    ASSERT_EQ(slots.active, 0);
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_TRUE(phys_is_slot(0));
}

/* The newer counter, 0, is in slot B but even: the game loads slot A, whose counters aren't 0 */
TEST(test_slots_wrap_odd) {
    build(FakeGameRS, true, 0xFFFFFFFFu, 2, true, 0, 7);
    analyze();
    ASSERT_EQ(slots.result, Gen3SaveOk);
    ASSERT_EQ(slots.save_counter, 0);
    ASSERT_EQ(slots.active, 0);
    ASSERT_FALSE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_TRUE(phys_is_slot(0));
}

TEST(test_slots_one_bad) {
    build(FakeGameRS, true, 4, 0, true, 5, 3);
    fake_save_break_checksum(img, phys(1, 6));
    analyze();
    ASSERT_EQ(slots.slot[0], Gen3SlotOk);
    ASSERT_EQ(slots.slot[1], Gen3SlotError);
    ASSERT_EQ(slots.result, Gen3SaveError);
    ASSERT_EQ(slots.save_counter, 4);
    ASSERT_EQ(slots.active, 0);
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_TRUE(phys_is_slot(0));
}

TEST(test_slots_missing_id) {
    build(FakeGameRS, true, 4, 0, false, 0, 0);
    fake_save_set_id(img, phys(0, 4), 3);
    fake_save_fix(img, phys(0, 4), FakeGameRS);
    analyze();
    ASSERT_TRUE(info[phys(0, 4)].valid); /* a valid id 3, now twice */
    ASSERT_EQ(slots.slot[0], Gen3SlotError);
    ASSERT_EQ(slots.slot[1], Gen3SlotEmpty);
    ASSERT_EQ(slots.result, Gen3SaveCorrupt);
    ASSERT_EQ(slots.save_counter, 0);
    ASSERT_FALSE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_TRUE(phys_none());
}

TEST(test_slots_empty) {
    memset(img, 0xFF, sizeof(img));
    analyze_as(Gen3LinkGameRS);
    ASSERT_EQ(slots.slot[0], Gen3SlotEmpty);
    ASSERT_EQ(slots.slot[1], Gen3SlotEmpty);
    ASSERT_EQ(slots.result, Gen3SaveEmpty);
    ASSERT_EQ(slots.save_counter, 0);
    ASSERT_EQ(slots.active, 0);
    ASSERT_EQ(slots.first, 0);
    ASSERT_FALSE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_TRUE(phys_none());
}

TEST(test_slots_corrupt) {
    build(FakeGameRS, true, 4, 0, true, 5, 3);
    fake_save_break_checksum(img, phys(0, 0));
    fake_save_break_checksum(img, phys(1, 13));
    analyze();
    ASSERT_EQ(slots.slot[0], Gen3SlotError);
    ASSERT_EQ(slots.slot[1], Gen3SlotError);
    ASSERT_EQ(slots.result, Gen3SaveCorrupt);
    ASSERT_EQ(slots.save_counter, 0);
    ASSERT_EQ(slots.active, 0);
    ASSERT_EQ(slots.first, 0);
    ASSERT_FALSE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_TRUE(phys_none());
}

/* The game takes the counter of the slot's last valid sector in physical order */
TEST(test_slots_counter_mismatch) {
    build(FakeGameRS, true, 4, 0, false, 0, 0);
    fake_save_set_counter(img, 13, 2);
    analyze();
    ASSERT_EQ(slots.slot[0], Gen3SlotOk);
    ASSERT_EQ(slots.counter[0], 2);
    ASSERT_EQ(slots.result, Gen3SaveOk);
    ASSERT_EQ(slots.save_counter, 2);
    ASSERT_EQ(slots.active, 0);
    ASSERT_FALSE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_TRUE(phys_is_slot(0));
    /* the other way round: a stray counter first in physical order is overwritten */
    build(FakeGameRS, true, 4, 0, false, 0, 0);
    fake_save_set_counter(img, 0, 2);
    analyze();
    ASSERT_EQ(slots.counter[0], 4);
    ASSERT_EQ(slots.save_counter, 4);
    ASSERT_FALSE(slots.strict_ok);
}

/* An id over 13 is never valid, whatever its checksum: the game reads its size past its table */
TEST(test_slots_bad_id) {
    static const uint16_t ids[3] = {14, 20, 0xFFFF};
    for(uint32_t i = 0; i < 3; i++) {
        build(FakeGameRS, true, 4, 0, false, 0, 0);
        fake_save_set_id(img, phys(0, 7), ids[i]);
        fake_save_fix(img, phys(0, 7), FakeGameRS);
        analyze();
        ASSERT_FALSE(info[phys(0, 7)].valid);
        ASSERT_TRUE(info[phys(0, 7)].full_ok);
        ASSERT_EQ(slots.slot[0], Gen3SlotError);
        ASSERT_EQ(slots.result, Gen3SaveCorrupt);
        ASSERT_FALSE(slots.strict_ok);
        ASSERT_FALSE(slots.padded);
    }
    /* infos made by hand, valid with an id over 13, in the slot the game loads (B: slot A's
     * counter 5 is odd): never taken, and nothing is written past phys (ASan, UBSan) */
    build(FakeGameRS, true, 5, 0, true, 5, 0);
    analyze();
    ASSERT_EQ(slots.active, 1);
    ASSERT_TRUE(slots.strict_ok);
    static const uint16_t big[2] = {200, 31};
    for(uint32_t i = 0; i < 2; i++) {
        info[phys(1, 7)].f.id = big[i];
        gen3_save_analyze(info, &slots);
        ASSERT_EQ(slots.slot[1], Gen3SlotError);
        ASSERT_EQ(slots.result, Gen3SaveError);
        ASSERT_EQ(slots.active, 1);
        ASSERT_FALSE(slots.strict_ok);
        ASSERT_EQ(slots.phys[7], GEN3_SAVE_NO_SECTOR);
        ASSERT_EQ(slots.phys[6], phys(1, 6));
    }
}

/* A valid signature makes a slot non-empty, even with no valid sector */
TEST(test_slots_sig_only) {
    build(FakeGameRS, true, 4, 0, false, 0, 0);
    for(uint8_t s = 0; s < 14; s++) fake_save_break_checksum(img, s);
    analyze();
    ASSERT_EQ(slots.slot[0], Gen3SlotError);
    ASSERT_EQ(slots.counter[0], 0);
    ASSERT_EQ(slots.result, Gen3SaveCorrupt);
    ASSERT_FALSE(slots.padded);
    /* one erased sector with only its signature written */
    memset(img, 0xFF, sizeof(img));
    static const uint8_t sig[4] = {0x25, 0x20, 0x01, 0x08};
    memcpy(sector(20) + 0xFF8, sig, sizeof(sig));
    analyze_as(Gen3LinkGameRS);
    ASSERT_EQ(slots.slot[0], Gen3SlotEmpty);
    ASSERT_EQ(slots.slot[1], Gen3SlotError);
    ASSERT_EQ(slots.result, Gen3SaveCorrupt);
    ASSERT_FALSE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
}

/* The game writes even counters to slot A: a slot A with an odd one sends the game to slot B */
TEST(test_slots_parity) {
    build(FakeGameRS, true, 5, 4, false, 0, 0);
    analyze();
    ASSERT_EQ(slots.slot[0], Gen3SlotOk);
    ASSERT_EQ(slots.result, Gen3SaveOk);
    ASSERT_EQ(slots.save_counter, 5);
    ASSERT_EQ(slots.active, 1);
    ASSERT_FALSE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    ASSERT_EQ(slots.first, 0);
    ASSERT_TRUE(phys_none());
    /* slot B, loaded so, has 14 valid sectors with counter 5, but id 4 is a second id 3 */
    build(FakeGameRS, true, 5, 4, true, 5, 3);
    fake_save_set_id(img, phys(1, 4), 3);
    fake_save_fix(img, phys(1, 4), FakeGameRS);
    analyze();
    ASSERT_EQ(slots.slot[1], Gen3SlotError);
    ASSERT_EQ(slots.active, 1);
    ASSERT_FALSE(slots.strict_ok);
    ASSERT_EQ(slots.phys[3], phys(1, 4)); /* the later of the two, in physical order */
    ASSERT_EQ(slots.phys[4], GEN3_SAVE_NO_SECTOR);
    ASSERT_EQ(slots.phys[5], phys(1, 5));
    Gen3SaveWritePlan p;
    ASSERT_FALSE(gen3_save_plan_write(&slots, &p));
    /* the game's first sector is wherever a footer last says id 0, valid or not */
    build(FakeGameRS, true, 5, 4, true, 5, 3);
    fake_save_set_id(img, phys(1, 4), 0);
    analyze();
    ASSERT_FALSE(info[phys(1, 4)].valid);
    ASSERT_EQ(slots.first, 7);
    ASSERT_EQ(slots.phys[0], phys(1, 0));
}

/* A loaded sector with a non-zero byte past its section: the game takes it, the helper won't keep
 * it */
TEST(test_slots_padded) {
    build(FakeGameRS, true, 4, 5, false, 0, 0);
    analyze();
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    sector(phys(0, 0))[0x890] = 0x5A;
    analyze();
    ASSERT_TRUE(info[phys(0, 0)].valid);
    ASSERT_FALSE(info[phys(0, 0)].full_ok);
    ASSERT_EQ(slots.result, Gen3SaveOk);
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_TRUE(slots.padded);
    /* the last data byte of id 13 */
    build(FakeGameRS, true, 4, 5, false, 0, 0);
    sector(phys(0, 13))[0xF7F] = 1;
    analyze();
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_TRUE(slots.padded);
    /* past the data area, which neither checksum covers */
    build(FakeGameRS, true, 4, 5, false, 0, 0);
    sector(phys(0, 13))[0xF80] = 1;
    analyze();
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    /* in the slot the game doesn't load */
    build(FakeGameRS, true, 6, 5, true, 5, 1);
    sector(phys(1, 0))[0x890] = 0x5A;
    analyze();
    ASSERT_EQ(slots.active, 0);
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
}

/* The header names the layout; a save is never read with another game's */
TEST(test_save_layout_mismatch) {
    build(FakeGameFRLG, true, 4, 0, false, 0, 0);
    analyze_as(Gen3LinkGameRS);
    ASSERT_FALSE(info[phys(0, 0)].valid);
    ASSERT_FALSE(info[phys(0, 4)].valid);
    ASSERT_TRUE(info[phys(0, 1)].valid);
    ASSERT_EQ(slots.slot[0], Gen3SlotError);
    ASSERT_FALSE(slots.strict_ok);
    analyze_as(Gen3LinkGameFRLG);
    ASSERT_TRUE(slots.strict_ok);
    ASSERT_FALSE(slots.padded);
    /* Emerald's are longer still */
    build(FakeGameE, true, 4, 0, false, 0, 0);
    analyze_as(Gen3LinkGameFRLG);
    ASSERT_FALSE(info[phys(0, 0)].valid);
    ASSERT_FALSE(slots.strict_ok);
    analyze_as(Gen3LinkGameE);
    ASSERT_TRUE(slots.strict_ok);
    /* no layout */
    analyze_as(Gen3LinkGameUnknown);
    ASSERT_EQ(slots.slot[0], Gen3SlotError);
    ASSERT_EQ(slots.result, Gen3SaveCorrupt);
}

/* The helper on the fake GBA picks the same slot over the same image, with the same full-length
 * checksums, and its ARM_WRITE accepts a write over the other slot exactly when the Flipper would
 * plan one (strict and not padded). */
typedef struct {
    FakeGame game; /* the image's */
    FakeGame as; /* the layout both sides read it with */
    bool a, b;
    uint32_t ca, cb;
    uint8_t edit; /* 0 none, 1 break B's id 6, 2 id 4 relabelled 3, 3 counter of sector 13 = 2,
                   * 4 id 20, 5 pad id 0 of slot A, 6 break A's id 0 and B's id 13 */
} SlotCase;

static void slot_case_build(const SlotCase* c) {
    build(c->game, c->a, c->ca, 3, c->b, c->cb, 11);
    switch(c->edit) {
    case 1:
        fake_save_break_checksum(img, phys(1, 6));
        break;
    case 2:
        fake_save_set_id(img, phys(0, 4), 3);
        fake_save_fix(img, phys(0, 4), c->game);
        break;
    case 3:
        fake_save_set_counter(img, 13, 2);
        break;
    case 4:
        fake_save_set_id(img, phys(0, 7), 20);
        fake_save_fix(img, phys(0, 7), c->game);
        break;
    case 5:
        sector(phys(0, 0))[0x890] = 0x5A;
        break;
    case 6:
        fake_save_break_checksum(img, phys(0, 0));
        fake_save_break_checksum(img, phys(1, 13));
        break;
    default:
        break;
    }
}

TEST(test_slots_match_helper) {
    static const SlotCase cases[] = {
        {FakeGameRS, FakeGameRS, true, true, 4, 5, 0},
        {FakeGameRS, FakeGameRS, true, true, 6, 5, 0},
        {FakeGameRS, FakeGameRS, true, true, 0, 0xFFFFFFFFu, 0},
        {FakeGameRS, FakeGameRS, true, true, 0xFFFFFFFFu, 0, 0},
        {FakeGameRS, FakeGameRS, true, true, 4, 5, 1},
        {FakeGameRS, FakeGameRS, true, false, 4, 0, 0},
        {FakeGameRS, FakeGameRS, false, true, 0, 7, 0},
        {FakeGameRS, FakeGameRS, true, false, 5, 0, 0},
        {FakeGameRS, FakeGameRS, true, false, 4, 0, 2},
        {FakeGameRS, FakeGameRS, true, false, 4, 0, 3},
        {FakeGameRS, FakeGameRS, true, false, 4, 0, 4},
        {FakeGameRS, FakeGameRS, true, false, 4, 0, 5},
        {FakeGameRS, FakeGameRS, true, true, 4, 5, 6},
        {FakeGameRS, FakeGameRS, false, false, 0, 0, 0},
        {FakeGameFRLG, FakeGameFRLG, true, true, 8, 7, 0},
        {FakeGameFRLG, FakeGameFRLG, true, true, 8, 9, 1},
        {FakeGameFRLG, FakeGameRS, true, true, 8, 9, 0},
        {FakeGameRS, FakeGameFRLG, true, true, 8, 9, 0},
        {FakeGameE, FakeGameFRLG, true, false, 8, 0, 0},
    };
    uint32_t armed = 0;
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const SlotCase* c = &cases[i];
        slot_case_build(c);
        analyze_as(link_game(c->as));
        /* An English cart of the layout's game, which the helper writes */
        fake_gba_init(&gba);
        helper_cmd_header(hdr, gba.own, "POKEMON", c->as == FakeGameRS ? "AXVE" : "BPRE", 0);
        fake_gba_rom(&gba, hdr, 0x100000);
        fake_gba_rom_string(&gba, 0xB0000, "FLASH1M_V103");
        fake_gba_save(&gba, FakeSaveFlash, MX);
        memcpy(fake_gba_flash(&gba), img, sizeof(img));
        /* The real ARM_WRITE, as the Flipper would send it for its plan: over the slot the game
         * doesn't load, with the loaded slot's counter */
        hp_init(&hp);
        ASSERT_EQ(helper_cmd_run(&hp, 1, HP_CMD_CART_INFO, NULL, 0, resp), HP_ST_OK);
        ASSERT_EQ(helper_cmd_run(&hp, 2, HP_CMD_SAVE_PROBE, NULL, 0, resp), HP_ST_OK);
        const uint16_t arm[6] = {
            HP_ARM_MAGIC,
            MX,
            (uint16_t)(1u - slots.active),
            0,
            (uint16_t)slots.save_counter,
            (uint16_t)(slots.save_counter >> 16),
        };
        const uint16_t st = helper_cmd_run(&hp, 3, HP_CMD_ARM_WRITE, arm, 6, resp);
        ASSERT_EQ(st == HP_ST_OK, slots.strict_ok && !slots.padded);
        if(st != HP_ST_OK) ASSERT_EQ(st, HP_ST_REFUSED);
        armed += st == HP_ST_OK;
        const HpSaveGame hg = c->as == FakeGameRS ? HpSaveGameRS : HpSaveGameFRLG;
        const bool loads = slots.result == Gen3SaveOk || slots.result == Gen3SaveError;
        const int active = hp_save_active_slot(hg);
        ASSERT_EQ(active, loads ? slots.active : -1);
        /* ARM_WRITE mode 0 over slot 1 - active, with the Flipper's counter */
        uint32_t n = 0;
        const bool keep_ok = hp_save_slot_ok(slots.active, &n);
        const bool arms = active == slots.active && keep_ok && n == slots.save_counter &&
                          slots.save_counter % 2u == slots.active;
        ASSERT_EQ(arms, slots.strict_ok && !slots.padded);
        for(uint8_t s = 0; s < 28; s++) {
            HpFooter f;
            hp_save_footer(s, &f);
            ASSERT_EQ(f.full_checksum, gen3_save_checksum(sector(s), GEN3_SAVE_SECTOR_DATA));
            ASSERT_EQ(info[s].full_ok, f.full_checksum == f.checksum);
        }
    }
    /* both answers occur: 9 of the cases are strict and not padded (an RS save read as FRLG
     * among them, its zero padding counting in FRLG's longer sums) */
    ASSERT_EQ(armed, 9);
}

/* ---- Write plans ---- */

TEST(test_plan_write) {
    build(FakeGameRS, true, 4, 5, false, 0, 0);
    analyze();
    Gen3SaveWritePlan p;
    ASSERT_TRUE(gen3_save_plan_write(&slots, &p));
    ASSERT_EQ(p.target, 1);
    ASSERT_EQ(p.counter, 5);
    ASSERT_EQ(p.first, 6);
    ASSERT_EQ(p.dst[0], 20);
    ASSERT_EQ(p.dst[8], 14);
    ASSERT_EQ(p.dst[13], 19);
    uint32_t seen = 0;
    for(uint8_t k = 0; k < 14; k++) {
        ASSERT_EQ(p.src[k], slots.phys[k]);
        ASSERT_EQ(p.dst[k], 14 + (k + 6) % 14);
        seen |= 1u << (p.dst[k] - 14);
    }
    ASSERT_EQ(seen, 0x3FFF);
    /* B active: the new save goes to slot A */
    build(FakeGameFRLG, true, 6, 0, true, 7, 13);
    analyze();
    ASSERT_TRUE(gen3_save_plan_write(&slots, &p));
    ASSERT_EQ(p.target, 0);
    ASSERT_EQ(p.counter, 8);
    ASSERT_EQ(p.first, 0);
    for(uint8_t k = 0; k < 14; k++) {
        ASSERT_EQ(p.dst[k], k);
        ASSERT_EQ(p.src[k], phys(1, k));
    }
}

TEST(test_plan_write_wrap) {
    build(FakeGameRS, false, 0, 0, true, 0xFFFFFFFFu, 2);
    analyze();
    ASSERT_EQ(slots.active, 1);
    Gen3SaveWritePlan p;
    ASSERT_TRUE(gen3_save_plan_write(&slots, &p));
    ASSERT_EQ(p.target, 0);
    ASSERT_EQ(p.counter, 0);
    ASSERT_EQ(p.first, 3);
    ASSERT_EQ(p.dst[0], 3);
    ASSERT_EQ(p.dst[11], 0);
}

TEST(test_plan_refused) {
    build(FakeGameRS, true, 4, 0, false, 0, 0);
    fake_save_set_counter(img, 13, 2);
    analyze();
    ASSERT_FALSE(slots.strict_ok);
    Gen3SaveWritePlan p;
    memset(&p, 0xAB, sizeof(p));
    ASSERT_FALSE(gen3_save_plan_write(&slots, &p));
    const uint8_t* b = (const uint8_t*)&p;
    for(size_t i = 0; i < sizeof(p); i++) ASSERT_EQ(b[i], 0xAB);
    /* Empty and Corrupt too */
    memset(img, 0xFF, sizeof(img));
    analyze_as(Gen3LinkGameRS);
    ASSERT_FALSE(gen3_save_plan_write(&slots, &p));
}

TEST(test_plan_fresh) {
    Gen3SaveWritePlan p;
    gen3_save_plan_fresh(7, 2, &p);
    ASSERT_EQ(p.target, 1);
    ASSERT_EQ(p.counter, 7);
    ASSERT_EQ(p.first, 2);
    ASSERT_EQ(p.dst[0], 16);
    ASSERT_EQ(p.dst[12], 14);
    for(uint8_t k = 0; k < 14; k++) ASSERT_EQ(p.src[k], GEN3_SAVE_NO_SECTOR);
    gen3_save_plan_fresh(0xFFFFFFFEu, 16, &p);
    ASSERT_EQ(p.target, 0);
    ASSERT_EQ(p.first, 2);
    ASSERT_EQ(p.dst[0], 2);
    ASSERT_EQ(p.dst[13], 1);
}

/* ---- Where things sit ---- */

static bool span_is(const Gen3SaveSpan* s, uint8_t id, uint16_t off, uint8_t len) {
    return s->id == id && s->off == off && s->len == len;
}

TEST(test_mon_spans) {
    Gen3SaveSpan sp[2];
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameRS, 0, 0, sp), 1);
    ASSERT_TRUE(span_is(&sp[0], 5, 4, 80));
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameRS, 1, 19, sp), 2);
    ASSERT_TRUE(span_is(&sp[0], 5, 0xF54, 44));
    ASSERT_TRUE(span_is(&sp[1], 6, 0, 36));
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameRS, 3, 9, sp), 2);
    ASSERT_TRUE(span_is(&sp[0], 6, 0xF74, 12));
    ASSERT_TRUE(span_is(&sp[1], 7, 0, 68));
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameRS, 13, 29, sp), 1);
    ASSERT_TRUE(span_is(&sp[0], 13, 0x6F4, 80));
    /* multiboot-and-save.md §5.6's eight straddlers, and no other */
    static const uint16_t straddlers[8] = {49, 99, 148, 198, 247, 297, 347, 396};
    static const uint8_t first_len[8] = {44, 12, 60, 28, 76, 44, 12, 60};
    uint32_t found = 0;
    for(uint16_t i = 0; i < 420; i++) {
        const uint8_t n = gen3_save_mon_spans(Gen3LinkGameFRLG, i / 30, i % 30, sp);
        if(n == 1) continue;
        ASSERT_EQ(n, 2);
        ASSERT_TRUE(found < 8);
        ASSERT_EQ(i, straddlers[found]);
        ASSERT_EQ(sp[0].len, first_len[found]);
        ASSERT_EQ(sp[0].off + sp[0].len, 0xF80);
        ASSERT_TRUE(span_is(&sp[1], sp[0].id + 1, 0, 80 - first_len[found]));
        found++;
    }
    ASSERT_EQ(found, 8);
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameRS, GEN3_SAVE_PARTY, 2, sp), 1);
    ASSERT_TRUE(span_is(&sp[0], 1, 0x300, 100));
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameE, GEN3_SAVE_PARTY, 5, sp), 1);
    ASSERT_TRUE(span_is(&sp[0], 1, 0x238 + 500, 100));
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameFRLG, GEN3_SAVE_PARTY, 0, sp), 1);
    ASSERT_TRUE(span_is(&sp[0], 1, 0x38, 100));
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameRS, 14, 0, sp), 0);
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameRS, 0, 30, sp), 0);
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameRS, GEN3_SAVE_PARTY, 6, sp), 0);
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameUnknown, 0, 0, sp), 0);
    ASSERT_EQ(gen3_save_mon_spans(Gen3LinkGameUnknown, GEN3_SAVE_PARTY, 0, sp), 0);
}

TEST(test_count_and_name_spans) {
    Gen3SaveSpan s = gen3_save_party_count_span(Gen3LinkGameRS);
    ASSERT_TRUE(span_is(&s, 1, 0x234, 1));
    s = gen3_save_party_count_span(Gen3LinkGameE);
    ASSERT_TRUE(span_is(&s, 1, 0x234, 1));
    s = gen3_save_party_count_span(Gen3LinkGameFRLG);
    ASSERT_TRUE(span_is(&s, 1, 0x34, 1));
    s = gen3_save_party_count_span(Gen3LinkGameUnknown);
    ASSERT_EQ(s.len, 0);
    s = gen3_save_box_name_span(0);
    ASSERT_TRUE(span_is(&s, 13, 0x744, 9));
    s = gen3_save_box_name_span(13);
    ASSERT_TRUE(span_is(&s, 13, 0x7B9, 9));
    s = gen3_save_box_name_span(14);
    ASSERT_EQ(s.len, 0);
}

/* len bytes read through the spans out of img's slot the game loads */
static void read_spans(const Gen3SaveSpan* sp, uint8_t n, uint8_t* out) {
    for(uint8_t i = 0; i < n; i++) {
        memcpy(out, sector(slots.phys[sp[i].id]) + sp[i].off, sp[i].len);
        out += sp[i].len;
    }
}

/* Every party slot, box slot, box name and the count, placed by fake_save and read back through
 * the spans; the save stays strict. */
TEST(test_spans_match_fake) {
    static const FakeGame games[3] = {FakeGameRS, FakeGameFRLG, FakeGameE};
    static uint8_t party[6][100];
    uint8_t mon[100];
    uint8_t got[100];
    Gen3SaveSpan sp[2];
    for(uint32_t g = 0; g < 3; g++) {
        build(games[g], true, 6, 4, true, 7, 9);
        for(uint16_t i = 0; i < 420; i++) {
            for(uint32_t j = 0; j < 80; j++) mon[j] = (uint8_t)(i * 7 + j * 3 + 1);
            fake_save_put_box_mon(img, &spec, 1, (uint8_t)(i / 30), (uint8_t)(i % 30), mon);
        }
        for(uint32_t p = 0; p < 6; p++) {
            for(uint32_t j = 0; j < 100; j++) party[p][j] = (uint8_t)(0x80 + p * 11 + j);
        }
        fake_save_put_party(img, &spec, 1, 6, (const uint8_t(*)[100])party);
        fake_save_put_box_name(img, &spec, 1, 0, "BOX 1");
        fake_save_put_box_name(img, &spec, 1, 13, "Zz09abcd");
        const Gen3LinkGame lg = link_game(games[g]);
        analyze_as(lg);
        ASSERT_EQ(slots.active, 1);
        ASSERT_TRUE(slots.strict_ok);
        ASSERT_FALSE(slots.padded);
        for(uint16_t i = 0; i < 420; i++) {
            for(uint32_t j = 0; j < 80; j++) mon[j] = (uint8_t)(i * 7 + j * 3 + 1);
            const uint8_t n = gen3_save_mon_spans(lg, (uint8_t)(i / 30), (uint8_t)(i % 30), sp);
            ASSERT_TRUE(n == 1 || n == 2);
            ASSERT_EQ(sp[0].len + (n == 2 ? sp[1].len : 0), 80);
            read_spans(sp, n, got);
            ASSERT_MEM_EQ(got, mon, 80);
        }
        for(uint8_t p = 0; p < 6; p++) {
            ASSERT_EQ(gen3_save_mon_spans(lg, GEN3_SAVE_PARTY, p, sp), 1);
            read_spans(sp, 1, got);
            ASSERT_MEM_EQ(got, party[p], 100);
        }
        Gen3SaveSpan s = gen3_save_party_count_span(lg);
        ASSERT_EQ(sector(slots.phys[s.id])[s.off], 6);
        s = gen3_save_box_name_span(0);
        static const uint8_t box1[9] = {0xBC, 0xC9, 0xD2, 0x00, 0xA2, 0xFF, 0x00, 0x00, 0x00};
        ASSERT_MEM_EQ(sector(slots.phys[s.id]) + s.off, box1, 9);
        s = gen3_save_box_name_span(13);
        static const uint8_t box14[9] = {0xD4, 0xEE, 0xA1, 0xAA, 0xD5, 0xD6, 0xD7, 0xD8, 0xFF};
        ASSERT_MEM_EQ(sector(slots.phys[s.id]) + s.off, box14, 9);
        /* a party of 2: the count, and the slots after it empty */
        fake_save_put_party(img, &spec, 1, 2, (const uint8_t(*)[100])party);
        analyze_as(lg);
        s = gen3_save_party_count_span(lg);
        ASSERT_EQ(sector(slots.phys[s.id])[s.off], 2);
        ASSERT_EQ(gen3_save_mon_spans(lg, GEN3_SAVE_PARTY, 2, sp), 1);
        read_spans(sp, 1, got);
        memset(mon, 0, sizeof(mon));
        ASSERT_MEM_EQ(got, mon, 100);
    }
}

/* A freshly built save has no Pokemon: every slot reads Empty */
TEST(test_built_save_is_empty) {
    build(FakeGameRS, true, 4, 0, false, 0, 0);
    analyze();
    Gen3SaveSpan sp[2];
    uint8_t got[100];
    Gen3MonSummary m;
    for(uint16_t i = 0; i < 420; i++) {
        const uint8_t n = gen3_save_mon_spans(Gen3LinkGameRS, (uint8_t)(i / 30), i % 30, sp);
        read_spans(sp, n, got);
        gen3_save_mon_summary(got, 80, &m);
        ASSERT_EQ(m.kind, Gen3MonEmpty);
    }
    const Gen3SaveSpan c = gen3_save_party_count_span(Gen3LinkGameRS);
    ASSERT_EQ(sector(slots.phys[c.id])[c.off], 0);
}

TEST(test_trainer) {
    build(FakeGameRS, true, 4, 6, false, 0, 0);
    fake_save_put_trainer(img, &spec, 0, "BRENDAN", 0, 12345, 54321);
    uint8_t* s0 = sector(phys(0, 0));
    s0[0x0E] = 0x02;
    s0[0x0F] = 0x01;
    s0[0x10] = 3;
    fake_save_fix(img, phys(0, 0), FakeGameRS);
    analyze();
    ASSERT_TRUE(slots.strict_ok);
    Gen3SaveTrainer t;
    gen3_save_trainer(sector(slots.phys[0]), &t);
    ASSERT_STR_EQ(t.name, "BRENDAN");
    static const uint8_t raw[8] = {0xBC, 0xCC, 0xBF, 0xC8, 0xBE, 0xBB, 0xC8, 0xFF};
    ASSERT_MEM_EQ(t.name_raw, raw, 8);
    ASSERT_MEM_EQ(t.name_raw, s0, 8);
    ASSERT_EQ(t.gender, 0);
    ASSERT_EQ(t.tid, 12345);
    ASSERT_EQ(t.sid, 54321);
    ASSERT_EQ(t.hours, 0x0102);
    ASSERT_EQ(t.minutes, 3);
    /* FRLG: the same place */
    build(FakeGameFRLG, false, 0, 0, true, 9, 2);
    fake_save_put_trainer(img, &spec, 1, "May", 1, 1, 65535);
    analyze();
    gen3_save_trainer(sector(slots.phys[0]), &t);
    ASSERT_STR_EQ(t.name, "May");
    ASSERT_EQ(t.gender, 1);
    ASSERT_EQ(t.tid, 1);
    ASSERT_EQ(t.sid, 65535);
}

/* ---- Pokemon ---- */

TEST(test_mon_summary) {
    uint8_t g[100];
    golden_game(g);
    Gen3MonSummary m;
    gen3_save_mon_summary(g, 100, &m);
    ASSERT_EQ(m.kind, Gen3MonOk);
    ASSERT_EQ(m.national, 252);
    ASSERT_EQ(m.level, 5);
    ASSERT_STR_EQ(m.nickname, "TREECKO");
    gen3_save_mon_summary(g, 80, &m);
    ASSERT_EQ(m.kind, Gen3MonOk);
    ASSERT_EQ(m.national, 252);
    ASSERT_EQ(m.level, 5);
    uint8_t z[100];
    memset(z, 0, sizeof(z));
    gen3_save_mon_summary(z, 80, &m);
    ASSERT_EQ(m.kind, Gen3MonEmpty);
    ASSERT_EQ(m.national, 0);
    ASSERT_EQ(m.level, 0);
    ASSERT_STR_EQ(m.nickname, "");
    gen3_save_mon_summary(z, 100, &m);
    ASSERT_EQ(m.kind, Gen3MonEmpty);
    uint8_t b[100];
    memcpy(b, g, 100);
    b[0x30] ^= 1;
    gen3_save_mon_summary(b, 100, &m);
    ASSERT_EQ(m.kind, Gen3MonBad);
    ASSERT_EQ(m.national, 0);
    gen3_save_mon_summary(g, 79, &m);
    ASSERT_EQ(m.kind, Gen3MonBad);
    /* fields changed, then re-encrypted with the checksum fixed */
    Gen3Pkm p;
    ASSERT_TRUE(gen3_pkm_decrypt(g, 100, &p));
    Gen3Pkm q = p;
    gen3_pkm_set(&q, Gen3FieldIsEgg, 1);
    gen3_pkm_encrypt(&q, b);
    gen3_save_mon_summary(b, 80, &m);
    ASSERT_EQ(m.kind, Gen3MonEgg);
    ASSERT_EQ(m.national, 252);
    q = p;
    gen3_pkm_set(&q, Gen3FieldEggFlag, 1);
    gen3_pkm_encrypt(&q, b);
    gen3_save_mon_summary(b, 80, &m);
    ASSERT_EQ(m.kind, Gen3MonEgg);
    q = p;
    gen3_pkm_set(&q, Gen3FieldBadEgg, 1);
    gen3_pkm_encrypt(&q, b);
    gen3_save_mon_summary(b, 80, &m);
    ASSERT_EQ(m.kind, Gen3MonBad);
    q = p;
    gen3_pkm_set(&q, Gen3FieldSpecies, 412); /* the egg's internal id: no species */
    gen3_pkm_encrypt(&q, b);
    gen3_save_mon_summary(b, 80, &m);
    ASSERT_EQ(m.kind, Gen3MonBad);
    q = p;
    gen3_pkm_set(&q, Gen3FieldSpecies, 0);
    gen3_pkm_encrypt(&q, b);
    gen3_save_mon_summary(b, 80, &m);
    ASSERT_EQ(m.kind, Gen3MonEmpty);
    /* level from exp: 1,059,860 is level 100 on the medium slow curve */
    q = p;
    gen3_pkm_set(&q, Gen3FieldExp, 1059860);
    gen3_pkm_encrypt(&q, b);
    gen3_save_mon_summary(b, 80, &m);
    ASSERT_EQ(m.kind, Gen3MonOk);
    ASSERT_EQ(m.level, 100);
}

TEST(test_mon_pkm_roundtrip) {
    uint8_t g[100];
    golden_game(g);
    Gen3Pkm p;
    ASSERT_TRUE(gen3_save_mon_to_pkm(g, 80, &p));
    ASSERT_MEM_EQ(p.raw + 0x50, GOLDEN_PK3 + 0x50, 20);
    ASSERT_MEM_EQ(p.raw, GOLDEN_PK3, 100);
    uint8_t out[100];
    memset(out, 0xEE, sizeof(out));
    gen3_save_pkm_to_mon(&p, 80, out);
    ASSERT_MEM_EQ(out, g, 80);
    for(uint32_t i = 80; i < 100; i++) ASSERT_EQ(out[i], 0xEE);
    gen3_save_pkm_to_mon(&p, 100, out);
    ASSERT_MEM_EQ(out, g, 100);
    /* len 100 keeps the party fields as stored */
    uint8_t h[100];
    memcpy(h, g, 100);
    h[0x50] = 0x08; /* poisoned */
    h[0x56] = 7; /* HP 7 of 20 */
    ASSERT_TRUE(gen3_save_mon_to_pkm(h, 100, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldStatus), 0x08);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldHp), 7);
    /* a box Pokemon's party fields are derived, whatever the 80 bytes */
    ASSERT_TRUE(gen3_save_mon_to_pkm(h, 80, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldStatus), 0);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldMail), GEN3_MAIL_NONE);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldHp), 20);
    /* a broken checksum or a wrong length: false, *out untouched */
    g[0x30] ^= 1;
    Gen3Pkm q;
    memset(&q, 0x5A, sizeof(q));
    ASSERT_FALSE(gen3_save_mon_to_pkm(g, 80, &q));
    ASSERT_FALSE(gen3_save_mon_to_pkm(h, 99, &q));
    for(uint32_t i = 0; i < sizeof(q.raw); i++) ASSERT_EQ(q.raw[i], 0x5A);
}

int main(void) {
    RUN(test_save_checksum);
    RUN(test_section_sizes);
    RUN(test_section_sizes_match_fake_and_helper);
    RUN(test_footer_roundtrip);
    RUN(test_sector_info);
    RUN(test_slots_both_ok_b_newer);
    RUN(test_slots_a_newer);
    RUN(test_slots_wrap);
    RUN(test_slots_wrap_odd);
    RUN(test_slots_one_bad);
    RUN(test_slots_missing_id);
    RUN(test_slots_empty);
    RUN(test_slots_corrupt);
    RUN(test_slots_counter_mismatch);
    RUN(test_slots_bad_id);
    RUN(test_slots_sig_only);
    RUN(test_slots_parity);
    RUN(test_slots_padded);
    RUN(test_save_layout_mismatch);
    RUN(test_slots_match_helper);
    RUN(test_plan_write);
    RUN(test_plan_write_wrap);
    RUN(test_plan_refused);
    RUN(test_plan_fresh);
    RUN(test_mon_spans);
    RUN(test_count_and_name_spans);
    RUN(test_spans_match_fake);
    RUN(test_built_save_is_empty);
    RUN(test_trainer);
    RUN(test_mon_summary);
    RUN(test_mon_pkm_roundtrip);
    TEST_EXIT();
}
