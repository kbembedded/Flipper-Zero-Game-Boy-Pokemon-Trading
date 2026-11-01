#include "test.h"
#include "bulk_file.h"
#include "fake_gba.h"
#include "fake_helper.h"
#include "fake_save.h"
#include "fake_store.h"
#include "gen3_golden.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_save.h>
#include <src/gen3/gen3_trade_block.h>

/* The cart jobs through the client and the real packet pump against the helper's real core on the
 * fake GBA (fake_helper), with a RAM store. The default cart: English Ruby (AXVE version 0), 1 MiB
 * of ROM with FLASH1M_V103, an MX chip. Its save: slot A with counter 4 and id 0 at its first
 * sector, slot B absent; trainer BRENDAN 12345/54321; a party of 3 (the golden Treecko and two
 * edits of it); box 0 named BOX 1; the golden Treecko at box 1 slot 19. The store holds 64 MiB. */

#define MIB 0x100000u
#define KIB 1024u
#define SECTOR_LEN 4096u
#define WORK_LEN (14u * SECTOR_LEN)
#define MX 0x09C2u
#define FOREVER 10000000u /* an unplug longer than any test */
#define LIB_OFF 0xB0000u /* FLASH1M_V103 in the ROM */
#define LIB_STRING "FLASH1M_V103"
#define STAMP "20261025-143005"
#define BACKUP_NAME "backups/AXVE_12345_" STAMP ".sav"
#define BACKUP_NAME_2 "backups/AXVE_12345_" STAMP "-2.sav"
#define BACKUP_NAME_3 "backups/AXVE_12345_" STAMP "-3.sav"
#define ROM_NAME "roms/AXVE_v0.gba"
#define PART_NAME part_name() /* the .part of the cart inserted */
#define REGION GEN3_CART_ROM_REGION
#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

static FakeGba gba;
static FakeHelper fh;
static Gen3Hlink pump;
static Gen3Hcli cli;
static FakeStore store;
static Gen3Store api;
static Gen3Cart cart;
static uint8_t buf[GEN3_CART_BUF_SIZE];
static FakeSaveSpec spec;
static uint8_t img[FAKE_SAVE_LEN];
static uint8_t img2[FAKE_SAVE_LEN];
static uint8_t hdr[0xC0];
static uint8_t party[3][100];
static uint8_t chunk[SECTOR_LEN];
static Gen3Editor ed;
static volatile bool cancel;

/* ---- Observers ---- */

/* The progress the jobs report, and what a test does at a step */
typedef struct {
    uint32_t calls[Gen3CartStepRomVerify + 1];
    uint32_t last_done[Gen3CartStepRomVerify + 1];
    uint32_t last_total[Gen3CartStepRomVerify + 1];
    Gen3CartStep cancel_step; /* None: never */
    uint32_t cancel_at; /* cancel once done reaches this */
    Gen3CartStep unplug_step; /* None: never */
    uint32_t unplug_at; /* the cable out for good once done reaches this */
} Prog;

static Prog prog;

static void prog_cb(void* ctx, Gen3CartStep step, uint32_t done, uint32_t total) {
    Prog* p = ctx;
    p->calls[step]++;
    p->last_done[step] = done;
    p->last_total[step] = total;
    if(p->cancel_step != Gen3CartStepNone && step == p->cancel_step && done >= p->cancel_at) {
        cancel = true;
    }
    if(p->unplug_step != Gen3CartStepNone && step == p->unplug_step && done >= p->unplug_at) {
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), FOREVER);
        p->unplug_step = Gen3CartStepNone;
    }
}

/* The fake helper's command hook: counts each code; after READ_SECTOR of flip_sector, flips a
 * byte of that sector in the flash; when unplug_code runs, the cable out for unplug_len
 * transfers; when slow_code runs, the helper busy slow_us longer; after the glitch_after_crc-th
 * ROM_CRC32, a ROM glitch at glitch_off. */
typedef struct {
    uint32_t calls[256];
    bool flip;
    uint8_t flip_sector;
    uint8_t unplug_code;
    uint32_t unplug_len;
    uint8_t slow_code;
    uint32_t slow_us;
    uint32_t glitch_after_crc;
    uint32_t glitch_off;
} Hook;

static Hook hook;

static void hook_cb(void* ctx, uint8_t code, uint16_t arg0) {
    Hook* k = ctx;
    k->calls[code]++;
    if(k->slow_code && code == k->slow_code) {
        k->slow_code = 0;
        fh.busy_end_us += k->slow_us;
    }
    if(k->flip && code == GEN3_HCMD_READ_SECTOR && arg0 == k->flip_sector) {
        k->flip = false;
        fake_gba_flash(&gba)[k->flip_sector * SECTOR_LEN + 0x20] ^= 0x01;
    }
    if(k->unplug_code && code == k->unplug_code) {
        k->unplug_code = 0;
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), k->unplug_len);
    }
    if(k->glitch_after_crc && code == GEN3_HCMD_ROM_CRC32 &&
       k->calls[code] == k->glitch_after_crc) {
        k->glitch_after_crc = 0;
        fake_gba_rom_glitch_once(&gba, k->glitch_off);
    }
}

/* A transport around the fake helper's that counts syncs and exchanges, and can answer fake_code
 * itself, without the helper, with fake_status and fake_n payload words (fail code 7, offset 0) */
typedef struct {
    Gen3HTransport inner;
    uint32_t syncs;
    uint32_t exchanges;
    uint8_t fake_code;
    uint8_t fake_status;
    uint16_t fake_n;
} Tap;

static Tap tap;

static Gen3HlinkFail
    tap_exchange(void* ctx, const Gen3HlinkReq* req, uint8_t* status, uint16_t* resp_words) {
    Tap* t = ctx;
    t->exchanges++;
    if(t->fake_code && req->code == t->fake_code) {
        memset(req->resp, 0, 2u * t->fake_n);
        req->resp[0] = 7;
        *status = t->fake_status;
        *resp_words = t->fake_n;
        return Gen3HlinkFailNone;
    }
    return t->inner.exchange(t->inner.ctx, req, status, resp_words);
}

static bool tap_sync(void* ctx, uint8_t need, uint32_t timeout_us) {
    Tap* t = ctx;
    t->syncs++;
    return t->inner.sync(t->inner.ctx, need, timeout_us);
}

/* ---- Fixtures ---- */

static uint8_t* flash(void) {
    return fake_gba_flash(&gba);
}

static uint8_t phys(uint8_t slot, uint8_t id) {
    return (uint8_t)fake_save_phys(&spec, slot, id);
}

/* The default save in img, in game's layout; the party edits go through the editor */
static bool default_save_for(FakeGame game) {
    spec = (FakeSaveSpec){
        .game = game,
        .slot_present = {true, false},
        .counter = {4, 0},
        .first = {0, 0},
        .seed = 0x5EED0009u,
        .no_mons = true,
    };
    fake_save_build(img, &spec);
    fake_save_put_trainer(img, &spec, 0, "BRENDAN", 0, 12345, 54321);
    golden_game(party[0]);
    Gen3Pkm p;
    if(!gen3_save_mon_to_pkm(party[0], 100, &p)) return false;
    if(!gen3_edit_init(&ed, 1) || !gen3_edit_load(&ed, &p)) return false;
    gen3_edit_set_level(&ed, 30);
    gen3_save_pkm_to_mon(&ed.pkm, 100, party[1]);
    if(!gen3_edit_set_nickname(&ed, "LEAFY")) return false;
    gen3_save_pkm_to_mon(&ed.pkm, 100, party[2]);
    fake_save_put_party(img, &spec, 0, 3, (const uint8_t(*)[100])party);
    fake_save_put_box_name(img, &spec, 0, 0, "BOX 1");
    fake_save_put_box_mon(img, &spec, 0, 1, 19, party[0]);
    return true;
}

static bool default_save(void) {
    return default_save_for(FakeGameRS);
}

/* Inserts a cart with img on its save chip; a fresh helper, transport, client, store and cart. */
static void insert(const char* code, uint32_t rom_size, FakeSaveKind kind) {
    fake_gba_init(&gba);
    helper_cmd_header(hdr, gba.own, "POKEMON RUBY", code, 0);
    fake_gba_rom(&gba, hdr, rom_size);
    fake_gba_rom_string(&gba, LIB_OFF, LIB_STRING);
    fake_gba_save(&gba, kind, kind == FakeSaveFlash ? MX : 0);
    memcpy(flash(), img, sizeof(img));
    fake_helper_init(&fh, &gba);
    memset(&hook, 0, sizeof(hook));
    fake_helper_on_command(&fh, hook_cb, &hook);
    memset(&tap, 0, sizeof(tap));
    tap.inner = fake_helper_transport(&fh, &pump);
    const Gen3HTransport t = {
        .ctx = &tap,
        .exchange = tap_exchange,
        .sync = tap_sync,
    };
    gen3_hcli_init(&cli, &t);
    fake_store_init(&store, 64u * MIB);
    api = fake_store_api(&store);
    memset(&prog, 0, sizeof(prog));
    cancel = false;
    const Gen3CartProgress p = {
        .cb = prog_cb,
        .ctx = &prog,
        .cancel = &cancel,
    };
    gen3_cart_init(&cart, &cli, &api, buf, &p);
}

static bool setup(void) {
    if(!default_save()) return false;
    insert("AXVE", MIB, FakeSaveFlash);
    return true;
}

static bool connected(void) {
    return setup() && gen3_cart_connect(&cart) == Gen3CartOk;
}

static bool backed_up(void) {
    return connected() && gen3_cart_backup(&cart, STAMP) == Gen3CartOk;
}

static const uint8_t* file(const char* name, uint32_t* size) {
    *size = 0;
    return fake_store_file(&store, name, size);
}

/* The working copy holds the given sectors in id order */
static bool work_is(const uint8_t* flash_img, const FakeSaveSpec* s, uint8_t slot) {
    uint32_t size;
    const uint8_t* w = file(GEN3_CART_WORK_NAME, &size);
    if(!w || size != WORK_LEN) return false;
    for(uint8_t k = 0; k < 14; k++) {
        const uint32_t at = fake_save_phys(s, slot, k) * SECTOR_LEN;
        if(memcmp(w + k * SECTOR_LEN, flash_img + at, SECTOR_LEN) != 0) return false;
    }
    return true;
}

/* Every sector of the working copy but id k equals slot A's */
static bool work_differs_only_in(uint8_t only) {
    uint32_t size;
    const uint8_t* w = file(GEN3_CART_WORK_NAME, &size);
    if(!w || size != WORK_LEN) return false;
    for(uint8_t k = 0; k < 14; k++) {
        const bool same =
            memcmp(w + k * SECTOR_LEN, flash() + phys(0, k) * SECTOR_LEN, SECTOR_LEN) == 0;
        if(same == (k == only)) return false;
    }
    return true;
}

/* The card holds n files, all .sav: no temporary backup, no working copy */
static bool only_savs(uint32_t n) {
    return fake_store_count(&store, ".sav") == n && fake_store_count(&store, "") == n;
}

/* The .sav named holds the fake flash as it is, and no temporary backup is left */
static bool sav_is_flash(const char* name) {
    uint32_t size;
    const uint8_t* s = file(name, &size);
    return s && size == FAKE_SAVE_LEN && memcmp(s, flash(), FAKE_SAVE_LEN) == 0 &&
           fake_store_count(&store, ".tmp") == 0;
}

/* The fake ROM's byte at off (fake_gba.h): the header, the library string, else the pattern */
static uint8_t rom_byte(uint32_t off) {
    if(off < sizeof(hdr)) return hdr[off];
    if(off >= LIB_OFF && off - LIB_OFF < strlen(LIB_STRING))
        return (uint8_t)LIB_STRING[off - LIB_OFF];
    const uint16_t hw = (uint16_t)(((off & ~1u) * 31u) >> 3);
    return (off & 1u) ? (uint8_t)(hw >> 8) : (uint8_t)hw;
}

static bool is_rom(const uint8_t* data, uint32_t len) {
    for(uint32_t off = 0; off < len; off++) {
        if(data[off] != rom_byte(off)) return false;
    }
    return true;
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

/* A ROM copy's .part for the cart inserted: the ROM's name, then the CRC-32 of the cart's header
 * bytes 0xA0..0xBF and its ROM size (little-endian), as the copy that started it read them */
static const char* part_name(void) {
    static char name[GEN3_CART_NAME_MAX];
    uint8_t id[0x24];
    memcpy(id, hdr + 0xA0, 0x20);
    for(uint32_t i = 0; i < 4; i++) id[0x20 + i] = (uint8_t)(gba.rom_size >> (8u * i));
    snprintf(name, sizeof(name), ROM_NAME ".%08x.part", (unsigned)crc32_ref(id, sizeof(id)));
    return name;
}

static uint32_t calls(uint8_t code) {
    return hook.calls[code];
}

/* A Pokemon put: the golden Treecko at level 50, through the editor */
static bool level50(Gen3Pkm* out) {
    Gen3Pkm p;
    if(!gen3_save_mon_to_pkm(party[0], 100, &p) || !gen3_edit_load(&ed, &p)) return false;
    gen3_edit_set_level(&ed, 50);
    *out = ed.pkm;
    return true;
}

/* ---- Connect ---- */

TEST(test_job_connect) {
    ASSERT_TRUE(connected());
    const Gen3CartId* id = gen3_cart_id(&cart);
    ASSERT_TRUE(id->can_write);
    ASSERT_STR_EQ(id->code, "AXVE");
    ASSERT_EQ(id->why, Gen3CartRefuseNone);
    ASSERT_EQ(id->flash_id, MX);
    ASSERT_EQ(gen3_cart_rom_size(&cart), MIB);
    ASSERT_EQ(calls(GEN3_HCMD_HELLO), 1);
    ASSERT_EQ(calls(GEN3_HCMD_CART_INFO), 1);
    ASSERT_EQ(calls(GEN3_HCMD_SAVE_PROBE), 1);
    ASSERT_TRUE(prog.calls[Gen3CartStepIdentify] > 0);
    /* nothing backed up yet: nothing to browse or write */
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    Gen3CartRefuse why = Gen3CartRefuseNone;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "");
    Gen3SaveTrainer t;
    ASSERT_FALSE(gen3_cart_trainer(&cart, &t));
}

/* A lost command can keep the helper busy past one connect's sync: the next connect keeps trying
 * for that command's BUSY budget. A 16 MiB ROM: CART_INFO's scan takes about 7 s. */
TEST(test_job_connect_waits_out_busy) {
    ASSERT_TRUE(default_save());
    insert("AXVE", 16u * MIB, FakeSaveFlash);
    hook.unplug_code = GEN3_HCMD_CART_INFO;
    hook.unplug_len = 600;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartLinkLost);
    ASSERT_TRUE(fh.busy);
    tap.syncs = 0;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_TRUE(tap.syncs >= 2);
    ASSERT_STR_EQ(gen3_cart_id(&cart)->code, "AXVE");
    ASSERT_EQ(calls(GEN3_HCMD_CART_INFO), 2);
}

/* After multiboot the GBA takes a few seconds to start the helper (3.5 s measured on 2026-10-31):
 * one sync is too short for it... */
TEST(test_job_connect_late_helper) {
    ASSERT_TRUE(setup());
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), 3300); /* about 3.5 s */
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartLinkLost);
}

/* ...so the first connect after a send keeps trying HELLO for GEN3_CART_BOOT_US, and only that
 * one: a connect that came through uses the wait up */
TEST(test_job_connect_after_boot) {
    ASSERT_TRUE(setup());
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), 3300);
    gen3_cart_expect_boot(&cart);
    tap.syncs = 0;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_TRUE(tap.syncs >= 2);
    ASSERT_EQ(calls(GEN3_HCMD_HELLO), 1);
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), 3300);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartLinkLost);
}

/* After a lost command, no job sends anything until a connect */
TEST(test_job_link_needs_connect) {
    ASSERT_TRUE(backed_up());
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + 3000, FOREVER);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    fake_helper_replug(&fh);
    const uint32_t before = fake_helper_transfers(&fh);
    const uint32_t exchanges = tap.exchanges;
    bool dropped = true;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    ASSERT_EQ(fake_helper_transfers(&fh), before);
    ASSERT_EQ(tap.exchanges, exchanges);
    /* nothing about the backup changed meanwhile */
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
    ASSERT_FALSE(dropped);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    /* a backup lost midway */
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + 3000, FOREVER);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
}

/* The helper busy past a read's budget: BusyTimeout, and the next connect waits for it */
TEST(test_job_busy_timeout) {
    ASSERT_TRUE(connected());
    hook.slow_code = GEN3_HCMD_READ_SECTOR;
    hook.slow_us = 10000000u;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartBusyTimeout);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    tap.syncs = 0;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_TRUE(tap.syncs >= 2);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
}

/* A helper that answers HELLO for another protocol: no job talks to it */
TEST(test_job_connect_wrong_helper) {
    ASSERT_TRUE(connected());
    tap.fake_code = GEN3_HCMD_HELLO;
    tap.fake_status = GEN3_HST_OK;
    tap.fake_n = 4;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartHelperError);
    tap.fake_code = 0;
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
}

/* Back while connecting: between commands */
TEST(test_job_connect_cancel) {
    ASSERT_TRUE(setup());
    prog.cancel_step = Gen3CartStepIdentify;
    prog.cancel_at = 1;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartCancelled);
    ASSERT_EQ(calls(GEN3_HCMD_HELLO), 1);
    ASSERT_EQ(calls(GEN3_HCMD_CART_INFO), 0);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
}

/* A connect cut short after its HELLO, by cancel or an error, leaves the cart unlinked: its
 * CART_INFO may already describe another cart than its id does. Swapped to FireRed, a ROM copy
 * then would write FireRed as roms/AXVE_v0.gba over the Ruby one. */
TEST(test_job_connect_partial) {
    ASSERT_TRUE(backed_up());
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
    static uint8_t ruby[MIB];
    uint32_t size;
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL && size == MIB);
    memcpy(ruby, rom, MIB);
    helper_cmd_header(hdr, gba.own, "POKEMON FIRE", "BPRE", 0);
    fake_gba_rom(&gba, hdr, MIB);
    fake_gba_rom_string(&gba, LIB_OFF, LIB_STRING);
    fake_helper_restart(&fh);
    /* cancelled after CART_INFO */
    prog.cancel_step = Gen3CartStepIdentify;
    prog.cancel_at = 2;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartCancelled);
    ASSERT_EQ(calls(GEN3_HCMD_CART_INFO), 2);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    const uint32_t exchanges = tap.exchanges;
    bool dropped = true;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartLinkLost);
    ASSERT_EQ(tap.exchanges, exchanges);
    rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL && size == MIB);
    ASSERT_MEM_EQ(rom, ruby, MIB);
    ASSERT_EQ(fake_store_count(&store, ".part"), 0);
    /* an error after HELLO: the same */
    tap.fake_code = GEN3_HCMD_SAVE_PROBE;
    tap.fake_status = GEN3_HST_UNSUPPORTED;
    tap.fake_n = 2;
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartHelperError);
    tap.fake_code = 0;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    ASSERT_EQ(fake_store_count(&store, ".part"), 0);
    /* a whole connect: FireRed, under its own name */
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_STR_EQ(gen3_cart_id(&cart)->code, "BPRE");
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
    rom = file("roms/BPRE_v0.gba", &size);
    ASSERT_TRUE(rom != NULL && size == MIB);
    ASSERT_TRUE(is_rom(rom, size));
    rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL && size == MIB);
    ASSERT_MEM_EQ(rom, ruby, MIB);
}

/* Every answer the client can give, as a job's result */
TEST(test_job_client_results) {
    static const struct {
        uint8_t status;
        uint16_t n;
        Gen3CartResult want;
    } cases[] = {
        {GEN3_HST_REFUSED, 2, Gen3CartRefused},
        {GEN3_HST_UNSUPPORTED, 2, Gen3CartHelperError},
        {GEN3_HST_BAD_ARG, 2, Gen3CartHelperError},
        {GEN3_HST_OK, 3, Gen3CartHelperError}, /* a sound response of the wrong length */
    };
    for(uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASSERT_TRUE(connected());
        tap.fake_code = GEN3_HCMD_READ_SECTOR;
        tap.fake_status = cases[i].status;
        tap.fake_n = cases[i].n;
        ASSERT_EQ(gen3_cart_backup(&cart, STAMP), cases[i].want);
        ASSERT_EQ(fake_store_count(&store, ""), 0);
        /* the link is still in step: the next job runs */
        tap.fake_code = 0;
        ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    }
    /* the helper itself refuses a read the Flipper should never send: a save chip it can't read */
    ASSERT_TRUE(default_save());
    insert("AXVE", MIB, FakeSaveSram);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    cart.id.can_read_save = true;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartHelperError);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    FakeGbaCounts n;
    fake_gba_counts(&gba, &n);
    ASSERT_EQ(n.erases + n.programs + n.commits + n.bank_selects, 0);
}

/* ---- Backup ---- */

TEST(test_job_backup_ok) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    ASSERT_TRUE(work_is(flash(), &spec, 0));
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), BACKUP_NAME);
    Gen3SaveTrainer t;
    ASSERT_TRUE(gen3_cart_trainer(&cart, &t));
    ASSERT_STR_EQ(t.name, "BRENDAN");
    ASSERT_EQ(t.tid, 12345);
    ASSERT_EQ(t.sid, 54321);
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    ASSERT_EQ(s->result, Gen3SaveOk);
    ASSERT_EQ(s->active, 0);
    ASSERT_EQ(s->save_counter, 4);
    ASSERT_TRUE(s->strict_ok);
    ASSERT_FALSE(s->padded);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    Gen3CartRefuse why = Gen3CartRefuseSave;
    ASSERT_TRUE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseNone);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseNone);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    /* read once, then checked on the cart: every sector */
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 32);
    ASSERT_EQ(calls(GEN3_HCMD_SECTOR_CRC32), 32);
    ASSERT_EQ(prog.calls[Gen3CartStepBackupRead], 32);
    ASSERT_EQ(prog.last_done[Gen3CartStepBackupRead], 32);
    ASSERT_EQ(prog.last_total[Gen3CartStepBackupRead], 32);
    ASSERT_TRUE(prog.calls[Gen3CartStepBackupVerify] > 0);
    ASSERT_TRUE(prog.calls[Gen3CartStepAnalyze] > 0);
    /* nothing ever wrote to the chip */
    FakeGbaCounts n;
    fake_gba_counts(&gba, &n);
    ASSERT_EQ(n.erases + n.programs + n.commits + n.chip_erases, 0);
}

TEST(test_job_backup_nosave) {
    ASSERT_TRUE(default_save());
    memset(img, 0xFF, sizeof(img));
    insert("AXVE", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash("backups/AXVE_nosave_" STAMP ".sav"));
    ASSERT_EQ(gen3_cart_slots(&cart)->result, Gen3SaveEmpty);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    uint32_t size;
    ASSERT_TRUE(file(GEN3_CART_WORK_NAME, &size) == NULL);
    Gen3SaveTrainer t;
    ASSERT_FALSE(gen3_cart_trainer(&cart, &t));
    /* a cart with no save can still be written, by a restore, but not by an edit */
    ASSERT_TRUE(gen3_cart_id(&cart)->can_write);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
    Gen3CartRefuse why;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
    ASSERT_EQ(gen3_cart_party_count(&cart), 0);
    uint8_t mon[100];
    size_t len;
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 0, mon, &len), Gen3CartBadArg);
}

/* A save gone since the last backup: its working copy goes too */
TEST(test_job_backup_save_gone) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    uint32_t size;
    ASSERT_TRUE(file(GEN3_CART_WORK_NAME, &size) != NULL);
    memset(flash(), 0xFF, FAKE_SAVE_LEN);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash("backups/AXVE_nosave_" STAMP ".sav"));
    ASSERT_TRUE(file(GEN3_CART_WORK_NAME, &size) == NULL);
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 1, 19));
}

TEST(test_job_backup_sd_full_precheck) {
    ASSERT_TRUE(connected());
    fake_store_init(&store, 150000);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdFull);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

/* The pre-check sees 64 MiB, the card fills after 17 sectors: SdFull, not SdError */
TEST(test_job_backup_sd_full_midway) {
    ASSERT_TRUE(connected());
    fake_store_init(&store, 70000);
    fake_store_report_free(&store, 64u * MIB);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdFull);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 0);
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 18);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
}

TEST(test_job_backup_sd_write_error) {
    ASSERT_TRUE(connected());
    fake_store_fail_write_after(&store, 5);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

TEST(test_job_backup_readback_mismatch) {
    ASSERT_TRUE(connected());
    fake_store_corrupt_read(&store, GEN3_CART_BACKUP_TMP, 7 * SECTOR_LEN + 9);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartVerifyFailed);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 0);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

/* The cart reads one sector differently the second time */
TEST(test_job_backup_flash_unstable) {
    ASSERT_TRUE(connected());
    hook.flip = true;
    hook.flip_sector = 4;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartVerifyFailed);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

TEST(test_job_backup_link_lost) {
    ASSERT_TRUE(connected());
    fake_helper_unplug_at(&fh, 25000, FOREVER);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartLinkLost);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_TRUE(calls(GEN3_HCMD_READ_SECTOR) > 1);
}

TEST(test_job_backup_cancel) {
    ASSERT_TRUE(connected());
    prog.cancel_step = Gen3CartStepBackupRead;
    prog.cancel_at = 3;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartCancelled);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 3);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

/* Back while the cart reads each sector again */
TEST(test_job_backup_cancel_verify) {
    ASSERT_TRUE(connected());
    prog.cancel_step = Gen3CartStepBackupVerify;
    prog.cancel_at = 5;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartCancelled);
    ASSERT_EQ(calls(GEN3_HCMD_SECTOR_CRC32), 5);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

/* The card fails to close the temporary file (its flush), or to give a sector back */
TEST(test_job_backup_store_faults) {
    ASSERT_TRUE(connected());
    fake_store_fail_close(&store, 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_EQ(calls(GEN3_HCMD_SECTOR_CRC32), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    /* the read-back's first read */
    fake_store_fail_read(&store, 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    /* the trainer's sector, read after the 32 of the read-back */
    fake_store_fail_read(&store, 32);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    /* the working copy's close: after the temporary file's, the read-back's, the trainer read's
     * and the .sav's as the copy's source. The .sav, kept and verified by then, stays: */
    fake_store_fail_close(&store, 4);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_TRUE(only_savs(1));
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    /* the working copy's reads: its first sector from the .sav, after the read-back's 32 and the
     * trainer's, with only the backup's 32 sectors written; then the first of its own read-back,
     * after the 14 it copied. Each leaves its .sav, the next one -2, then -3. */
    static const struct {
        uint32_t reads, writes;
    } work_reads[] = {{33, 32}, {47, 46}};
    for(uint32_t i = 0; i < sizeof(work_reads) / sizeof(work_reads[0]); i++) {
        fake_store_fail_read(&store, work_reads[i].reads);
        const uint32_t writes = store.writes;
        ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
        ASSERT_EQ(store.writes - writes, work_reads[i].writes);
        ASSERT_TRUE(only_savs(2 + i));
        ASSERT_TRUE(sav_is_flash(i == 0 ? BACKUP_NAME_2 : BACKUP_NAME_3));
        ASSERT_FALSE(gen3_cart_backup_current(&cart));
        ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "");
        ASSERT_FALSE(gen3_cart_browsable(&cart));
        ASSERT_FALSE(cart.work_ok);
        ASSERT_FALSE(store.read_fault);
    }
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "backups/AXVE_12345_" STAMP "-4.sav");
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
}

/* Backup only: the Japanese layouts aren't known */
TEST(test_job_backup_japanese) {
    ASSERT_TRUE(default_save());
    insert("AXVJ", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseJapanese);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash("backups/AXVJ_nosave_" STAMP ".sav"));
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    uint32_t size;
    ASSERT_TRUE(file(GEN3_CART_WORK_NAME, &size) == NULL);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseJapanese);
    Gen3SaveTrainer t;
    ASSERT_FALSE(gen3_cart_trainer(&cart, &t));
}

/* A save chip the helper doesn't read: not even one read is sent */
TEST(test_job_backup_refused_sram) {
    ASSERT_TRUE(default_save());
    insert("AXVE", MIB, FakeSaveSram);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSram);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartRefused);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 0);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
    FakeGbaCounts n;
    fake_gba_counts(&gba, &n);
    ASSERT_EQ(n.erases + n.programs + n.commits + n.bank_selects, 0);
}

/* A cart whose save the helper doesn't read attaches with no backup: Cart info and the ROM copy
 * stay, and nothing that needs the save is offered */
TEST(test_job_attach_unreadable) {
    static const struct {
        const char* code;
        FakeSaveKind save;
        bool bad_complement;
        Gen3CartRefuse why;
    } carts[] = {
        {"AXVE", FakeSaveSram, false, Gen3CartRefuseSram},
        {"AXVE", FakeSaveNone, false, Gen3CartRefuseChip},
        {"AXVE", FakeSaveRomLike, false, Gen3CartRefuseChip},
        {"AGBE", FakeSaveFlash, false, Gen3CartRefuseNotPokemon},
        {"AXVE", FakeSaveFlash, true, Gen3CartRefuseHeader},
    };
    for(uint32_t i = 0; i < sizeof(carts) / sizeof(carts[0]); i++) {
        ASSERT_TRUE(default_save());
        insert(carts[i].code, MIB, carts[i].save);
        if(carts[i].bad_complement) {
            gba.header[0xBD] ^= 1;
            hdr[0xBD] ^= 1;
        }
        ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
        const Gen3CartId* id = gen3_cart_id(&cart);
        ASSERT_FALSE(id->can_read_save);
        ASSERT_EQ(id->why, carts[i].why);
        bool dropped = true;
        ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
        ASSERT_FALSE(dropped);
        ASSERT_FALSE(gen3_cart_backup_current(&cart));
        ASSERT_FALSE(gen3_cart_browsable(&cart));
        Gen3CartRefuse why = Gen3CartRefuseNone;
        ASSERT_FALSE(gen3_cart_writable(&cart, &why));
        ASSERT_EQ(why, carts[i].why);
        ASSERT_FALSE(id->can_browse);
        ASSERT_FALSE(id->can_write);
        uint8_t mon[100];
        size_t len;
        ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 0, mon, &len), Gen3CartBadArg);
        ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartBadArg);
        ASSERT_EQ(gen3_cart_party_count(&cart), 0);
        ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 0);
        ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 0);
        ASSERT_EQ(fake_store_count(&store, ""), 0);
        /* the ROM copy works */
        ASSERT_EQ(gen3_cart_rom_size(&cart), MIB);
        ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
        char name[GEN3_CART_NAME_MAX];
        gen3_cart_rom_name(&cart, name);
        uint32_t size;
        const uint8_t* rom = file(name, &size);
        ASSERT_TRUE(rom != NULL);
        ASSERT_EQ(size, MIB);
        ASSERT_TRUE(is_rom(rom, size));
        FakeGbaCounts n;
        fake_gba_counts(&gba, &n);
        ASSERT_EQ(n.erases + n.programs + n.commits + n.bank_selects, 0);
    }
    /* edits on the cart that was in before are dropped */
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    fake_gba_save(&gba, FakeSaveSram, 0);
    fake_helper_restart(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    bool dropped = false;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
    ASSERT_TRUE(dropped);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(cart.dirty, 0);
    ASSERT_FALSE(cart.work_ok);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(cart.backup_name[0], '\0');
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "");
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
}

/* A save the game loads but the Flipper can't write: Save, and can_write stays (a restore may
 * still write the cart) */
TEST(test_job_backup_not_strict_keeps_can_write) {
    ASSERT_TRUE(default_save());
    fake_save_set_counter(img, phys(0, 13), 2);
    insert("AXVE", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_FALSE(gen3_cart_slots(&cart)->strict_ok);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
    ASSERT_TRUE(gen3_cart_id(&cart)->can_write);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    Gen3CartRefuse why = Gen3CartRefuseNone;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
    ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
    /* the reason is the backup's: without one, only the identification's is left */
    fake_store_fail_write_after(&store, 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseNone);
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
}

/* The working copy can't be made: the backup isn't current, but its .sav, verified, stays */
TEST(test_job_backup_work_copy_fails) {
    ASSERT_TRUE(connected());
    fake_store_fail_write_after(&store, 32);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartSdError);
    ASSERT_TRUE(only_savs(1));
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "");
    ASSERT_FALSE(gen3_cart_browsable(&cart));
}

/* A loaded sector with a non-zero byte past its section: browsable, not writable */
TEST(test_job_backup_padded_not_writable) {
    ASSERT_TRUE(default_save());
    img[phys(0, 0) * SECTOR_LEN + 0x890] = 0x5A;
    insert("AXVE", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_slots(&cart)->strict_ok);
    ASSERT_TRUE(gen3_cart_slots(&cart)->padded);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
    ASSERT_TRUE(gen3_cart_id(&cart)->can_write);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    Gen3CartRefuse why = Gen3CartRefuseNone;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
}

/* A backup for a stamp already used gets -2, -3, ... */
TEST(test_job_backup_name_taken) {
    ASSERT_TRUE(backed_up());
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "backups/AXVE_12345_" STAMP "-2.sav");
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "backups/AXVE_12345_" STAMP "-3.sav");
    ASSERT_EQ(fake_store_count(&store, ".sav"), 3);
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
    /* a stamp too long for a name */
    ASSERT_EQ(gen3_cart_backup(&cart, "20261025-143005-1234567"), Gen3CartBadArg);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 3);
}

/* ---- Attach ---- */

/* The helper sent again to the same cart, whose save didn't change: the backup and the edits stay */
TEST(test_job_attach_keeps_changes) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    fake_helper_restart(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    bool dropped = true;
    ASSERT_EQ(gen3_cart_attach(&cart, "20261025-150000", &dropped), Gen3CartOk);
    ASSERT_FALSE(dropped);
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 0));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 1);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 32);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    ASSERT_TRUE(work_differs_only_in(1));
}

/* The game saved since the backup: a new backup, the edits dropped */
TEST(test_job_attach_after_game_save) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    FakeSaveSpec s2 = spec;
    s2.slot_present[1] = true;
    s2.counter[1] = 5;
    s2.first[1] = 11;
    fake_save_build(img2, &s2);
    fake_save_put_trainer(img2, &s2, 1, "MAY", 1, 22222, 33333);
    memcpy(flash() + 14 * SECTOR_LEN, img2 + 14 * SECTOR_LEN, 14 * SECTOR_LEN);
    bool dropped = false;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
    ASSERT_TRUE(dropped);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 0));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "backups/AXVE_22222_" STAMP ".sav");
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    ASSERT_FALSE(sav_is_flash(BACKUP_NAME));
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    ASSERT_EQ(s->active, 1);
    ASSERT_EQ(s->save_counter, 5);
    /* the trainer of the slot the game loads now, from its id 0 at sector 25 */
    Gen3SaveTrainer t;
    ASSERT_TRUE(gen3_cart_trainer(&cart, &t));
    ASSERT_STR_EQ(t.name, "MAY");
    ASSERT_EQ(t.tid, 22222);
    ASSERT_EQ(t.gender, 1);
    /* the working copy is slot B, in id order */
    ASSERT_TRUE(work_is(flash(), &s2, 1));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
}

/* Only a footer changed (a counter), or only a byte the game's checksum doesn't cover (the full
 * checksum, so the slot is padded now): either is another save */
TEST(test_job_attach_footer_changed) {
    for(uint32_t i = 0; i < 2; i++) {
        ASSERT_TRUE(backed_up());
        Gen3Pkm p;
        ASSERT_TRUE(level50(&p));
        ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
        if(i == 0) {
            fake_save_set_counter(flash(), phys(0, 13), 2);
        } else {
            flash()[phys(0, 0) * SECTOR_LEN + 0x890] = 0x5A;
        }
        bool dropped = false;
        ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
        ASSERT_TRUE(dropped);
        ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
        ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
        ASSERT_EQ(gen3_cart_changes(&cart), 0);
        ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
        ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
        ASSERT_EQ(gen3_cart_slots(&cart)->padded, i == 1);
    }
}

/* Each footer field on its own tells another save: only the checksums (a save at the same counter
 * with other data), only two ids (swapped), or only a signature */
TEST(test_job_attach_footer_fields) {
    for(uint32_t i = 0; i < 3; i++) {
        ASSERT_TRUE(backed_up());
        Gen3Pkm p;
        ASSERT_TRUE(level50(&p));
        ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
        if(i == 0) {
            FakeSaveSpec s2 = spec;
            s2.seed ^= 0x00010000u;
            fake_save_build(img2, &s2);
            memcpy(flash(), img2, 14 * SECTOR_LEN);
            /* ids, checksums (some), signatures and counters: only checksums differ */
            uint32_t other = 0;
            for(uint8_t s = 0; s < 14; s++) {
                const uint8_t* now = flash() + s * SECTOR_LEN;
                const uint8_t* was = img + s * SECTOR_LEN;
                ASSERT_MEM_EQ(now + 0xFF4, was + 0xFF4, 2);
                ASSERT_MEM_EQ(now + 0xFF8, was + 0xFF8, 8);
                other += memcmp(now + 0xFF6, was + 0xFF6, 2) != 0;
            }
            ASSERT_TRUE(other > 0);
        } else if(i == 1) {
            fake_save_set_id(flash(), phys(0, 2), 3);
            fake_save_set_id(flash(), phys(0, 3), 2);
        } else {
            flash()[phys(0, 5) * SECTOR_LEN + 0xFF9] ^= 0x01;
        }
        bool dropped = false;
        ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
        ASSERT_TRUE(dropped);
        ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
        ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
        ASSERT_EQ(gen3_cart_changes(&cart), 0);
    }
}

/* The same cart with another save chip: the backup isn't current */
TEST(test_job_connect_other_chip) {
    ASSERT_TRUE(backed_up());
    fake_gba_save(&gba, FakeSaveFlash, 0x1362u);
    fake_helper_restart(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->flash_id, 0x1362u);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    bool dropped = true;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
    ASSERT_FALSE(dropped);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
}

/* Another cart in the GBA: the backup isn't current from its connect on */
TEST(test_job_attach_other_cart) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    /* the same save, but revision 1 of the game */
    helper_cmd_header(hdr, gba.own, "POKEMON RUBY", "AXVE", 1);
    fake_gba_rom(&gba, hdr, MIB);
    fake_gba_rom_string(&gba, LIB_OFF, LIB_STRING);
    fake_helper_restart(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->version, 1);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
    bool dropped = false;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP, &dropped), Gen3CartOk);
    ASSERT_TRUE(dropped);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 0);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
}

/* ---- The working copy ---- */

TEST(test_job_mon_get_put) {
    ASSERT_TRUE(backed_up());
    uint8_t mon[100];
    size_t len = 0;
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 0, mon, &len), Gen3CartOk);
    ASSERT_EQ(len, 100);
    uint8_t golden[100];
    golden_game(golden);
    ASSERT_MEM_EQ(mon, golden, 100);
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 2, mon, &len), Gen3CartOk);
    ASSERT_MEM_EQ(mon, party[2], 100);
    ASSERT_EQ(gen3_cart_mon_get(&cart, 1, 19, mon, &len), Gen3CartOk);
    ASSERT_EQ(len, 80);
    ASSERT_MEM_EQ(mon, golden, 80);
    ASSERT_EQ(gen3_cart_party_count(&cart), 3);
    /* the party edit: id 1 only */
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    ASSERT_TRUE(work_differs_only_in(1));
    ASSERT_EQ(cart.dirty, 0x0002);
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 0));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 1));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 0, 0));
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 0, mon, &len), Gen3CartOk);
    uint8_t want[100];
    gen3_save_pkm_to_mon(&p, 100, want);
    ASSERT_MEM_EQ(mon, want, 100);
    Gen3Pkm back;
    ASSERT_TRUE(gen3_save_mon_to_pkm(mon, 100, &back));
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldLevel), 50);
    /* box 1 slot 19 straddles ids 5 and 6 */
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    ASSERT_EQ(cart.dirty, 0x0062);
    ASSERT_EQ(gen3_cart_changes(&cart), 2);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, 1, 19));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 1, 18));
    ASSERT_EQ(gen3_cart_mon_get(&cart, 1, 19, mon, &len), Gen3CartOk);
    ASSERT_MEM_EQ(mon, want, 80);
    /* the same slot again is one change */
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_changes(&cart), 2);
    /* box 0's first slots are not the party's */
    ASSERT_EQ(gen3_cart_mon_put(&cart, 0, 2, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_changes(&cart), 3);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, 0, 2));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 2));
    ASSERT_EQ(cart.dirty, 0x0062);
    /* the backup is never written */
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
    /* out of range */
    ASSERT_EQ(gen3_cart_mon_get(&cart, 14, 0, mon, &len), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_mon_get(&cart, 0, 30, mon, &len), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 6, mon, &len), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_mon_put(&cart, 14, 0, &p), Gen3CartBadArg);
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 14, 0));
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 6));
}

TEST(test_job_party_append) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 3, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_party_count(&cart), 4);
    const Gen3SaveSpan cs = gen3_save_party_count_span(Gen3LinkGameRS);
    uint32_t size;
    const uint8_t* w = file(GEN3_CART_WORK_NAME, &size);
    ASSERT_TRUE(w != NULL);
    ASSERT_EQ(w[cs.id * SECTOR_LEN + cs.off], 4);
    ASSERT_TRUE(work_differs_only_in(1));
    ASSERT_EQ(cart.dirty, 0x0002);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 3));
    /* past the count: refused, nothing changes */
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 5, &p), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_party_count(&cart), 4);
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    /* up to six */
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 4, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 5, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_party_count(&cart), 6);
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 6, &p), Gen3CartBadArg);
}

TEST(test_job_discard) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartOk);
    ASSERT_TRUE(work_is(flash(), &spec, 0));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(cart.dirty, 0);
    ASSERT_FALSE(gen3_cart_mon_changed(&cart, 1, 19));
    ASSERT_TRUE(gen3_cart_browsable(&cart));
}

TEST(test_job_box_name) {
    ASSERT_TRUE(backed_up());
    char name[9];
    ASSERT_TRUE(gen3_cart_box_name(&cart, 0, name));
    ASSERT_STR_EQ(name, "BOX 1");
    ASSERT_FALSE(gen3_cart_box_name(&cart, 14, name));
    ASSERT_STR_EQ(name, "");
}

/* A store failure while putting: the working copy is no longer trusted until a discard */
TEST(test_job_mon_put_sd_error) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    fake_store_fail_write_after(&store, 0);
    ASSERT_TRUE(gen3_cart_work_ok(&cart));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartSdError);
    ASSERT_FALSE(gen3_cart_work_ok(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    /* not the save's fault: the backup is current, and its save one to write */
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    Gen3CartRefuse why = Gen3CartRefuseNone;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseSave);
    /* even a first edit that failed counts as a change: Discard and the leave warning show */
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, 1, 19));
    ASSERT_EQ(cart.dirty, 0x0060);
    uint8_t mon[100];
    size_t len;
    ASSERT_EQ(gen3_cart_mon_get(&cart, 1, 19, mon, &len), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartBadArg);
    fake_store_fail_write_after(&store, 1000);
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_work_ok(&cart));
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    ASSERT_TRUE(work_is(flash(), &spec, 0));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(cart.dirty, 0);
}

/* The working copy is read back after it is made: a sector the card stored wrong, or can't give
 * back, fails it */
TEST(test_job_work_copy_read_back) {
    /* while backing up: the backup isn't current, its .sav stays */
    ASSERT_TRUE(connected());
    fake_store_corrupt_write(&store, GEN3_CART_WORK_NAME, 3 * SECTOR_LEN + 7);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartVerifyFailed);
    ASSERT_TRUE(only_savs(1));
    ASSERT_TRUE(sav_is_flash(BACKUP_NAME));
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    /* discarding: the backup stays, the working copy goes until a discard works */
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    fake_store_corrupt_write(&store, GEN3_CART_WORK_NAME, 13 * SECTOR_LEN + 0x7CF);
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartVerifyFailed);
    uint32_t size;
    ASSERT_TRUE(file(GEN3_CART_WORK_NAME, &size) == NULL);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
    /* 14 sectors read from the .sav, then the read-back's first fails */
    fake_store_fail_read(&store, 14);
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartSdError);
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartOk);
    ASSERT_TRUE(work_is(flash(), &spec, 0));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
}

/* ---- ROM copy ---- */

TEST(test_job_rom_dump) {
    ASSERT_TRUE(connected());
    char name[GEN3_CART_NAME_MAX];
    gen3_cart_rom_name(&cart, name);
    ASSERT_STR_EQ(name, ROM_NAME);
    uint32_t done = 1;
    ASSERT_FALSE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, 0);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
    uint32_t size;
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 256);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 4);
    ASSERT_EQ(fake_store_count(&store, ".part"), 0);
    ASSERT_EQ(prog.last_done[Gen3CartStepRomRead], MIB);
    ASSERT_EQ(prog.last_total[Gen3CartStepRomRead], MIB);
    ASSERT_EQ(prog.calls[Gen3CartStepRomVerify], 4);
    /* a second copy replaces the first */
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
    ASSERT_EQ(fake_store_count(&store, ".gba"), 1);
}

TEST(test_job_rom_resume) {
    ASSERT_TRUE(connected());
    prog.cancel_step = Gen3CartStepRomRead;
    prog.cancel_at = 300u * KIB;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartCancelled);
    uint32_t size;
    const uint8_t* part = file(PART_NAME, &size);
    ASSERT_TRUE(part != NULL);
    ASSERT_EQ(size, REGION);
    ASSERT_TRUE(is_rom(part, size));
    uint32_t done = 0;
    ASSERT_TRUE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, REGION);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    memset(hook.calls, 0, sizeof(hook.calls));
    ASSERT_EQ(gen3_cart_rom_dump(&cart, true), Gen3CartOk);
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 192);
    ASSERT_EQ(fake_store_count(&store, ".part"), 0);
    ASSERT_FALSE(gen3_cart_rom_part(&cart, &done));
}

/* Swaps in a cart with the same code and version as the default one: the title t in its header,
 * and rom_size, the ROM pattern the same; then a whole connect */
static bool swap_rom(const char* t, uint32_t rom_size) {
    helper_cmd_header(hdr, gba.own, t, "AXVE", 0);
    fake_gba_rom(&gba, hdr, rom_size);
    fake_gba_rom_string(&gba, LIB_OFF, LIB_STRING);
    return gen3_cart_connect(&cart) == Gen3CartOk;
}

/* A .part resumes only on the cart it started on: another cart with the same code and version but
 * another header (title, say) or ROM size finds none, and copies from 0, leaving the first one's
 * .part as it was, which that cart still resumes */
TEST(test_job_rom_resume_other_cart) {
    ASSERT_TRUE(connected());
    prog.cancel_step = Gen3CartStepRomRead;
    prog.cancel_at = 300u * KIB;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartCancelled);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    static char first[GEN3_CART_NAME_MAX];
    snprintf(first, sizeof(first), "%s", PART_NAME);
    uint32_t size = 0;
    ASSERT_TRUE(file(first, &size) != NULL);
    ASSERT_EQ(size, REGION);
    /* the title differs */
    ASSERT_TRUE(swap_rom("POKEMON RUBX", MIB));
    uint32_t done = 1;
    ASSERT_FALSE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, 0);
    memset(hook.calls, 0, sizeof(hook.calls));
    ASSERT_EQ(gen3_cart_rom_dump(&cart, true), Gen3CartOk);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 256);
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    ASSERT_TRUE(file(first, &size) != NULL);
    ASSERT_EQ(size, REGION);
    ASSERT_EQ(fake_store_count(&store, ".part"), 1);
    /* the first cart's header, another ROM size */
    ASSERT_TRUE(swap_rom("POKEMON RUBY", 2u * MIB));
    ASSERT_FALSE(gen3_cart_rom_part(&cart, &done));
    /* the first cart again */
    ASSERT_TRUE(swap_rom("POKEMON RUBY", MIB));
    ASSERT_STR_EQ(PART_NAME, first);
    ASSERT_TRUE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, REGION);
}

/* A resumed copy needs room only for the rest */
TEST(test_job_rom_resume_space) {
    ASSERT_TRUE(connected());
    prog.cancel_step = Gen3CartStepRomRead;
    prog.cancel_at = 300u * KIB;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartCancelled);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    /* the .part's region, then 800,000 bytes free: enough for the other three and a sector */
    store.capacity = REGION + 800000u;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, true), Gen3CartOk);
    uint32_t size;
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_TRUE(is_rom(rom, MIB));
    /* from the start, the same room doesn't do */
    ASSERT_TRUE(api.remove(api.ctx, ROM_NAME));
    store.capacity = 800000u;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartSdFull);
}

/* A copy from the start removes the .part first: its room counts as free, and nothing of it is
 * kept */
TEST(test_job_rom_restart_removes_part) {
    ASSERT_TRUE(connected());
    prog.cancel_step = Gen3CartStepRomRead;
    prog.cancel_at = 600u * KIB;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartCancelled);
    uint32_t size = 0;
    ASSERT_TRUE(file(PART_NAME, &size) != NULL);
    ASSERT_EQ(size, 2u * REGION);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    /* room for the whole ROM and a sector only once the .part has gone */
    store.capacity = MIB + SECTOR_LEN + 1000u;
    memset(hook.calls, 0, sizeof(hook.calls));
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 256);
    ASSERT_EQ(fake_store_count(&store, ".part"), 0);
    /* not linked: nothing is removed */
    prog.cancel_step = Gen3CartStepRomRead;
    prog.cancel_at = 300u * KIB;
    store.capacity = 64u * MIB;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartCancelled);
    cancel = false;
    prog.cancel_step = Gen3CartStepNone;
    cart.linked = false;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    ASSERT_TRUE(file(PART_NAME, &size) != NULL);
    ASSERT_EQ(size, REGION);
}

/* The card gives a region back differently: read once more, then VerifyFailed */
TEST(test_job_rom_sd_readback_differs) {
    ASSERT_TRUE(connected());
    fake_store_corrupt_read(&store, PART_NAME, REGION + 5u);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartVerifyFailed);
    uint32_t size;
    const uint8_t* part = file(PART_NAME, &size);
    ASSERT_TRUE(part != NULL);
    ASSERT_EQ(size, REGION);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 192);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 3);
}

/* A power cut can leave a region written but not checked: a resume checks the last one again */
TEST(test_job_rom_resume_checks_last_region) {
    ASSERT_TRUE(connected());
    /* two regions, the second one wrong, and a part of a third */
    void* f = api.open(api.ctx, PART_NAME, Gen3StoreCreate);
    ASSERT_TRUE(f != NULL);
    for(uint32_t off = 0; off < 2u * REGION + SECTOR_LEN; off += SECTOR_LEN) {
        for(uint32_t i = 0; i < SECTOR_LEN; i++) chunk[i] = rom_byte(off + i);
        if(off == REGION + 0x10000u) chunk[5] ^= 0x40;
        ASSERT_TRUE(api.write(api.ctx, f, off, chunk, SECTOR_LEN));
    }
    ASSERT_TRUE(api.close(api.ctx, f));
    uint32_t done = 0;
    ASSERT_TRUE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, 2u * REGION);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, true), Gen3CartOk);
    uint32_t size;
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    /* region 1 read again, region 0 kept */
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 192);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 4);
}

/* A region that comes over wrong once is read again */
TEST(test_job_rom_region_retry) {
    ASSERT_TRUE(connected());
    /* armed now: CART_INFO's scan reads the ROM too */
    fake_gba_rom_glitch_once(&gba, 0x40010);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartOk);
    uint32_t size;
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_EQ(size, MIB);
    ASSERT_TRUE(is_rom(rom, size));
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 320);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 5);
}

/* Wrong twice: VerifyFailed, and the .part keeps the regions before it */
TEST(test_job_rom_region_fails_twice) {
    ASSERT_TRUE(connected());
    fake_gba_rom_glitch_once(&gba, 0x40010);
    /* armed again once region 1's first check has run: its second read is wrong too */
    hook.glitch_after_crc = 2;
    hook.glitch_off = 0x40010;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartVerifyFailed);
    uint32_t size;
    const uint8_t* part = file(PART_NAME, &size);
    ASSERT_TRUE(part != NULL);
    ASSERT_EQ(size, REGION);
    ASSERT_TRUE(is_rom(part, size));
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 192);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_CRC32), 3);
    uint32_t n;
    ASSERT_TRUE(file(ROM_NAME, &n) == NULL);
}

TEST(test_job_rom_sd_full) {
    ASSERT_TRUE(connected());
    fake_store_init(&store, 500000);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartSdFull);
    ASSERT_EQ(calls(GEN3_HCMD_ROM_READ), 0);
    ASSERT_EQ(fake_store_count(&store, ""), 0);
}

/* A lost link keeps only the regions already checked */
TEST(test_job_rom_link_lost) {
    ASSERT_TRUE(connected());
    prog.unplug_step = Gen3CartStepRomRead;
    prog.unplug_at = 600u * KIB;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    uint32_t size;
    const uint8_t* part = file(PART_NAME, &size);
    ASSERT_TRUE(part != NULL);
    ASSERT_EQ(size, 2u * REGION);
    ASSERT_TRUE(is_rom(part, size));
    uint32_t done = 0;
    ASSERT_TRUE(gen3_cart_rom_part(&cart, &done));
    ASSERT_EQ(done, 2u * REGION);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, true), Gen3CartOk);
    const uint8_t* rom = file(ROM_NAME, &size);
    ASSERT_TRUE(rom != NULL);
    ASSERT_TRUE(is_rom(rom, MIB));
}

/* ---- The link ---- */

/* Linked once a connect ran to its end, until a command is lost */
TEST(test_job_linked) {
    ASSERT_TRUE(setup());
    ASSERT_FALSE(gen3_cart_linked(&cart));
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_linked(&cart));
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_linked(&cart));
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + 3000, FOREVER);
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    ASSERT_FALSE(gen3_cart_linked(&cart));
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_linked(&cart));
}

/* ---- Opening a save's Pokemon in the editor, and putting one back ---- */

/* The app's buffers for it, which it keeps in its heap state */
static uint8_t orig[GEN3_PARTY_SIZE];
static size_t orig_len;
static Gen3Pkm scratch;
static uint8_t rmon[GEN3_PARTY_SIZE];
static uint8_t crafted_party[6][100];
static uint8_t crafted_box[5][100];
static Gen3Editor keep;

static void mod_mail_item(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldHeldItem, GEN3_ITEM_MAIL_FIRST);
    gen3_pkm_set(p, Gen3FieldMail, 0);
}
static void mod_mail_index(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldMail, 2);
}
/* A party stat that isn't what level, IVs, EVs and nature give: the editor recalculates it */
static void mod_odd_stat(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldAtk, gen3_pkm_get(p, Gen3FieldAtk) + 7u);
}
static void mod_egg(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldIsEgg, 1);
}
static void mod_bad_egg(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldBadEgg, 1);
}
static void mod_enigma(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldHeldItem, GEN3_ITEM_ENIGMA_BERRY);
}
/* An item FireRed has and Ruby hasn't */
static void mod_frlg_item(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldHeldItem, GEN3_RUBY_ITEMS_COUNT);
}
static void mod_bad_move(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldMove2, GEN3_MOVE_COUNT + 45u);
}
/* An empty first move slot before a used one: the editor packs the moves */
static void mod_move_gap(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldMove1, 0);
    gen3_pkm_set(p, Gen3FieldMove2, 43);
    gen3_pkm_set(p, Gen3FieldPp2, 30);
}
/* No move at all: the editor gives the default moveset, from the bulk file */
static void mod_no_moves(Gen3Pkm* p) {
    for(int i = 0; i < 4; i++) gen3_pkm_set(p, Gen3FieldMove1 + i, 0);
}

/* The golden Treecko changed by f (NULL: as it is), in game form */
static bool crafted(void (*f)(Gen3Pkm*), uint8_t out[100]) {
    uint8_t g[100];
    Gen3Pkm p;
    golden_game(g);
    if(!gen3_save_mon_to_pkm(g, 100, &p)) return false;
    if(f) f(&p);
    gen3_save_pkm_to_mon(&p, 100, out);
    return true;
}

/* The default save, with a party of six crafted Pokemon: the golden Treecko, a Mail item, a mail
 * index, an odd stat, an egg and a Bad Egg; and box 2's slots 0..4: the Enigma Berry, a FireRed
 * item, a move out of range, a gap in the moves and no moves. Backed up. */
static bool crafted_backed_up(void) {
    if(!default_save()) return false;
    void (*const party_mods[6])(
        Gen3Pkm*) = {NULL, mod_mail_item, mod_mail_index, mod_odd_stat, mod_egg, mod_bad_egg};
    void (*const box_mods[5])(
        Gen3Pkm*) = {mod_enigma, mod_frlg_item, mod_bad_move, mod_move_gap, mod_no_moves};
    for(uint8_t k = 0; k < 6; k++) {
        if(!crafted(party_mods[k], crafted_party[k])) return false;
    }
    for(uint8_t k = 0; k < 5; k++) {
        if(!crafted(box_mods[k], crafted_box[k])) return false;
        fake_save_put_box_mon(img, &spec, 0, 2, k, crafted_box[k]);
    }
    fake_save_put_party(img, &spec, 0, 6, (const uint8_t(*)[100])crafted_party);
    insert("AXVE", MIB, FakeSaveFlash);
    return gen3_cart_connect(&cart) == Gen3CartOk && gen3_cart_backup(&cart, STAMP) == Gen3CartOk;
}

static Gen3CartMon open_at(uint8_t box, uint8_t slot) {
    return gen3_cart_mon_open(&cart, &ed, box, slot, orig, &orig_len, &scratch);
}

static Gen3CartMon close_at(uint8_t box, uint8_t slot, bool* put) {
    return gen3_cart_mon_close(&cart, &ed, box, slot, orig, orig_len, rmon, &scratch, put);
}

/* The slot's bytes in the working copy equal want's first len */
static bool slot_is(uint8_t box, uint8_t slot, const uint8_t* want, size_t len) {
    uint8_t m[100];
    size_t n = 0;
    return gen3_cart_mon_get(&cart, box, slot, m, &n) == Gen3CartOk && n == len &&
           memcmp(m, want, len) == 0;
}

/* Opened and closed with no edit, nothing is put: the bytes compared are the editor's, after it
 * normalised them (stats recalculated, moves packed, the default moveset), not the save's */
TEST(test_mon_open_close_unchanged) {
    ASSERT_TRUE(crafted_backed_up());
    static const struct {
        uint8_t box, slot;
        size_t len;
        bool normalised; /* the editor's bytes differ from the save's */
    } at[] = {
        {GEN3_SAVE_PARTY, 0, 100, false},
        {GEN3_SAVE_PARTY, 3, 100, true},
        {1, 19, 80, false},
        {2, 3, 80, true},
        {2, 4, 80, true},
    };
    for(size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        uint8_t saved[100];
        size_t n = 0;
        ASSERT_EQ(gen3_cart_mon_get(&cart, at[i].box, at[i].slot, saved, &n), Gen3CartOk);
        ASSERT_EQ(open_at(at[i].box, at[i].slot), Gen3CartMonOk);
        ASSERT_EQ(orig_len, at[i].len);
        /* the editor holds it, and orig is the editor's bytes */
        gen3_save_pkm_to_mon(&ed.pkm, orig_len, rmon);
        ASSERT_MEM_EQ(rmon, orig, orig_len);
        ASSERT_EQ(memcmp(orig, saved, orig_len) != 0, at[i].normalised);
        bool put = true;
        ASSERT_EQ(close_at(at[i].box, at[i].slot, &put), Gen3CartMonOk);
        ASSERT_FALSE(put);
        ASSERT_TRUE(slot_is(at[i].box, at[i].slot, saved, at[i].len));
    }
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(cart.dirty, 0);
    ASSERT_TRUE(work_is(flash(), &spec, 0));
}

/* What the game can't take back, or the Flipper can't write back, isn't opened: Mail (its letter
 * is a separate record), an item Ruby has no entry for, a move out of range, eggs. The editor is
 * left as it was. */
TEST(test_mon_open_refused) {
    ASSERT_TRUE(crafted_backed_up());
    static const struct {
        uint8_t box, slot;
        Gen3CartMon want;
    } at[] = {
        {GEN3_SAVE_PARTY, 1, Gen3CartMonMail},
        {GEN3_SAVE_PARTY, 2, Gen3CartMonMail},
        {GEN3_SAVE_PARTY, 4, Gen3CartMonEgg},
        {GEN3_SAVE_PARTY, 5, Gen3CartMonBadEgg},
        {2, 0, Gen3CartMonItem},
        {2, 1, Gen3CartMonItem},
        {2, 2, Gen3CartMonBadData},
        /* not a Pokemon to open */
        {2, 5, Gen3CartMonEmpty},
        {GEN3_SAVE_PARTY, 6, Gen3CartMonBadArg},
        {14, 0, Gen3CartMonBadArg},
        {2, 30, Gen3CartMonBadArg},
    };
    for(size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        keep = ed;
        ASSERT_EQ(open_at(at[i].box, at[i].slot), at[i].want);
        ASSERT_MEM_EQ(&ed, &keep, sizeof(ed));
    }
    /* a checksum that fails is a Bad Egg to the game */
    ASSERT_TRUE(default_save());
    uint8_t broken[100];
    ASSERT_TRUE(crafted(NULL, broken));
    broken[0x20] ^= 0x01;
    fake_save_put_box_mon(img, &spec, 0, 3, 0, broken);
    insert("AXVE", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    keep = ed;
    ASSERT_EQ(open_at(3, 0), Gen3CartMonBadEgg);
    ASSERT_MEM_EQ(&ed, &keep, sizeof(ed));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
}

/* The party: its slots up to the count hold Pokemon, the one after is free, and past that nothing */
TEST(test_mon_open_party_slots) {
    ASSERT_TRUE(backed_up());
    ASSERT_EQ(open_at(GEN3_SAVE_PARTY, 2), Gen3CartMonOk);
    ASSERT_EQ(open_at(GEN3_SAVE_PARTY, 3), Gen3CartMonEmpty);
    ASSERT_EQ(open_at(GEN3_SAVE_PARTY, 4), Gen3CartMonBadArg);
    ASSERT_EQ(open_at(0, 0), Gen3CartMonEmpty);
    /* nothing to open while not browsable */
    fake_store_fail_write_after(&store, 0);
    Gen3Pkm p;
    ASSERT_TRUE(level50(&p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartSdError);
    ASSERT_EQ(open_at(1, 18), Gen3CartMonBadArg);
}

/* The editor can't load it (the bulk file unreadable, for the default moveset): NoData, nothing is
 * opened, staged or changed, and the editor keeps byte for byte what it held (an edit of another
 * slot's Pokemon). Once the file reads again, the slot opens. */
TEST(test_mon_open_needs_bulk) {
    ASSERT_TRUE(crafted_backed_up());
    ASSERT_EQ(open_at(GEN3_SAVE_PARTY, 0), Gen3CartMonOk);
    gen3_edit_set_level(&ed, 60);
    keep = ed;
    bulk_file_failing = true;
    ASSERT_EQ(open_at(2, 4), Gen3CartMonNoData);
    ASSERT_MEM_EQ(&ed, &keep, sizeof(ed));
    ASSERT_TRUE(bulk_file_open());
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(open_at(2, 4), Gen3CartMonOk);
}

/* An edit is put back where it came from, in the slot's form */
TEST(test_mon_close_puts_edit) {
    ASSERT_TRUE(backed_up());
    bool put = false;
    ASSERT_EQ(open_at(GEN3_SAVE_PARTY, 0), Gen3CartMonOk);
    gen3_edit_set_level(&ed, 50);
    ASSERT_EQ(close_at(GEN3_SAVE_PARTY, 0, &put), Gen3CartMonOk);
    ASSERT_TRUE(put);
    gen3_save_pkm_to_mon(&ed.pkm, 100, rmon);
    ASSERT_TRUE(slot_is(GEN3_SAVE_PARTY, 0, rmon, 100));
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, GEN3_SAVE_PARTY, 0));
    ASSERT_EQ(open_at(1, 19), Gen3CartMonOk);
    gen3_edit_set_level(&ed, 40);
    put = false;
    ASSERT_EQ(close_at(1, 19, &put), Gen3CartMonOk);
    ASSERT_TRUE(put);
    gen3_save_pkm_to_mon(&ed.pkm, 80, rmon);
    ASSERT_TRUE(slot_is(1, 19, rmon, 80));
    ASSERT_EQ(gen3_cart_changes(&cart), 2);
    ASSERT_EQ(gen3_cart_party_count(&cart), 3);
}

/* The slot's Pokemon, decrypted */
static bool slot_pkm(uint8_t box, uint8_t slot, Gen3Pkm* out) {
    uint8_t m[100];
    size_t n = 0;
    return gen3_cart_mon_get(&cart, box, slot, m, &n) == Gen3CartOk &&
           gen3_save_mon_to_pkm(m, n, out);
}

/* A species changed in the editor makes a new Pokemon, met in Ruby: put back, it is this cart's
 * game's again. Any other edit keeps where it was met. */
static void mod_met_sapphire(Gen3Pkm* p) {
    gen3_pkm_set(p, Gen3FieldMetGame, 1);
}

TEST(test_mon_close_species_met_game) {
    /* a FireRed save whose party slot 0 and box 2 slot 20 hold a Pokemon met in Sapphire */
    ASSERT_TRUE(default_save_for(FakeGameFRLG));
    ASSERT_TRUE(crafted(mod_met_sapphire, party[0]));
    fake_save_put_party(img, &spec, 0, 3, (const uint8_t(*)[100])party);
    fake_save_put_box_mon(img, &spec, 0, 1, 19, party[0]);
    insert("BPRE", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->met_game, 4);
    Gen3Pkm p;
    ASSERT_TRUE(slot_pkm(GEN3_SAVE_PARTY, 0, &p));
    const uint32_t met = gen3_pkm_get(&p, Gen3FieldMetGame);
    const uint32_t species = gen3_pkm_get(&p, Gen3FieldSpecies);
    ASSERT_EQ(met, 1);
    bool put = false;
    /* a level: met where it was */
    ASSERT_EQ(open_at(GEN3_SAVE_PARTY, 0), Gen3CartMonOk);
    gen3_edit_set_level(&ed, 50);
    ASSERT_EQ(close_at(GEN3_SAVE_PARTY, 0, &put), Gen3CartMonOk);
    ASSERT_TRUE(put);
    ASSERT_TRUE(slot_pkm(GEN3_SAVE_PARTY, 0, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldMetGame), met);
    /* another species: FireRed */
    ASSERT_EQ(open_at(GEN3_SAVE_PARTY, 0), Gen3CartMonOk);
    ASSERT_TRUE(gen3_edit_set_species(&ed, 255));
    ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldMetGame), GEN3_EDIT_MET_GAME);
    ASSERT_EQ(close_at(GEN3_SAVE_PARTY, 0, &put), Gen3CartMonOk);
    ASSERT_TRUE(put);
    ASSERT_TRUE(slot_pkm(GEN3_SAVE_PARTY, 0, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldMetGame), 4);
    ASSERT_TRUE(gen3_pkm_get(&p, Gen3FieldSpecies) != species);
    gen3_save_pkm_to_mon(&ed.pkm, 100, rmon);
    ASSERT_TRUE(slot_is(GEN3_SAVE_PARTY, 0, rmon, 100));
    /* one met in Ruby already: its species tells */
    ASSERT_TRUE(slot_pkm(GEN3_SAVE_PARTY, 1, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldMetGame), GEN3_EDIT_MET_GAME);
    ASSERT_EQ(open_at(GEN3_SAVE_PARTY, 1), Gen3CartMonOk);
    ASSERT_TRUE(gen3_edit_set_species(&ed, 255));
    ASSERT_EQ(close_at(GEN3_SAVE_PARTY, 1, &put), Gen3CartMonOk);
    ASSERT_TRUE(put);
    ASSERT_TRUE(slot_pkm(GEN3_SAVE_PARTY, 1, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldMetGame), 4);
    /* another species and back again: still a new Pokemon, met in Ruby by the editor */
    ASSERT_EQ(open_at(1, 19), Gen3CartMonOk);
    ASSERT_TRUE(slot_pkm(1, 19, &p));
    const uint32_t box_species = gen3_pkm_get(&p, Gen3FieldSpecies);
    const uint16_t national = gen3_edit_national(&ed);
    ASSERT_TRUE(gen3_edit_set_species(&ed, national == 252 ? 255 : 252));
    ASSERT_TRUE(gen3_edit_set_species(&ed, national));
    ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldSpecies), box_species);
    ASSERT_EQ(close_at(1, 19, &put), Gen3CartMonOk);
    ASSERT_TRUE(put);
    ASSERT_TRUE(slot_pkm(1, 19, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldMetGame), 4);
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldSpecies), box_species);
}

/* An edited Pokemon the offer rules refuse, or gen3_pkm_check, isn't put; the slot keeps its own */
TEST(test_mon_close_refused) {
    ASSERT_TRUE(backed_up());
    void (*const mods[])(
        Gen3Pkm*) = {mod_enigma, mod_frlg_item, mod_mail_item, mod_bad_move, mod_egg, mod_bad_egg};
    const Gen3CartMon want[] = {
        Gen3CartMonItem,
        Gen3CartMonItem,
        Gen3CartMonMail,
        Gen3CartMonBadData,
        Gen3CartMonEgg,
        Gen3CartMonBadEgg};
    uint8_t saved[100];
    size_t n = 0;
    ASSERT_EQ(gen3_cart_mon_get(&cart, 1, 19, saved, &n), Gen3CartOk);
    for(size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); i++) {
        ASSERT_EQ(open_at(1, 19), Gen3CartMonOk);
        mods[i](&ed.pkm);
        gen3_pkm_fix_checksum(&ed.pkm);
        bool put = true;
        ASSERT_EQ(close_at(1, 19, &put), want[i]);
        ASSERT_FALSE(put);
        ASSERT_TRUE(slot_is(1, 19, saved, 80));
    }
    /* a party Pokemon given a mail index */
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 1, saved, &n), Gen3CartOk);
    ASSERT_EQ(open_at(GEN3_SAVE_PARTY, 1), Gen3CartMonOk);
    mod_mail_index(&ed.pkm);
    bool put = true;
    ASSERT_EQ(close_at(GEN3_SAVE_PARTY, 1, &put), Gen3CartMonMail);
    ASSERT_FALSE(put);
    ASSERT_TRUE(slot_is(GEN3_SAVE_PARTY, 1, saved, 100));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(cart.dirty, 0);
}

/* A new Pokemon or a .pk3 goes only into a free slot: an empty box slot, or the party slot after
 * the last. A new one's MetGame is this cart's game, whatever a species change set; a .pk3 keeps
 * its own. */
TEST(test_mon_place) {
    ASSERT_TRUE(backed_up());
    Gen3SaveTrainer t;
    ASSERT_TRUE(gen3_cart_trainer(&cart, &t));
    ASSERT_TRUE(gen3_cart_new_mon(&ed, &t, gen3_cart_id(&cart)->met_game, 7));
    ASSERT_TRUE(gen3_edit_set_species(&ed, 252));
    gen3_edit_set_level(&ed, 10);
    /* taken: a Pokemon there, or a party slot that isn't the next one; nothing is put */
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, 1, 19, true, rmon), Gen3CartMonTaken);
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, GEN3_SAVE_PARTY, 1, true, rmon), Gen3CartMonTaken);
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, GEN3_SAVE_PARTY, 4, true, rmon), Gen3CartMonTaken);
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, 14, 0, true, rmon), Gen3CartMonBadArg);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(gen3_cart_party_count(&cart), 3);
    /* refused by the offer rules: nothing is put */
    keep = ed;
    mod_enigma(&ed.pkm);
    gen3_pkm_fix_checksum(&ed.pkm);
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, 2, 0, true, rmon), Gen3CartMonItem);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ed = keep;
    /* a box slot */
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, 2, 0, true, rmon), Gen3CartMonOk);
    gen3_save_pkm_to_mon(&ed.pkm, 80, rmon);
    ASSERT_TRUE(slot_is(2, 0, rmon, 80));
    ASSERT_TRUE(gen3_cart_mon_changed(&cart, 2, 0));
    /* the next party slot */
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, GEN3_SAVE_PARTY, 3, true, rmon), Gen3CartMonOk);
    ASSERT_EQ(gen3_cart_party_count(&cart), 4);
    gen3_save_pkm_to_mon(&ed.pkm, 100, rmon);
    ASSERT_TRUE(slot_is(GEN3_SAVE_PARTY, 3, rmon, 100));
    ASSERT_EQ(gen3_cart_changes(&cart), 2);
    /* now taken */
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, 2, 0, true, rmon), Gen3CartMonTaken);

    /* FireRed: a species change sets MetGame to Ruby; the put sets it to FireRed again */
    ASSERT_TRUE(default_save_for(FakeGameFRLG));
    insert("BPRE", MIB, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->met_game, 4);
    ASSERT_TRUE(gen3_cart_trainer(&cart, &t));
    ASSERT_TRUE(gen3_cart_new_mon(&ed, &t, gen3_cart_id(&cart)->met_game, 7));
    ASSERT_TRUE(gen3_edit_set_species(&ed, 252));
    ASSERT_EQ(gen3_pkm_get(&ed.pkm, Gen3FieldMetGame), GEN3_EDIT_MET_GAME);
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, 0, 4, true, rmon), Gen3CartMonOk);
    size_t n = 0;
    ASSERT_EQ(gen3_cart_mon_get(&cart, 0, 4, rmon, &n), Gen3CartOk);
    Gen3Pkm back;
    ASSERT_TRUE(gen3_save_mon_to_pkm(rmon, n, &back));
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldMetGame), 4);
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldSpecies), gen3_pkm_get(&ed.pkm, Gen3FieldSpecies));
    /* a .pk3 keeps where it came from */
    gen3_pkm_set(&ed.pkm, Gen3FieldMetGame, 3);
    gen3_pkm_fix_checksum(&ed.pkm);
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, 0, 5, false, rmon), Gen3CartMonOk);
    ASSERT_EQ(gen3_cart_mon_get(&cart, 0, 5, rmon, &n), Gen3CartOk);
    ASSERT_TRUE(gen3_save_mon_to_pkm(rmon, n, &back));
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldMetGame), 3);
}

/* ---- Results ---- */

TEST(test_result_texts) {
    static const char* const want[] = {
        [Gen3CartOk] = "Done.",
        [Gen3CartWrittenNoBackup] =
            "Written and verified,\nbut no new backup.\nBack up again first.",
        [Gen3CartNothing] = "No changes to write.",
        [Gen3CartCancelled] = "Stopped.",
        [Gen3CartLinkLost] = "Link lost. The cart\nstill loads its last\ngood save.",
        [Gen3CartBusyTimeout] = "The GBA stopped\nanswering. Cart keeps\nits last good save.",
        [Gen3CartHelperError] = "The helper reported\nan error. See\ngen3_cart.log.",
        [Gen3CartRefused] = "Not allowed for this\ncartridge or save.",
        [Gen3CartChanged] = "The cart changed\nsince the backup.\nBack up again.",
        [Gen3CartSdFull] = "SD card full.",
        [Gen3CartSdError] = "SD card error.",
        [Gen3CartVerifyFailed] = "Check failed: the\ndata read back\ndiffers.",
        [Gen3CartFlashError] = "Flash write failed.\nThe cart still loads\nits last good save.",
        [Gen3CartBadBackup] = "Not a valid backup\nfor this game.",
        [Gen3CartBadArg] = "Invalid request.",
        [Gen3CartNoHelper] = "Helper file missing.\nReinstall Pokemon\nCartridge.",
        [Gen3CartMultibootFailed] =
            "Power the GBA off,\nhold START+SELECT,\npower on, then Retry.",
        [Gen3CartLinkBusy] = "The link is busy.\nClose other link\napps, then Retry.",
    };
    for(int r = Gen3CartOk; r <= Gen3CartLinkBusy; r++) {
        const char* t = gen3_cart_result_text((Gen3CartResult)r);
        ASSERT_TRUE(t != NULL);
        ASSERT_STR_EQ(t, want[r]);
        ASSERT_TRUE(strlen(t) > 0);
        uint32_t lines = 1;
        uint32_t line = 0;
        for(const char* c = t; *c; c++) {
            if(*c == '\n') {
                lines++;
                line = 0;
            } else {
                line++;
                ASSERT_TRUE(line <= 21);
            }
        }
        ASSERT_TRUE(lines <= 3);
    }
    ASSERT_STR_EQ(gen3_cart_result_text((Gen3CartResult)(Gen3CartLinkBusy + 1)), "");
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_job_connect);
    RUN(test_job_connect_waits_out_busy);
    RUN(test_job_connect_late_helper);
    RUN(test_job_connect_after_boot);
    RUN(test_job_link_needs_connect);
    RUN(test_job_busy_timeout);
    RUN(test_job_connect_wrong_helper);
    RUN(test_job_connect_cancel);
    RUN(test_job_connect_partial);
    RUN(test_job_client_results);
    RUN(test_job_backup_ok);
    RUN(test_job_backup_nosave);
    RUN(test_job_backup_save_gone);
    RUN(test_job_backup_sd_full_precheck);
    RUN(test_job_backup_sd_full_midway);
    RUN(test_job_backup_sd_write_error);
    RUN(test_job_backup_readback_mismatch);
    RUN(test_job_backup_flash_unstable);
    RUN(test_job_backup_link_lost);
    RUN(test_job_backup_cancel);
    RUN(test_job_backup_cancel_verify);
    RUN(test_job_backup_store_faults);
    RUN(test_job_backup_japanese);
    RUN(test_job_backup_refused_sram);
    RUN(test_job_attach_unreadable);
    RUN(test_job_backup_not_strict_keeps_can_write);
    RUN(test_job_backup_padded_not_writable);
    RUN(test_job_backup_work_copy_fails);
    RUN(test_job_backup_name_taken);
    RUN(test_job_attach_keeps_changes);
    RUN(test_job_attach_after_game_save);
    RUN(test_job_attach_footer_changed);
    RUN(test_job_attach_footer_fields);
    RUN(test_job_connect_other_chip);
    RUN(test_job_attach_other_cart);
    RUN(test_job_mon_get_put);
    RUN(test_job_party_append);
    RUN(test_job_discard);
    RUN(test_job_box_name);
    RUN(test_job_mon_put_sd_error);
    RUN(test_job_work_copy_read_back);
    RUN(test_job_rom_dump);
    RUN(test_job_rom_resume);
    RUN(test_job_rom_resume_other_cart);
    RUN(test_job_rom_resume_space);
    RUN(test_job_rom_resume_checks_last_region);
    RUN(test_job_rom_restart_removes_part);
    RUN(test_job_rom_sd_readback_differs);
    RUN(test_job_rom_region_retry);
    RUN(test_job_rom_region_fails_twice);
    RUN(test_job_rom_sd_full);
    RUN(test_job_rom_link_lost);
    RUN(test_job_linked);
    RUN(test_mon_open_close_unchanged);
    RUN(test_mon_open_refused);
    RUN(test_mon_open_party_slots);
    RUN(test_mon_open_needs_bulk);
    RUN(test_mon_close_puts_edit);
    RUN(test_mon_close_species_met_game);
    RUN(test_mon_close_refused);
    RUN(test_mon_place);
    RUN(test_result_texts);
    TEST_EXIT();
}
