#include "test.h"
#include "fake_gba.h"
#include "fake_helper.h"
#include "fake_save.h"
#include "helper_cmd.h"
#include <stdbool.h>
#include <hp_cart.h>
#include <hp_core.h>
#include <hp_flash.h>
#include <hp_link.h>
#include <hp_save.h>
#include <hp_version.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/mlink_types.h>

/* The helper client against the helper's real core on the fake GBA, through the real packet pump
 * (fake_helper); the fake helper's own link behaviour; and the client's resend and parsing rules
 * against scripted exchanges. The default cart is the helper tests' one: English Ruby, 16 MiB, an MX
 * chip, slot A with counter 4 and id 0 at its first sector, slot B empty. */

#define MIB 0x100000u
#define SECTOR_LEN 4096u
#define MX 0x09C2u
#define FOREVER 10000000u /* an unplug longer than any test */
#define DEFER_PROGRAMS 4095u /* byte programs of a deferred sector write: all but 0xFF8 */
#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))

static FakeGba gba;
static FakeHelper fh;
static Gen3Hlink pump;
static Gen3Hcli cli;
static FakeSaveSpec spec;
static uint8_t hdr[0xC0];
static uint8_t before[FAKE_GBA_FLASH_LEN];
static uint8_t buf[SECTOR_LEN + 16];
static uint8_t img[SECTOR_LEN];
static uint8_t rom[0x2000];
static Gen3HCartInfo info;
static Gen3HFooter footers[32];

/* ---- Fixtures ---- */

static uint8_t* sector(uint32_t n) {
    return fake_gba_flash(&gba) + n * SECTOR_LEN;
}

static FakeGbaCounts counts(void) {
    FakeGbaCounts c;
    fake_gba_counts(&gba, &c);
    return c;
}

static uint16_t le16(const uint8_t* b) {
    return (uint16_t)(b[0] | b[1] << 8);
}

static uint32_t le32(const uint8_t* b) {
    return (uint32_t)le16(b) | (uint32_t)le16(b + 2) << 16;
}

static bool all_ff(const uint8_t* b, uint32_t n) {
    for(uint32_t i = 0; i < n; i++) {
        if(b[i] != 0xFF) return false;
    }
    return true;
}

/* multiboot-and-save.md §5.3 over len bytes: the test's own */
static uint16_t sum_ref(const uint8_t* b, uint32_t len) {
    uint32_t s = 0;
    for(uint32_t i = 0; i < len; i += 4) s += le32(b + i);
    return (uint16_t)((s >> 16) + s);
}

/* The fake ROM's halfword at an even offset past the header and away from its strings */
static uint16_t rom_pattern(uint32_t off) {
    return (uint16_t)((off * 31u) >> 3);
}

/* A packet's CRC over its words after the SOF, each as its low byte, then its high byte */
static uint32_t words_crc(const uint16_t* w, uint32_t n) {
    uint32_t reg = 0xFFFFFFFFu;
    for(uint32_t i = 0; i < n; i++) {
        const uint8_t le[2] = {(uint8_t)w[i], (uint8_t)(w[i] >> 8)};
        reg = gen3_crc32_update(reg, le, 2);
    }
    return ~reg;
}

/* A transport around the fake helper's: counts the exchanges and syncs, keeps each code's last
 * busy_us, and can lose the response of the next `drop` exchanges of one code after the helper ran
 * them (the client is told drop_as). */
typedef struct {
    Gen3HTransport inner;
    uint32_t exchanges;
    uint32_t syncs;
    uint32_t busy_us[256];
    uint8_t drop_code;
    uint32_t drop;
    Gen3HlinkFail drop_as;
    uint32_t dropped;
} Tap;

static Tap tap;

static Gen3HlinkFail
    tap_exchange(void* ctx, const Gen3HlinkReq* req, uint8_t* status, uint16_t* resp_words) {
    Tap* t = ctx;
    t->exchanges++;
    t->busy_us[req->code] = req->busy_us;
    const Gen3HlinkFail fail = t->inner.exchange(t->inner.ctx, req, status, resp_words);
    if(fail == Gen3HlinkFailNone && t->drop && req->code == t->drop_code) {
        t->drop--;
        t->dropped++;
        return t->drop_as;
    }
    return fail;
}

static bool tap_sync(void* ctx, uint8_t need, uint32_t timeout_us) {
    Tap* t = ctx;
    t->syncs++;
    return t->inner.sync(t->inner.ctx, need, timeout_us);
}

static void tap_drop(uint8_t code, uint32_t times, Gen3HlinkFail as) {
    tap.drop_code = code;
    tap.drop = times;
    tap.drop_as = as;
    tap.dropped = 0;
}

/* The fake helper's command hook: counts each code, and when `code` next runs, unplugs the cable
 * for `unplug` transfers from `unplug_after` transfers on and corrupts the reply `corrupt`
 * transfers on (if non-zero), XORs 0x01 into the flash byte at `flip` (if non-zero), so that
 * running that command again would see another save, and makes the command's modelled time
 * `busy_us` from the packet's last word (if non-zero). */
typedef struct {
    uint32_t calls[256];
    uint16_t last_arg0;
    bool armed;
    uint8_t code;
    uint32_t unplug;
    uint32_t unplug_after;
    uint32_t corrupt;
    uint32_t flip;
    uint32_t busy_us;
} Hook;

static Hook hook;

static void hook_cb(void* ctx, uint8_t code, uint16_t arg0) {
    Hook* k = ctx;
    k->calls[code]++;
    k->last_arg0 = arg0;
    if(!k->armed || code != k->code) return;
    k->armed = false;
    if(k->unplug) {
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + k->unplug_after, k->unplug);
    }
    if(k->corrupt) fake_helper_corrupt_at(&fh, fake_helper_transfers(&fh) + k->corrupt);
    if(k->flip) fake_gba_flash(&gba)[k->flip] ^= 0x01;
    /* heard_us is the packet's last word's time: the helper just heard it */
    if(k->busy_us) fh.busy_end_us = fh.heard_us + k->busy_us;
}

static void hook_arm(uint8_t code, uint32_t unplug, uint32_t corrupt) {
    hook.armed = true;
    hook.code = code;
    hook.unplug = unplug;
    hook.unplug_after = 0;
    hook.corrupt = corrupt;
    hook.flip = 0;
    hook.busy_us = 0;
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

/* Inserts a cart: its header, rom_size of ROM with FLASH1M_V103 at 0x3B0000 (and extra, if any, at
 * 0x500000), an MX chip holding spec's save. Starts the fake helper with the hook, and a client over
 * the tap; before = the flash. */
static void insert(const char* code, uint32_t rom_size, const char* extra) {
    fake_gba_init(&gba);
    helper_cmd_header(hdr, gba.own, "POKEMON RUBY", code, 0);
    fake_gba_rom(&gba, hdr, rom_size);
    fake_gba_rom_string(&gba, 0x3B0000, "FLASH1M_V103");
    if(extra) fake_gba_rom_string(&gba, 0x500000, extra);
    fake_gba_save(&gba, FakeSaveFlash, MX);
    fake_save_build(fake_gba_flash(&gba), &spec);
    memcpy(before, fake_gba_flash(&gba), sizeof(before));
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
}

static void setup(void) {
    default_spec();
    insert("AXVE", 16 * MIB, NULL);
}

/* The default cart, with the fake helper on that wire (insert starts it on MultiPlay) */
static void setup_on(MlinkWire wire) {
    setup();
    fake_helper_set_wire(&fh, wire);
}

/* The cart inserted, as the Flipper starts on it: connected, CART_INFO, SAVE_PROBE; the flash
 * counts start at 0. */
static bool identify(void) {
    Gen3HProbe probe;
    const bool ok = gen3_hcli_connect(&cli, NULL) == Gen3HcliOk &&
                    gen3_hcli_cart_info(&cli, &info) == Gen3HcliOk &&
                    gen3_hcli_save_probe(&cli, &probe) == Gen3HcliOk;
    memset(&gba.counts, 0, sizeof(gba.counts));
    return ok;
}

/* The default cart on that wire, identified */
static bool setup_identified_on(MlinkWire wire) {
    setup_on(wire);
    return identify();
}

static bool setup_identified(void) {
    return setup_identified_on(MlinkWireMulti);
}

/* Slot A's sector of id k with its counter set: an image as a game save would carry it. */
static void make_img(uint8_t id, uint32_t counter) {
    memcpy(img, before + fake_save_phys(&spec, 0, id) * SECTOR_LEN, SECTOR_LEN);
    for(uint32_t i = 0; i < 4; i++) img[0xFFC + i] = (uint8_t)(counter >> (8 * i));
}

/* The written sector holds img, the commit byte (0xFF8) as `commit` */
static bool holds_img(uint32_t s, uint8_t commit) {
    return memcmp(sector(s), img, 0xFF8) == 0 && sector(s)[0xFF8] == commit &&
           memcmp(sector(s) + 0xFF9, img + 0xFF9, SECTOR_LEN - 0xFF9) == 0;
}

/* ---- Driving the fake helper word by word ---- */

static bool last_lost;
static bool last_notready;
static uint16_t last_ticks;

/* One one-word stream frame: the reply (MLINK_NO_DATA if lost, with last_lost set, and on the
 * Normal wire last_notready too); last_ticks is the frame's time in ticks. */
static uint16_t xfer(uint16_t w) {
    const MlinkTx tx = {.count = 1, .word = {w}};
    MlinkRx rx;
    fake_helper_frame(&fh, &tx, &rx, MLINK_TICK_US);
    last_lost = (rx.timeout_mask & 1u) != 0;
    last_notready = (rx.notready_mask & 1u) != 0;
    last_ticks = rx.ticks;
    return rx.word[0];
}

/* An idle frame of us microseconds: no transfer. */
static void idle(uint32_t us) {
    const MlinkTx tx = {.count = 0};
    MlinkRx rx;
    fake_helper_frame(&fh, &tx, &rx, us);
}

/* HELLO, seq 1 */
static const uint16_t hello_req[] = {HP_SOF, 0x0101, 0x0000, 0xD24E, 0x983A};

/* The 9 words of a HELLO response (from its SOF) and the READY after it; false if any is wrong.
 * The Flipper's words meanwhile are junk: the helper doesn't read them. Slave id 1, and the boot
 * mode of the fake helper's wire: 2 on Normal, 3 on MultiPlay. */
static bool hello_response(uint8_t seq) {
    uint16_t r[9];
    for(uint32_t i = 0; i < 9; i++) r[i] = xfer(0x1234);
    const uint16_t want[7] = {
        HP_SOF,
        (uint16_t)(seq << 8),
        4,
        HP_PROTO_VERSION,
        HP_BUILD_ID,
        HP_MAX_PAYLOAD,
        fh.wire == MlinkWireNormal ? 0x0102 : 0x0103,
    };
    const uint32_t crc = words_crc(r + 1, 6);
    return memcmp(r, want, sizeof(want)) == 0 && r[7] == (uint16_t)crc &&
           r[8] == (uint16_t)(crc >> 16) && xfer(0x0000) == HP_READY;
}

/* ---- Scripted exchanges ---- */

#define SCRIPT_MAX 8u

typedef struct {
    Gen3HlinkFail fail;
    uint8_t status;
    uint16_t n;
} ScriptStep;

/* Each exchange takes the next step: a failure, or a response of n payload words (payload[]); a
 * response over resp_max_words is TooLong, as the pump makes it. Past the last step, NoReady. */
typedef struct {
    ScriptStep step[SCRIPT_MAX];
    uint32_t steps;
    uint32_t exchanges;
    Gen3HlinkReq req[SCRIPT_MAX];
    uint16_t payload[2048];
    bool sync_ok;
    uint32_t syncs;
    uint8_t sync_need;
    uint32_t sync_us;
} Script;

static Script script;

static Gen3HlinkFail
    script_exchange(void* ctx, const Gen3HlinkReq* req, uint8_t* status, uint16_t* resp_words) {
    Script* s = ctx;
    const uint32_t i = s->exchanges++;
    *status = 0;
    *resp_words = 0;
    if(i >= s->steps) return Gen3HlinkFailNoReady;
    s->req[i] = *req;
    const ScriptStep st = s->step[i];
    if(st.fail != Gen3HlinkFailNone) return st.fail;
    if(st.n > req->resp_max_words) return Gen3HlinkFailTooLong;
    for(uint16_t k = 0; k < st.n; k++) {
        req->resp[2 * k] = (uint8_t)s->payload[k];
        req->resp[2 * k + 1] = (uint8_t)(s->payload[k] >> 8);
    }
    *status = st.status;
    *resp_words = st.n;
    return Gen3HlinkFailNone;
}

static bool script_sync(void* ctx, uint8_t need, uint32_t timeout_us) {
    Script* s = ctx;
    s->syncs++;
    s->sync_need = need;
    s->sync_us = timeout_us;
    return s->sync_ok;
}

static void script_begin(void) {
    memset(&script, 0, sizeof(script));
    script.sync_ok = true;
    script.payload[0] = GEN3_HELPER_PROTO; /* HELLO's proto, if a step answers one */
    const Gen3HTransport t = {
        .ctx = &script,
        .exchange = script_exchange,
        .sync = script_sync,
    };
    gen3_hcli_init(&cli, &t);
}

static void script_add(Gen3HlinkFail fail, uint8_t status, uint16_t n) {
    script.step[script.steps++] = (ScriptStep){.fail = fail, .status = status, .n = n};
}

static bool same_req(const Gen3HlinkReq* a, const Gen3HlinkReq* b) {
    return a->code == b->code && a->seq == b->seq && a->nargs == b->nargs &&
           memcmp(a->args, b->args, sizeof(a->args)) == 0 && a->data == b->data &&
           a->data_words == b->data_words && a->resp == b->resp &&
           a->resp_max_words == b->resp_max_words && a->busy_us == b->busy_us;
}

static bool args_are(const Gen3HlinkReq* r, uint8_t code, const uint16_t* a, uint8_t n) {
    return r->code == code && r->nargs == n && memcmp(r->args, a, 2u * n) == 0;
}

/* ---- The two sides agree ---- */

TEST(test_hcli_codes_match_helper) {
    _Static_assert(GEN3_HELPER_PROTO == HP_PROTO_VERSION, "protocol version");
    _Static_assert(GEN3_HLINK_SOF == HP_SOF, "SOF");
    _Static_assert(GEN3_HLINK_READY == HP_READY, "READY");
    _Static_assert(GEN3_HLINK_BUSY == HP_BUSY, "BUSY");
    _Static_assert(GEN3_HLINK_RECV == HP_RECV, "RECV");
    _Static_assert(GEN3_HLINK_MAX_PAYLOAD == HP_MAX_PAYLOAD, "max payload");
    _Static_assert(GEN3_HCMD_HELLO == HP_CMD_HELLO, "HELLO");
    _Static_assert(GEN3_HCMD_CART_INFO == HP_CMD_CART_INFO, "CART_INFO");
    _Static_assert(GEN3_HCMD_SAVE_PROBE == HP_CMD_SAVE_PROBE, "SAVE_PROBE");
    _Static_assert(GEN3_HCMD_ARM_WRITE == HP_CMD_ARM_WRITE, "ARM_WRITE");
    _Static_assert(GEN3_HCMD_DISARM == HP_CMD_DISARM, "DISARM");
    _Static_assert(GEN3_HCMD_READ_SECTOR == HP_CMD_READ_SECTOR, "READ_SECTOR");
    _Static_assert(GEN3_HCMD_READ_FOOTERS == HP_CMD_READ_FOOTERS, "READ_FOOTERS");
    _Static_assert(GEN3_HCMD_SECTOR_CRC32 == HP_CMD_SECTOR_CRC32, "SECTOR_CRC32");
    _Static_assert(GEN3_HCMD_ERASE_SECTOR == HP_CMD_ERASE_SECTOR, "ERASE_SECTOR");
    _Static_assert(GEN3_HCMD_WRITE_SECTOR == HP_CMD_WRITE_SECTOR, "WRITE_SECTOR");
    _Static_assert(GEN3_HCMD_COPY_SECTOR == HP_CMD_COPY_SECTOR, "COPY_SECTOR");
    _Static_assert(GEN3_HCMD_COMMIT_SECTOR == HP_CMD_COMMIT_SECTOR, "COMMIT_SECTOR");
    _Static_assert(GEN3_HCMD_ROM_READ == HP_CMD_ROM_READ, "ROM_READ");
    _Static_assert(GEN3_HCMD_ROM_CRC32 == HP_CMD_ROM_CRC32, "ROM_CRC32");
    _Static_assert(GEN3_HST_OK == HP_ST_OK, "OK");
    _Static_assert(GEN3_HST_BAD_PACKET == HP_ST_BAD_PACKET, "BAD_PACKET");
    _Static_assert(GEN3_HST_BAD_ARG == HP_ST_BAD_ARG, "BAD_ARG");
    _Static_assert(GEN3_HST_UNSUPPORTED == HP_ST_UNSUPPORTED, "UNSUPPORTED");
    _Static_assert(GEN3_HST_NOT_ARMED == HP_ST_NOT_ARMED, "NOT_ARMED");
    _Static_assert(GEN3_HST_REFUSED == HP_ST_REFUSED, "REFUSED");
    _Static_assert(GEN3_HST_ERASE_FAIL == HP_ST_ERASE_FAIL, "ERASE_FAIL");
    _Static_assert(GEN3_HST_PROGRAM_FAIL == HP_ST_PROGRAM_FAIL, "PROGRAM_FAIL");
    _Static_assert(GEN3_HST_VERIFY_FAIL == HP_ST_VERIFY_FAIL, "VERIFY_FAIL");
    _Static_assert(GEN3_HST_NOT_ERASED == HP_ST_NOT_ERASED, "NOT_ERASED");
    _Static_assert(GEN3_HARM_MAGIC == HP_ARM_MAGIC, "ARM_WRITE magic");
    _Static_assert(GEN3_HFLAG_DEFER_COMMIT == HP_FLAG_DEFER_COMMIT, "DEFER_COMMIT");
    _Static_assert(GEN3_HCART_LOGO_OK == HP_CART_LOGO_OK, "logo flag");
    _Static_assert(GEN3_HCART_FIXED_OK == HP_CART_FIXED_OK, "fixed byte flag");
    _Static_assert(GEN3_HCART_COMPLEMENT_OK == HP_CART_COMPLEMENT_OK, "complement flag");
    _Static_assert(GEN3_HCART_OTHER_LIB == HP_CART_OTHER_LIB, "other library flag");
    _Static_assert(GEN3_HCART_WRITE_GAME == HP_CART_WRITE_GAME, "write game flag");
    _Static_assert(GEN3_HLIB_NONE == HpLibNone, "lib none");
    _Static_assert(GEN3_HLIB_FLASH1M == HpLibFlash1M, "lib FLASH1M_V");
    _Static_assert(GEN3_HLIB_FLASH == HpLibFlash, "lib FLASH_V");
    _Static_assert(GEN3_HLIB_FLASH512 == HpLibFlash512, "lib FLASH512_V");
    _Static_assert(GEN3_HLIB_SRAM == HpLibSram, "lib SRAM_V");
    _Static_assert(GEN3_HLIB_EEPROM == HpLibEeprom, "lib EEPROM_V");
    _Static_assert(GEN3_HCHIP_FLASH128 == HpChipFlash128, "chip 128 KB flash");
    _Static_assert(GEN3_HCHIP_FLASH64 == HpChipFlash64, "chip 64 KB flash");
    _Static_assert(GEN3_HCHIP_SRAM == HpChipSram, "chip SRAM");
    _Static_assert(GEN3_HCHIP_NONE == HpChipNone, "chip none");
    ASSERT_EQ(GEN3_HCMD_HELLO, HP_CMD_HELLO);
}

/* ---- The fake helper ---- */

/* Word by word, as the GBA's main loop runs: each reply is what the helper loaded after the
 * transfer before; the command runs with the packet's last word. */
TEST(test_fake_helper_words) {
    setup();
    ASSERT_EQ(xfer(0x0000), HP_READY);
    ASSERT_EQ(xfer(hello_req[0]), HP_READY);
    for(uint32_t i = 1; i < 5; i++) ASSERT_EQ(xfer(hello_req[i]), HP_RECV);
    ASSERT_EQ(hook.calls[HP_CMD_HELLO], 1);
    ASSERT_EQ(hook.last_arg0, 0);
    /* HELLO takes no time: one BUSY, the transfer the helper waits for, then the response */
    ASSERT_EQ(xfer(0x0000), HP_BUSY);
    ASSERT_TRUE(hello_response(1));
    ASSERT_EQ(fake_helper_transfers(&fh), 17);
    ASSERT_FALSE(last_lost);
    ASSERT_EQ(hook.calls[HP_CMD_HELLO], 1);
}

/* BUSY lasts the command's modelled time, whether or not the Flipper clocks meanwhile. */
TEST(test_fake_helper_busy_by_time) {
    ASSERT_TRUE(setup_identified());
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    uint16_t r[8];
    const uint16_t s14 = 14;
    const uint32_t len = helper_cmd_make(r, 0x70, HP_CMD_ERASE_SECTOR, &s14, 1);
    for(uint32_t i = 0; i < len; i++) xfer(r[i]);
    ASSERT_EQ(hook.last_arg0, 14);
    /* An erase is about 300 ms: BUSY for every transfer until then, and one more */
    uint32_t busy = 0;
    uint16_t w;
    while((w = xfer(0x0000)) == HP_BUSY && busy < 1000) busy++;
    ASSERT_EQ(w, HP_SOF);
    ASSERT_TRUE(busy >= 300000u / MLINK_TICK_US + 1u);
    ASSERT_TRUE(busy <= 300000u / MLINK_TICK_US + 8u);
    ASSERT_EQ(xfer(0x0000), 0x7000);
    for(uint32_t i = 0; i < 3; i++) xfer(0x0000);
    ASSERT_EQ(xfer(0x0000), HP_READY);
    ASSERT_EQ(counts().erases, 1);
    /* The cable idle for 400 ms: the erase is done, and the next transfer is the one it waits for */
    const uint16_t s15 = 15;
    helper_cmd_make(r, 0x71, HP_CMD_ERASE_SECTOR, &s15, 1);
    for(uint32_t i = 0; i < len; i++) xfer(r[i]);
    idle(400000u);
    ASSERT_EQ(xfer(0x0000), HP_BUSY);
    ASSERT_EQ(xfer(0x0000), HP_SOF);
    ASSERT_EQ(counts().erases, 2);
}

/* 200 ms without a transfer: a partial packet, or a response not yet out, is dropped. */
TEST(test_fake_helper_rx_timeout) {
    setup();
    /* 199 ms of silence inside a packet: it still completes */
    xfer(hello_req[0]);
    xfer(hello_req[1]);
    idle(198000u);
    for(uint32_t i = 2; i < 5; i++) ASSERT_EQ(xfer(hello_req[i]), HP_RECV);
    ASSERT_EQ(xfer(0x0000), HP_BUSY);
    ASSERT_TRUE(hello_response(1));
    ASSERT_EQ(hook.calls[HP_CMD_HELLO], 1);
    /* 200 ms: the packet is dropped, and its rest is junk to a helper hunting again */
    xfer(hello_req[0]);
    xfer(hello_req[1]);
    idle(199000u);
    for(uint32_t i = 2; i < 5; i++) ASSERT_EQ(xfer(hello_req[i]), HP_READY);
    ASSERT_EQ(xfer(0x0000), HP_READY);
    ASSERT_EQ(hook.calls[HP_CMD_HELLO], 1);
    /* The response's SOF went out, then 200 ms of silence: the rest of it is dropped */
    for(uint32_t i = 0; i < 5; i++) xfer(hello_req[i]);
    ASSERT_EQ(xfer(0x0000), HP_BUSY);
    ASSERT_EQ(xfer(0x0000), HP_SOF);
    idle(199000u);
    ASSERT_EQ(xfer(0x0000), HP_READY);
    ASSERT_EQ(hook.calls[HP_CMD_HELLO], 2);
}

TEST(test_fake_helper_unplug_corrupt) {
    setup();
    const uint32_t n0 = fake_helper_transfers(&fh);
    fake_helper_unplug_at(&fh, n0 + 1, 3);
    fake_helper_corrupt_at(&fh, n0 + 5);
    ASSERT_EQ(xfer(0x0000), HP_READY);
    ASSERT_FALSE(last_lost);
    /* The window's transfers time out and never reach the helper: a SOF sent into it starts nothing */
    ASSERT_EQ(xfer(HP_SOF), MLINK_NO_DATA);
    ASSERT_TRUE(last_lost);
    ASSERT_EQ(xfer(0x0101), MLINK_NO_DATA);
    ASSERT_TRUE(last_lost);
    xfer(0x0000);
    ASSERT_TRUE(last_lost);
    ASSERT_EQ(xfer(0x0000), HP_READY);
    ASSERT_FALSE(last_lost);
    ASSERT_EQ(xfer(0x0000), HP_READY ^ 1u);
    ASSERT_EQ(xfer(0x0000), HP_READY);
    /* Replug ends a window at once */
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), FOREVER);
    xfer(0x0000);
    ASSERT_TRUE(last_lost);
    fake_helper_replug(&fh);
    ASSERT_EQ(xfer(0x0000), HP_READY);
    /* In a full frame each transfer is on its own; an idle tick is no transfer */
    const uint32_t n1 = fake_helper_transfers(&fh);
    fake_helper_unplug_at(&fh, n1 + 3, 2);
    MlinkTx tx = {.count = MLINK_BURST_LEN};
    MlinkRx rx;
    fake_helper_frame(&fh, &tx, &rx, MLINK_BURST_LEN * MLINK_TICK_US);
    ASSERT_EQ(rx.count, MLINK_BURST_LEN);
    ASSERT_EQ(rx.timeout_mask, 0x0018);
    ASSERT_EQ(rx.framing_mask, 0);
    ASSERT_EQ(rx.word[2], HP_READY);
    ASSERT_EQ(rx.word[3], MLINK_NO_DATA);
    ASSERT_EQ(rx.word[5], HP_READY);
    tx.count = 0;
    fake_helper_frame(&fh, &tx, &rx, MLINK_TICK_US);
    ASSERT_EQ(rx.count, 0);
    ASSERT_EQ(fake_helper_transfers(&fh), n1 + MLINK_BURST_LEN);
}

/* A power cut ends the helper for good; a restart (the GBA power-cycled, the helper sent again)
 * keeps the flash and forgets everything else. */
TEST(test_fake_helper_power_cut_restart) {
    ASSERT_TRUE(setup_identified());
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    fake_gba_power_cut_after(&gba, 0);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliLinkLost);
    ASSERT_TRUE(gba.frozen);
    idle(1000000u);
    xfer(0x0000);
    ASSERT_TRUE(last_lost);
    ASSERT_MEM_EQ(fake_gba_flash(&gba), before, FAKE_GBA_FLASH_LEN);
    /* Left in bank 1 with a power cut still to come: both go */
    gba.bank = 1;
    fake_gba_power_cut_after(&gba, 3);
    fake_helper_restart(&fh);
    ASSERT_FALSE(gba.frozen);
    ASSERT_FALSE(gba.cut_armed);
    ASSERT_EQ(gba.bank, 0);
    ASSERT_EQ(gba.status, HpStatusReady);
    ASSERT_MEM_EQ(fake_gba_flash(&gba), before, FAKE_GBA_FLASH_LEN);
    /* A new helper: no probe, no cart checks, no arming */
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 3, buf), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_UNSUPPORTED);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_REFUSED);
    ASSERT_EQ(cli.last_fail_code, HP_REFUSE_CART);
    Gen3HProbe probe;
    ASSERT_EQ(gen3_hcli_cart_info(&cli, &info), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_save_probe(&cli, &probe), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 3, buf), Gen3HcliOk);
    ASSERT_MEM_EQ(buf, before + 3 * SECTOR_LEN, SECTOR_LEN);
    /* The chip takes a whole sector write: no cut comes */
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    make_img(0, 5);
    ASSERT_EQ(gen3_hcli_write_sector(&cli, 14, true, img), Gen3HcliOk);
    ASSERT_TRUE(holds_img(14, 0xFF));
}

/* On the Normal wire the helper sends no BUSY: it runs the command right after the packet's last
 * word, and while it runs the slave isn't ready, so every slot is not ready (nothing clocked,
 * MLINK_NORMAL_STALL_TICKS ticks each). The first slot delivered after it carries the response's
 * SOF. */
TEST(test_fake_helper_normal_busy_is_notready) {
    setup();
    /* MultiPlay, the default: a frame takes max(count, 1) ticks, and no slot is ever not ready */
    MlinkTx tx = {.count = MLINK_BURST_LEN};
    MlinkRx rx;
    fake_helper_frame(&fh, &tx, &rx, MLINK_BURST_LEN * MLINK_TICK_US);
    ASSERT_EQ(rx.ticks, MLINK_BURST_LEN);
    ASSERT_EQ(rx.notready_mask, 0);
    tx.count = 0;
    fake_helper_frame(&fh, &tx, &rx, MLINK_TICK_US);
    ASSERT_EQ(rx.ticks, 1);
    fake_helper_set_wire(&fh, MlinkWireNormal);
    /* HELLO, its modelled time made 100 ms */
    hook_arm(HP_CMD_HELLO, 0, 0);
    hook.busy_us = 100000u;
    ASSERT_EQ(xfer(hello_req[0]), HP_READY);
    ASSERT_EQ(last_ticks, 1);
    for(uint32_t i = 1; i < 5; i++) ASSERT_EQ(xfer(hello_req[i]), HP_RECV);
    ASSERT_EQ(hook.calls[HP_CMD_HELLO], 1);
    /* From the slot 1 tick after the last word, every 16 ticks, until the 100 ms are over: the
     * slots at 1, 17, 33, 49, 65 and 81 ticks */
    const uint32_t n0 = fake_helper_transfers(&fh);
    for(uint32_t i = 0; i < 6; i++) {
        ASSERT_EQ(xfer(0x1234), MLINK_NO_DATA);
        ASSERT_TRUE(last_notready);
        ASSERT_TRUE(last_lost);
        ASSERT_EQ(last_ticks, MLINK_NORMAL_STALL_TICKS);
    }
    /* At 97 ticks, the response from its SOF, then READY: every reply is pinned, none is BUSY */
    ASSERT_TRUE(hello_response(1));
    ASSERT_FALSE(last_notready);
    ASSERT_EQ(last_ticks, 1);
    ASSERT_EQ(fake_helper_transfers(&fh), n0 + 6 + 10);
    ASSERT_EQ(hook.calls[HP_CMD_HELLO], 1);
    /* The cable out: those slots are not ready too. A frame's ticks are 1 per delivered slot and
     * 16 per not-ready one; an idle tick is 1. */
    const uint32_t n1 = fake_helper_transfers(&fh);
    fake_helper_unplug_at(&fh, n1 + 3, 2);
    tx.count = MLINK_BURST_LEN;
    fake_helper_frame(&fh, &tx, &rx, MLINK_TICK_US);
    ASSERT_EQ(rx.notready_mask, 0x0018);
    ASSERT_EQ(rx.timeout_mask, 0x0018);
    ASSERT_EQ(rx.word[2], HP_READY);
    ASSERT_EQ(rx.word[3], MLINK_NO_DATA);
    ASSERT_EQ(rx.word[5], HP_READY);
    ASSERT_EQ(rx.ticks, 7 + 2 * MLINK_NORMAL_STALL_TICKS);
    tx.count = 0;
    fake_helper_frame(&fh, &tx, &rx, MLINK_TICK_US);
    ASSERT_EQ(rx.ticks, 1);
    ASSERT_EQ(fake_helper_transfers(&fh), n1 + MLINK_BURST_LEN);
    /* After a power cut, not ready for good; a restart keeps the wire: no BUSY after HELLO */
    gba.frozen = true;
    ASSERT_EQ(xfer(0x0000), MLINK_NO_DATA);
    ASSERT_TRUE(last_notready);
    ASSERT_EQ(last_ticks, MLINK_NORMAL_STALL_TICKS);
    fake_helper_restart(&fh);
    ASSERT_EQ(xfer(hello_req[0]), HP_READY);
    for(uint32_t i = 1; i < 5; i++) ASSERT_EQ(xfer(hello_req[i]), HP_RECV);
    ASSERT_TRUE(hello_response(1));
}

/* ---- Each command against the helper ---- */

/* test_hcli_connect's body on that wire (test_hcli_normal_roundtrip runs it on Normal) */
static void connect_on(MlinkWire wire) {
    setup_on(wire);
    Gen3HHello hello;
    memset(&hello, 0xEE, sizeof(hello));
    ASSERT_EQ(gen3_hcli_connect(&cli, &hello), Gen3HcliOk);
    ASSERT_EQ(hello.proto, 1);
    ASSERT_EQ(hello.build, HP_BUILD_ID);
    ASSERT_EQ(hello.max_payload, 2064);
    /* The boot mode the BIOS left at header 0xC4: 2 for a Normal-mode boot, 3 for MultiPlay */
    ASSERT_EQ(hello.boot_mode, wire == MlinkWireNormal ? 2 : 3);
    ASSERT_EQ(hello.slave_id, 1);
    ASSERT_EQ(tap.syncs, 1);
    ASSERT_EQ(tap.exchanges, 1);
    ASSERT_EQ(cli.resends, 0);
    ASSERT_EQ(cli.last_status, GEN3_HST_OK);
    ASSERT_EQ(hook.calls[HP_CMD_HELLO], 1);
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    /* The cable out: no READY, so LinkLost before any HELLO */
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), FOREVER);
    ASSERT_EQ(gen3_hcli_connect(&cli, &hello), Gen3HcliLinkLost);
    ASSERT_EQ(tap.syncs, 3);
    ASSERT_EQ(tap.exchanges, 2);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_hcli_connect(&cli, &hello), Gen3HcliOk);
    ASSERT_EQ(hook.calls[HP_CMD_HELLO], 3);
}

TEST(test_hcli_connect) {
    connect_on(MlinkWireMulti);
}

/* test_hcli_cart_info's body on that wire */
static void cart_info_on(MlinkWire wire) {
    setup_on(wire);
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    memset(&info, 0xEE, sizeof(info));
    ASSERT_EQ(gen3_hcli_cart_info(&cli, &info), Gen3HcliOk);
    ASSERT_MEM_EQ(info.header, hdr, sizeof(hdr));
    ASSERT_TRUE(info.logo_ok);
    ASSERT_TRUE(info.fixed_ok);
    ASSERT_TRUE(info.complement_ok);
    ASSERT_FALSE(info.other_lib);
    ASSERT_TRUE(info.write_game);
    ASSERT_EQ(info.rom_size, 0x1000000);
    ASSERT_EQ(info.lib, 1);
    ASSERT_EQ(info.lib, GEN3_HLIB_FLASH1M);
    ASSERT_EQ(info.lib_off, 0x3B0000);
    /* A Japanese cart, one logo byte wrong, a second library string */
    default_spec();
    insert("AXVJ", 16 * MIB, "EEPROM_V124");
    fake_helper_set_wire(&fh, wire);
    gba.header[0x20] ^= 0x01u;
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_cart_info(&cli, &info), Gen3HcliOk);
    ASSERT_MEM_EQ(info.header, gba.header, sizeof(hdr));
    ASSERT_FALSE(info.logo_ok);
    ASSERT_TRUE(info.fixed_ok);
    ASSERT_TRUE(info.complement_ok);
    ASSERT_TRUE(info.other_lib);
    ASSERT_FALSE(info.write_game);
    ASSERT_EQ(info.lib, GEN3_HLIB_FLASH1M);
    ASSERT_EQ(info.lib_off, 0x3B0000);
}

TEST(test_hcli_cart_info) {
    cart_info_on(MlinkWireMulti);
}

TEST(test_hcli_probe) {
    setup();
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    Gen3HProbe probe;
    memset(&probe, 0xEE, sizeof(probe));
    ASSERT_EQ(gen3_hcli_save_probe(&cli, &probe), Gen3HcliOk);
    ASSERT_EQ(probe.flash_id, 0x09C2);
    ASSERT_EQ(probe.cls, 0);
    ASSERT_EQ(probe.cls, GEN3_HCHIP_FLASH128);
    ASSERT_EQ(probe.pre0, before[0]);
    ASSERT_EQ(probe.pre1, before[1]);
    ASSERT_EQ(probe.id0, 0xC2);
    ASSERT_EQ(probe.id1, 0x09);
    ASSERT_FALSE(probe.sram);
    /* An SRAM where the flash should be */
    setup();
    fake_gba_save(&gba, FakeSaveSram, 0);
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_save_probe(&cli, &probe), Gen3HcliOk);
    ASSERT_EQ(probe.cls, GEN3_HCHIP_SRAM);
    ASSERT_TRUE(probe.sram);
}

/* test_hcli_read_sector's body on that wire */
static void read_sector_on(MlinkWire wire) {
    ASSERT_TRUE(setup_identified_on(wire));
    memset(buf, 0xEE, sizeof(buf));
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 9, buf), Gen3HcliOk);
    ASSERT_MEM_EQ(buf, sector(9), SECTOR_LEN);
    ASSERT_EQ(buf[SECTOR_LEN], 0xEE);
    for(uint32_t i = 0; i < SECTOR_LEN; i++) sector(20)[i] = (uint8_t)(i * 7u + 1u);
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 20, buf), Gen3HcliOk);
    ASSERT_MEM_EQ(buf, sector(20), SECTOR_LEN);
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 32, buf), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_BAD_ARG);
    ASSERT_EQ(cli.last_fail_off, 32);
    ASSERT_EQ(cli.resends, 0);
}

TEST(test_hcli_read_sector) {
    read_sector_on(MlinkWireMulti);
}

TEST(test_hcli_read_footers) {
    ASSERT_TRUE(setup_identified());
    memset(footers, 0xEE, sizeof(footers));
    ASSERT_EQ(gen3_hcli_read_footers(&cli, footers), Gen3HcliOk);
    /* Entry 3: the save's sector 3, slot A's id 3 */
    ASSERT_EQ(fake_save_phys(&spec, 0, 3), 3);
    ASSERT_EQ(footers[3].id, 3);
    ASSERT_EQ(footers[3].checksum, sum_ref(sector(3), fake_save_size(FakeGameRS, 3)));
    ASSERT_EQ(footers[3].signature, 0x08012025u);
    ASSERT_EQ(footers[3].counter, 4);
    ASSERT_EQ(footers[3].full_checksum, footers[3].checksum);
    for(uint32_t s = 0; s < 32; s++) {
        const uint8_t* b = sector(s);
        ASSERT_EQ(footers[s].id, le16(b + 0xFF4));
        ASSERT_EQ(footers[s].checksum, le16(b + 0xFF6));
        ASSERT_EQ(footers[s].signature, le32(b + 0xFF8));
        ASSERT_EQ(footers[s].counter, le32(b + 0xFFC));
        ASSERT_EQ(footers[s].full_checksum, sum_ref(b, 0xF80));
    }
}

TEST(test_hcli_sector_crc32) {
    ASSERT_TRUE(setup_identified());
    uint32_t crc = 0;
    ASSERT_EQ(gen3_hcli_sector_crc32(&cli, 5, &crc), Gen3HcliOk);
    ASSERT_EQ(crc, gen3_crc32(sector(5), SECTOR_LEN));
    ASSERT_EQ(gen3_hcli_sector_crc32(&cli, 30, &crc), Gen3HcliOk);
    ASSERT_EQ(crc, gen3_crc32(sector(30), SECTOR_LEN));
}

TEST(test_hcli_rom_read) {
    setup();
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    memset(buf, 0xEE, sizeof(buf));
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x08001000u, 2048, buf), Gen3HcliOk);
    for(uint32_t k = 0; k < 2048; k++) ASSERT_EQ(le16(buf + 2 * k), rom_pattern(0x1000u + 2 * k));
    ASSERT_EQ(buf[SECTOR_LEN], 0xEE);
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x08000000u, 96, buf), Gen3HcliOk);
    ASSERT_MEM_EQ(buf, hdr, sizeof(hdr));
    /* One word: exactly 2 bytes land in out */
    memset(buf, 0xEE, sizeof(buf));
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x080000ACu, 1, buf), Gen3HcliOk);
    ASSERT_MEM_EQ(buf, "AX", 2);
    ASSERT_EQ(buf[2], 0xEE);
    /* Outside the ROM window, and a refused one-word read writes nothing either */
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x0A000000u, 1, buf), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_BAD_ARG);
    ASSERT_EQ(buf[0], 'A');
    ASSERT_EQ(buf[2], 0xEE);
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x09FFFFFEu, 2, buf), Gen3HcliStatusError);
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x08000000u, 2049, buf), Gen3HcliStatusError);
}

TEST(test_hcli_rom_crc32) {
    setup();
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    memcpy(rom, hdr, sizeof(hdr));
    for(uint32_t off = sizeof(hdr); off < sizeof(rom); off += 2) {
        rom[off] = (uint8_t)rom_pattern(off);
        rom[off + 1] = (uint8_t)(rom_pattern(off) >> 8);
    }
    uint32_t crc = 0;
    ASSERT_EQ(gen3_hcli_rom_crc32(&cli, 0x08000000u, sizeof(rom), &crc), Gen3HcliOk);
    ASSERT_EQ(crc, gen3_crc32(rom, sizeof(rom)));
    ASSERT_EQ(gen3_hcli_rom_crc32(&cli, 0x08000100u, 0x100, &crc), Gen3HcliOk);
    ASSERT_EQ(crc, gen3_crc32(rom + 0x100, 0x100));
    ASSERT_EQ(gen3_hcli_rom_crc32(&cli, 0x08000000u, 0x40001u, &crc), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_BAD_ARG);
    ASSERT_EQ(gen3_hcli_rom_crc32(&cli, 0x08000000u, 0x40002u, &crc), Gen3HcliStatusError);
}

/* Every write command, end to end: slot B written from slot A, then the game's choice moves. */
TEST(test_hcli_write_commands) {
    ASSERT_TRUE(setup_identified());
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    /* id 0 uploaded, ids 1..13 copied, all with counter 5 and the commit byte left erased */
    make_img(0, 5);
    ASSERT_EQ(gen3_hcli_write_sector(&cli, 14, true, img), Gen3HcliOk);
    ASSERT_TRUE(holds_img(14, 0xFF));
    for(uint8_t k = 1; k < 14; k++) {
        ASSERT_EQ(gen3_hcli_copy_sector(&cli, k, (uint8_t)(14 + k), true, 5), Gen3HcliOk);
        ASSERT_EQ(sector(14 + k)[0xFF8], 0xFF);
    }
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 0);
    for(uint8_t s = 14; s < 28; s++) ASSERT_EQ(gen3_hcli_commit_sector(&cli, s), Gen3HcliOk);
    ASSERT_EQ(counts().commits, 14);
    for(uint32_t k = 0; k < 14; k++) {
        ASSERT_MEM_EQ(sector(14 + k), before + k * SECTOR_LEN, 0xFFC);
        ASSERT_EQ(le32(sector(14 + k) + 0xFFC), 5);
    }
    ASSERT_MEM_EQ(fake_gba_flash(&gba), before, 14 * SECTOR_LEN);
    ASSERT_MEM_EQ(sector(28), before + 28 * SECTOR_LEN, 4 * SECTOR_LEN);
    ASSERT_EQ(hp_save_active_slot(HpSaveGameRS), 1);
    /* The 14th commit completed slot B and ended the arming */
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_NOT_ARMED);
    ASSERT_EQ(cli.last_fail_off, 14);
    /* Now slot A is the one to write: an erase, then DISARM */
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 0, 0, 5), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 3), Gen3HcliOk);
    ASSERT_TRUE(all_ff(sector(3), SECTOR_LEN));
    ASSERT_EQ(gen3_hcli_disarm(&cli), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 4), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_NOT_ARMED);
    ASSERT_EQ(cli.resends, 0);
}

TEST(test_hcli_status_error) {
    ASSERT_TRUE(setup_identified());
    const uint32_t ex = tap.exchanges;
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_NOT_ARMED);
    ASSERT_EQ(cli.last_fail_code, 0);
    ASSERT_EQ(cli.last_fail_off, 14);
    ASSERT_EQ(tap.exchanges, ex + 1);
    ASSERT_EQ(cli.resends, 0);
    /* REFUSED names the check: the game loads slot A, so it can't be the target */
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 0, 0, 4), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_REFUSED);
    ASSERT_EQ(cli.last_fail_code, HP_REFUSE_SLOT);
    /* A write must leave the commit byte for COMMIT_SECTOR */
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    make_img(1, 5);
    ASSERT_EQ(gen3_hcli_write_sector(&cli, 15, false, img), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_BAD_ARG);
    ASSERT_EQ(cli.last_fail_off, 15);
    ASSERT_EQ(gen3_hcli_copy_sector(&cli, 1, 15, false, 5), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_BAD_ARG);
    /* The next call that succeeds clears them */
    ASSERT_EQ(gen3_hcli_disarm(&cli), Gen3HcliOk);
    ASSERT_EQ(cli.last_status, GEN3_HST_OK);
    ASSERT_EQ(cli.last_fail_code, 0);
    ASSERT_EQ(cli.last_fail_off, 0);
    ASSERT_EQ(counts().erases + counts().programs, 0);
    ASSERT_MEM_EQ(fake_gba_flash(&gba), before, FAKE_GBA_FLASH_LEN);
}

/* ---- Resends ---- */

TEST(test_hcli_resend_after_corrupt_reply) {
    ASSERT_TRUE(setup_identified());
    /* The sector holds the builder's data, not zeros, so a reply that drops it can't pass */
    uint32_t nonzero = 0;
    for(uint32_t i = 0; i < 0xF80; i++) nonzero += sector(9)[i] != 0;
    ASSERT_TRUE(nonzero > 0xF80 / 2);
    /* One word of the sector's data comes back wrong: BadCrc, then the same request again */
    hook_arm(GEN3_HCMD_READ_SECTOR, 0, 100);
    memset(buf, 0, sizeof(buf));
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 9, buf), Gen3HcliOk);
    ASSERT_EQ(cli.resends, 1);
    ASSERT_MEM_EQ(buf, sector(9), SECTOR_LEN);
    ASSERT_EQ(hook.calls[HP_CMD_READ_SECTOR], 2);
}

TEST(test_hcli_write_resend_not_reexecuted) {
    ASSERT_TRUE(setup_identified());
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    for(uint32_t i = 0; i < SECTOR_LEN; i++) sector(14)[i] = (uint8_t)i;
    /* The cable out for 40 transfers right after the request's last word: the pump gives up while
     * the helper erases; the resend meets a helper that has done it, and gets its answer */
    hook_arm(GEN3_HCMD_ERASE_SECTOR, 40, 0);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliOk);
    ASSERT_TRUE(cli.resends >= 1);
    ASSERT_EQ(counts().erases, 1);
    ASSERT_EQ(hook.calls[HP_CMD_ERASE_SECTOR], 2);
    ASSERT_TRUE(all_ff(sector(14), SECTOR_LEN));
}

/* No write-class command runs twice, however its response is lost. */
TEST(test_hcli_lost_responses_not_rerun) {
    ASSERT_TRUE(setup_identified());
    /* Each response lost twice after the helper ran the command, each a different way the pump
     * reports it; the client sends the same packet again, and the helper answers from memory.
     * Once ARM_WRITE has run, a data byte of slot A, the slot it keeps, goes bad: run again, it
     * would answer REFUSED; the stored answer is OK. */
    const uint32_t kept = fake_save_phys(&spec, 0, 3) * SECTOR_LEN + 0x10u;
    hook_arm(GEN3_HCMD_ARM_WRITE, 0, 0);
    hook.flip = kept;
    tap_drop(GEN3_HCMD_ARM_WRITE, 2, Gen3HlinkFailNoResponse);
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    ASSERT_EQ(tap.dropped, 2);
    ASSERT_EQ(hook.calls[HP_CMD_ARM_WRITE], 3);
    fake_gba_flash(&gba)[kept] ^= 0x01;
    tap_drop(GEN3_HCMD_ERASE_SECTOR, 2, Gen3HlinkFailBadCrc);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliOk);
    ASSERT_EQ(counts().erases, 1);
    make_img(0, 5);
    tap_drop(GEN3_HCMD_WRITE_SECTOR, 2, Gen3HlinkFailWire);
    ASSERT_EQ(gen3_hcli_write_sector(&cli, 14, true, img), Gen3HcliOk);
    /* Run again, it would erase (its first run programmed the sector) and program again */
    ASSERT_EQ(counts().erases, 1);
    ASSERT_EQ(counts().programs, DEFER_PROGRAMS);
    tap_drop(GEN3_HCMD_COPY_SECTOR, 2, Gen3HlinkFailSeq);
    ASSERT_EQ(gen3_hcli_copy_sector(&cli, 1, 15, true, 5), Gen3HcliOk);
    ASSERT_EQ(counts().erases, 2);
    ASSERT_EQ(counts().programs, 2 * DEFER_PROGRAMS);
    /* Run again, a commit would answer NOT_ERASED */
    tap_drop(GEN3_HCMD_COMMIT_SECTOR, 2, Gen3HlinkFailNoReady);
    ASSERT_EQ(gen3_hcli_commit_sector(&cli, 14), Gen3HcliOk);
    ASSERT_EQ(counts().commits, 1);
    ASSERT_EQ(counts().programs, 2 * DEFER_PROGRAMS + 1u);
    ASSERT_EQ(tap.dropped, 2);
    ASSERT_EQ(cli.resends, 10);
    /* Lost on every attempt: LinkLost, and still run once */
    tap_drop(GEN3_HCMD_ERASE_SECTOR, 4, Gen3HlinkFailNoResponse);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 16), Gen3HcliLinkLost);
    ASSERT_EQ(tap.dropped, 4);
    ASSERT_EQ(counts().erases, 3);
    /* The cable out for 40 transfers right after each request: the pump gives up while the helper
     * works, and the resend gets the stored answer */
    hook_arm(GEN3_HCMD_ERASE_SECTOR, 40, 0);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 17), Gen3HcliOk);
    ASSERT_EQ(counts().erases, 4);
    make_img(3, 5);
    hook_arm(GEN3_HCMD_WRITE_SECTOR, 40, 0);
    ASSERT_EQ(gen3_hcli_write_sector(&cli, 17, true, img), Gen3HcliOk);
    ASSERT_EQ(counts().erases, 4);
    ASSERT_EQ(counts().programs, 3 * DEFER_PROGRAMS + 1u);
    hook_arm(GEN3_HCMD_COPY_SECTOR, 40, 0);
    ASSERT_EQ(gen3_hcli_copy_sector(&cli, 2, 16, true, 5), Gen3HcliOk);
    ASSERT_EQ(counts().erases, 4);
    ASSERT_EQ(counts().programs, 4 * DEFER_PROGRAMS + 1u);
    hook_arm(GEN3_HCMD_COMMIT_SECTOR, 40, 0);
    ASSERT_EQ(gen3_hcli_commit_sector(&cli, 15), Gen3HcliOk);
    ASSERT_EQ(counts().commits, 2);
    ASSERT_EQ(counts().programs, 4 * DEFER_PROGRAMS + 2u);
    /* The hook sees every packet the helper takes: 3 + 2 COMMITs, each run once */
    ASSERT_EQ(hook.calls[HP_CMD_COMMIT_SECTOR], 5);
    /* What the slot holds: each image once, 14 and 15 completed */
    ASSERT_TRUE(holds_img(17, 0xFF));
    make_img(0, 5);
    ASSERT_TRUE(holds_img(14, 0x25));
    make_img(1, 5);
    ASSERT_TRUE(holds_img(15, 0x25));
    make_img(2, 5);
    ASSERT_TRUE(holds_img(16, 0xFF));
    ASSERT_MEM_EQ(fake_gba_flash(&gba), before, 14 * SECTOR_LEN);
}

TEST(test_hcli_link_lost) {
    ASSERT_TRUE(setup_identified());
    const uint32_t ex = tap.exchanges;
    fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), FOREVER);
    ASSERT_EQ(gen3_hcli_read_footers(&cli, footers), Gen3HcliLinkLost);
    ASSERT_EQ(tap.exchanges, ex + 1 + GEN3_HCLI_RESENDS);
    ASSERT_EQ(cli.resends, GEN3_HCLI_RESENDS);
    ASSERT_EQ(GEN3_HCLI_RESENDS, 3);
    ASSERT_EQ(hook.calls[HP_CMD_READ_FOOTERS], 0);
    /* The client never reconnects on its own; once the cable is back, connect does */
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_read_footers(&cli, footers), Gen3HcliOk);
    ASSERT_EQ(footers[0].counter, 4);
}

/* ---- Budgets ---- */

/* Each command's BUSY budget (multiboot-and-save.md §6.5) goes into its request. */
TEST(test_hcli_busy_per_command) {
    ASSERT_EQ(GEN3_HCLI_BUSY_FLASH_US, 12000000u);
    ASSERT_EQ(GEN3_HCLI_BUSY_SCAN_US, 20000000u);
    ASSERT_TRUE(setup_identified());
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_CART_INFO], GEN3_HCLI_BUSY_SCAN_US);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_HELLO], 0);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_SAVE_PROBE], 0);
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 9, buf), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_READ_SECTOR], 0);
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_ARM_WRITE], 0);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_ERASE_SECTOR], GEN3_HCLI_BUSY_FLASH_US);
    make_img(0, 5);
    ASSERT_EQ(gen3_hcli_write_sector(&cli, 14, true, img), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_WRITE_SECTOR], GEN3_HCLI_BUSY_FLASH_US);
    ASSERT_EQ(gen3_hcli_copy_sector(&cli, 1, 15, true, 5), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_COPY_SECTOR], GEN3_HCLI_BUSY_FLASH_US);
    ASSERT_EQ(gen3_hcli_commit_sector(&cli, 14), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_COMMIT_SECTOR], 0);
    ASSERT_EQ(gen3_hcli_disarm(&cli), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_DISARM], 0);
    ASSERT_EQ(gen3_hcli_read_footers(&cli, footers), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_READ_FOOTERS], 0);
    uint32_t crc;
    ASSERT_EQ(gen3_hcli_sector_crc32(&cli, 0, &crc), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_SECTOR_CRC32], 0);
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x08000000u, 4, buf), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_ROM_READ], 0);
    ASSERT_EQ(gen3_hcli_rom_crc32(&cli, 0x08000000u, 0x40000u, &crc), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_ROM_CRC32], 0);
    /* The 16 MiB scan is modelled as 7 s of BUSY: over the 6 s every other command gets */
    const Gen3HlinkReq scan = {
        .code = GEN3_HCMD_CART_INFO,
        .seq = 0x80,
        .resp = cli.resp,
        .resp_max_words = 102,
    };
    uint8_t status;
    uint16_t n;
    ASSERT_EQ(tap.inner.exchange(tap.inner.ctx, &scan, &status, &n), Gen3HlinkFailBusyTimeout);
    /* A 32 MiB cart the helper doesn't write: about 14 s of BUSY, and still an answer */
    default_spec();
    insert("AGBE", 32 * MIB, NULL);
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_cart_info(&cli, &info), Gen3HcliOk);
    ASSERT_EQ(info.rom_size, 32 * MIB);
    ASSERT_FALSE(info.write_game);
    ASSERT_EQ(cli.resends, 0);
}

/* The pump gave up on a READ_SECTOR (the cable out for 40 transfers right after the request), and
 * the helper sends its 2053-word response anyway: about 2.2 s before it hunts again. */
static bool lose_read_response(void) {
    hook_arm(GEN3_HCMD_READ_SECTOR, 40, 0);
    const Gen3HlinkReq req = {
        .code = GEN3_HCMD_READ_SECTOR,
        .seq = 0x55,
        .args = {9},
        .nargs = 1,
        .resp = buf,
        .resp_max_words = 2048,
    };
    uint8_t status;
    uint16_t n;
    return tap.inner.exchange(tap.inner.ctx, &req, &status, &n) == Gen3HlinkFailNoResponse;
}

TEST(test_hcli_connect_drains_tail) {
    ASSERT_TRUE(GEN3_HCLI_SYNC_US >= 3000000u);
    /* A 2 s sync ends before the tail does */
    ASSERT_TRUE(setup_identified());
    ASSERT_TRUE(lose_read_response());
    ASSERT_FALSE(tap.inner.sync(tap.inner.ctx, 4, 2000000u));
    /* connect's waits it out, and the link works again */
    ASSERT_TRUE(setup_identified());
    ASSERT_TRUE(lose_read_response());
    const uint32_t ex = tap.exchanges;
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    ASSERT_EQ(tap.exchanges, ex + 1);
    ASSERT_EQ(cli.resends, 0);
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 9, buf), Gen3HcliOk);
    ASSERT_MEM_EQ(buf, sector(9), SECTOR_LEN);
}

/* A response too long for the request is read out to its end: the next request meets a helper
 * that is hunting again. */
TEST(test_hcli_too_long_then_hello) {
    ASSERT_TRUE(setup_identified());
    memset(buf, 0xEE, sizeof(buf));
    const Gen3HlinkReq req = {
        .code = GEN3_HCMD_READ_SECTOR,
        .seq = 0x60,
        .args = {9},
        .nargs = 1,
        .resp = buf,
        .resp_max_words = 2,
    };
    uint8_t status;
    uint16_t n;
    ASSERT_EQ(tap.inner.exchange(tap.inner.ctx, &req, &status, &n), Gen3HlinkFailTooLong);
    for(uint32_t i = 0; i < 8; i++) ASSERT_EQ(buf[i], 0xEE);
    const Gen3HlinkReq hello = {
        .code = GEN3_HCMD_HELLO,
        .seq = 0x61,
        .resp = buf,
        .resp_max_words = 4,
    };
    ASSERT_EQ(tap.inner.exchange(tap.inner.ctx, &hello, &status, &n), Gen3HlinkFailNone);
    ASSERT_EQ(status, GEN3_HST_OK);
    ASSERT_EQ(n, 4);
    ASSERT_EQ(le16(buf), GEN3_HELPER_PROTO);
}

/* ---- The Normal wire ---- */

/* test_hcli_connect, test_hcli_cart_info and test_hcli_read_sector on the Normal wire, with the
 * same assertions but HELLO's boot mode, 2 there. The helper's time (CART_INFO's 7 s scan, a
 * sector read) comes as not-ready slots, which the pump takes as BUSY; with the cable out every
 * slot is not ready. */
TEST(test_hcli_normal_roundtrip) {
    connect_on(MlinkWireNormal);
    cart_info_on(MlinkWireNormal);
    read_sector_on(MlinkWireNormal);
}

/* A flash command keeps the helper at work for seconds, and on the Normal wire every slot
 * meanwhile is not ready. The request stays alive within its BUSY budget: no NoResponse, no
 * resend. */
TEST(test_hcli_normal_busy_flash) {
    ASSERT_TRUE(setup_identified_on(MlinkWireNormal));
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliOk);
    make_img(0, 5);
    hook_arm(GEN3_HCMD_WRITE_SECTOR, 0, 0);
    hook.busy_us = 3000000u;
    const uint32_t n0 = fake_helper_transfers(&fh);
    ASSERT_EQ(gen3_hcli_write_sector(&cli, 14, true, img), Gen3HcliOk);
    ASSERT_EQ(tap.busy_us[GEN3_HCMD_WRITE_SECTOR], GEN3_HCLI_BUSY_FLASH_US);
    /* The request's 2048 data words, and the 3 s as 175 not-ready slots of 16 ticks */
    ASSERT_TRUE(fake_helper_transfers(&fh) - n0 >= 2048u + 175u);
    ASSERT_EQ(cli.resends, 0);
    ASSERT_EQ(hook.calls[HP_CMD_WRITE_SECTOR], 1);
    ASSERT_TRUE(holds_img(14, 0xFF));
}

/* The cable pulled for good as a READ_SECTOR's response starts: from then every slot is not ready,
 * which the pump takes as BUSY, so the request ends at its BUSY budget (the default 6 s), not in a
 * hang: within 2 s of it in the fake's modelled time. Once the cable is back, connect links
 * again. */
TEST(test_hcli_normal_unplug) {
    ASSERT_TRUE(setup_identified_on(MlinkWireNormal));
    /* The read's time made 10 ms: one not-ready slot after the request's last word, then the
     * response's first word, from which the cable is out */
    hook_arm(GEN3_HCMD_READ_SECTOR, FOREVER, 0);
    hook.unplug_after = 1;
    hook.busy_us = 10000u;
    const uint64_t t0 = fh.now_us;
    const Gen3HcliResult r = gen3_hcli_read_sector(&cli, 9, buf);
    ASSERT_TRUE(r == Gen3HcliLinkLost || r == Gen3HcliBusyTimeout);
    ASSERT_TRUE(fh.now_us - t0 <= GEN3_HLINK_BUSY_US + 2000000u);
    ASSERT_EQ(hook.calls[HP_CMD_READ_SECTOR], 1);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 9, buf), Gen3HcliOk);
    ASSERT_MEM_EQ(buf, sector(9), SECTOR_LEN);
}

/* The cable pulled for good in the middle of a WRITE_SECTOR's request, at about its 100th word:
 * the pump fails the packet as Wire after GEN3_HLINK_NOTREADY_RUN not-ready words instead of
 * sending its other 1955 words into a cable that isn't there, and the resend waits for READY within
 * the write's BUSY budget. So the call ends BusyTimeout or LinkLost within 2 s of that budget in
 * the fake's modelled time, and the helper never ran the write. */
TEST(test_hcli_normal_unplug_mid_request) {
    ASSERT_TRUE(setup_identified_on(MlinkWireNormal));
    ASSERT_EQ(gen3_hcli_arm_write(&cli, MX, 1, 0, 4), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliOk);
    make_img(0, 5);
    /* The exchange's first frame is 9 idle words, the first answered READY; the request's words
     * follow, word k at transfer n0 + 9 + k */
    const uint32_t n0 = fake_helper_transfers(&fh);
    fake_helper_unplug_at(&fh, n0 + 9u + 99u, FOREVER);
    const uint64_t t0 = fh.now_us;
    const Gen3HcliResult r = gen3_hcli_write_sector(&cli, 14, true, img);
    ASSERT_TRUE(r == Gen3HcliLinkLost || r == Gen3HcliBusyTimeout);
    ASSERT_TRUE(fh.now_us - t0 <= GEN3_HCLI_BUSY_FLASH_US + 2000000u);
    ASSERT_EQ(hook.calls[HP_CMD_WRITE_SECTOR], 0);
    ASSERT_TRUE(all_ff(sector(14), SECTOR_LEN));
}

/* The cable pulled for good at the 5th word of a READ_SECTOR's response: the pump fails it as Wire
 * after GEN3_HLINK_NOTREADY_RUN not-ready words instead of reading its other 2050 words, and the
 * resend waits for READY within the read's BUSY budget (the default 6 s). So the call ends
 * BusyTimeout or LinkLost within 2 s of that budget in the fake's modelled time. */
TEST(test_hcli_normal_unplug_mid_response) {
    ASSERT_TRUE(setup_identified_on(MlinkWireNormal));
    /* As in test_hcli_normal_resend_after_stall, but the cable stays out */
    hook_arm(GEN3_HCMD_READ_SECTOR, FOREVER, 0);
    hook.unplug_after = 1 + 4;
    hook.busy_us = 10000u;
    const uint64_t t0 = fh.now_us;
    const Gen3HcliResult r = gen3_hcli_read_sector(&cli, 9, buf);
    ASSERT_TRUE(r == Gen3HcliLinkLost || r == Gen3HcliBusyTimeout);
    ASSERT_TRUE(fh.now_us - t0 <= GEN3_HLINK_BUSY_US + 2000000u);
    ASSERT_EQ(hook.calls[HP_CMD_READ_SECTOR], 1);
}

/* A word never delivered inside a response (the cable out for one slot at its 5th word): the
 * helper's words from there on come one slot late, the pump fails the packet as Wire, and the
 * resend reads the sector right. */
TEST(test_hcli_normal_resend_after_stall) {
    ASSERT_TRUE(setup_identified_on(MlinkWireNormal));
    /* The read's time made 10 ms: one not-ready slot after the request's last word, then the
     * response from its SOF; its 5th word, the second payload word, isn't delivered */
    hook_arm(GEN3_HCMD_READ_SECTOR, 1, 0);
    hook.unplug_after = 1 + 4;
    hook.busy_us = 10000u;
    memset(buf, 0, sizeof(buf));
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 9, buf), Gen3HcliOk);
    ASSERT_TRUE(cli.resends >= 1);
    ASSERT_MEM_EQ(buf, sector(9), SECTOR_LEN);
    ASSERT_EQ(hook.calls[HP_CMD_READ_SECTOR], 1 + cli.resends);
}

/* ---- The client's rules, on scripted exchanges ---- */

TEST(test_hcli_resend_rules) {
    /* Each link failure: the same request again, the same seq included */
    static const Gen3HlinkFail resend[] = {
        Gen3HlinkFailNoReady,
        Gen3HlinkFailNoResponse,
        Gen3HlinkFailBadCrc,
        Gen3HlinkFailWire,
        Gen3HlinkFailSeq,
    };
    for(uint32_t i = 0; i < sizeof(resend) / sizeof(resend[0]); i++) {
        script_begin();
        script_add(resend[i], 0, 0);
        script_add(Gen3HlinkFailNone, GEN3_HST_OK, 0);
        ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliOk);
        ASSERT_EQ(script.exchanges, 2);
        ASSERT_EQ(cli.resends, 1);
        ASSERT_EQ(script.req[0].seq, 1);
        ASSERT_TRUE(same_req(&script.req[0], &script.req[1]));
    }
    /* Three in a row, then the answer */
    script_begin();
    script_add(Gen3HlinkFailNoReady, 0, 0);
    script_add(Gen3HlinkFailBadCrc, 0, 0);
    script_add(Gen3HlinkFailSeq, 0, 0);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 0);
    ASSERT_EQ(gen3_hcli_disarm(&cli), Gen3HcliOk);
    ASSERT_EQ(script.exchanges, 4);
    ASSERT_EQ(cli.resends, 3);
    /* Four: LinkLost; the next call takes the next seq */
    script_begin();
    for(uint32_t i = 0; i < 4; i++) script_add(Gen3HlinkFailWire, 0, 0);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 0);
    ASSERT_EQ(gen3_hcli_disarm(&cli), Gen3HcliLinkLost);
    ASSERT_EQ(script.exchanges, 4);
    ASSERT_EQ(cli.resends, 3);
    ASSERT_EQ(gen3_hcli_disarm(&cli), Gen3HcliOk);
    ASSERT_EQ(script.req[4].seq, 2);
    /* BusyTimeout at once; TooLong is a BadResponse at once */
    script_begin();
    script_add(Gen3HlinkFailBusyTimeout, 0, 0);
    script_add(Gen3HlinkFailTooLong, 0, 0);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliBusyTimeout);
    ASSERT_EQ(script.exchanges, 1);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliBadResponse);
    ASSERT_EQ(script.exchanges, 2);
    ASSERT_EQ(cli.resends, 0);
    ASSERT_EQ(script.req[1].seq, 2);
    /* seq wraps: the 256th call's is 0 */
    script_begin();
    for(uint32_t i = 0; i < 256; i++) {
        script.steps = 0;
        script.exchanges = 0;
        script_add(Gen3HlinkFailNone, GEN3_HST_OK, 0);
        ASSERT_EQ(gen3_hcli_disarm(&cli), Gen3HcliOk);
        ASSERT_EQ(script.req[0].seq, (uint8_t)(i + 1));
    }
    ASSERT_EQ(script.req[0].seq, 0);
    /* A failed sync: LinkLost, no HELLO */
    script_begin();
    script.sync_ok = false;
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliLinkLost);
    ASSERT_EQ(script.exchanges, 0);
    ASSERT_EQ(script.sync_need, 4);
    ASSERT_EQ(script.sync_us, GEN3_HCLI_SYNC_US);
}

TEST(test_hcli_response_checks) {
    /* A status response: its fail code and offset */
    script_begin();
    script.payload[0] = 0xA012;
    script.payload[1] = 0x0FF8;
    script_add(Gen3HlinkFailNone, GEN3_HST_ERASE_FAIL, 2);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_ERASE_FAIL);
    ASSERT_EQ(cli.last_fail_code, 0xA012);
    ASSERT_EQ(cli.last_fail_off, 0x0FF8);
    /* ... and without them */
    script_add(Gen3HlinkFailNone, GEN3_HST_REFUSED, 0);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliBadResponse);
    ASSERT_EQ(cli.last_status, GEN3_HST_REFUSED);
    /* A link failure clears what the last response said */
    script_add(Gen3HlinkFailBusyTimeout, 0, 0);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliBusyTimeout);
    ASSERT_EQ(cli.last_status, GEN3_HST_OK);
    ASSERT_EQ(cli.last_fail_code, 0);
    ASSERT_EQ(cli.last_fail_off, 0);
    /* An OK response of the wrong length */
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 2);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, 14), Gen3HcliBadResponse);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 2047);
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 1, buf), Gen3HcliBadResponse);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 1);
    ASSERT_EQ(gen3_hcli_sector_crc32(&cli, 1, &(uint32_t){0}), Gen3HcliBadResponse);
    ASSERT_EQ(cli.resends, 0);
    /* Another protocol: BadResponse, with what it said */
    script_begin();
    script.payload[0] = 2;
    script.payload[1] = 7;
    script.payload[2] = 1000;
    script.payload[3] = 0x0203;
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 4);
    Gen3HHello hello;
    ASSERT_EQ(gen3_hcli_connect(&cli, &hello), Gen3HcliBadResponse);
    ASSERT_EQ(hello.proto, 2);
    ASSERT_EQ(hello.build, 7);
    ASSERT_EQ(hello.max_payload, 1000);
    ASSERT_EQ(hello.boot_mode, 3);
    ASSERT_EQ(hello.slave_id, 2);
    script.payload[0] = GEN3_HELPER_PROTO;
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 3);
    ASSERT_EQ(gen3_hcli_connect(&cli, &hello), Gen3HcliBadResponse);
    /* Values outside what the protocol defines */
    script_begin();
    memset(script.payload, 0, sizeof(script.payload));
    script.payload[99] = GEN3_HLIB_EEPROM + 1u;
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 102);
    ASSERT_EQ(gen3_hcli_cart_info(&cli, &info), Gen3HcliBadResponse);
    script.payload[99] = GEN3_HLIB_EEPROM;
    script.payload[97] = 0x0000;
    script.payload[98] = 0x0100;
    script.payload[100] = 0x1234;
    script.payload[101] = 0x0056;
    /* Each flag on its own */
    for(uint32_t bit = 0; bit < 5; bit++) {
        script.payload[96] = (uint16_t)(1u << bit);
        script_add(Gen3HlinkFailNone, GEN3_HST_OK, 102);
        ASSERT_EQ(gen3_hcli_cart_info(&cli, &info), Gen3HcliOk);
        script.steps = 0;
        script.exchanges = 0;
        const bool got[5] = {
            info.logo_ok, info.fixed_ok, info.complement_ok, info.other_lib, info.write_game};
        const uint16_t bits[5] = {
            GEN3_HCART_LOGO_OK,
            GEN3_HCART_FIXED_OK,
            GEN3_HCART_COMPLEMENT_OK,
            GEN3_HCART_OTHER_LIB,
            GEN3_HCART_WRITE_GAME};
        for(uint32_t k = 0; k < 5; k++) ASSERT_EQ(got[k], (bits[k] & (1u << bit)) != 0);
    }
    ASSERT_EQ(info.lib, GEN3_HLIB_EEPROM);
    ASSERT_EQ(info.rom_size, 0x01000000);
    ASSERT_EQ(info.lib_off, 0x00561234);
    Gen3HProbe probe;
    memset(script.payload, 0, sizeof(script.payload));
    script.payload[1] = GEN3_HCHIP_NONE + 1u;
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 5);
    ASSERT_EQ(gen3_hcli_save_probe(&cli, &probe), Gen3HcliBadResponse);
    script.payload[1] = GEN3_HCHIP_SRAM;
    script.payload[4] = 2;
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 5);
    ASSERT_EQ(gen3_hcli_save_probe(&cli, &probe), Gen3HcliBadResponse);
    script.payload[0] = 0x1362;
    script.payload[2] = 0x2211;
    script.payload[3] = 0x1362;
    script.payload[4] = 1;
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 5);
    ASSERT_EQ(gen3_hcli_save_probe(&cli, &probe), Gen3HcliOk);
    ASSERT_EQ(probe.flash_id, 0x1362);
    ASSERT_EQ(probe.cls, GEN3_HCHIP_SRAM);
    ASSERT_EQ(probe.pre0, 0x11);
    ASSERT_EQ(probe.pre1, 0x22);
    ASSERT_EQ(probe.id0, 0x62);
    ASSERT_EQ(probe.id1, 0x13);
    ASSERT_TRUE(probe.sram);
}

/* Every command's request: its code, arguments, data, room for the response (at least the 2 words
 * of a status response) and BUSY budget; each call the next seq. */
TEST(test_hcli_requests) {
    script_begin();
    for(uint32_t i = 0; i < SCRIPT_MAX; i++) script_add(Gen3HlinkFailNone, GEN3_HST_OK, 0);
    uint32_t crc;
    gen3_hcli_arm_write(&cli, 0x1362, 1, 1, 0x00050004u);
    gen3_hcli_disarm(&cli);
    gen3_hcli_sector_crc32(&cli, 9, &crc);
    gen3_hcli_erase_sector(&cli, 15);
    gen3_hcli_write_sector(&cli, 16, true, img);
    gen3_hcli_copy_sector(&cli, 3, 17, true, 0x00010002u);
    gen3_hcli_commit_sector(&cli, 18);
    gen3_hcli_rom_crc32(&cli, 0x08000000u, 0x40000u, &crc);
    const uint16_t arm[] = {GEN3_HARM_MAGIC, 0x1362, 1, 1, 0x0004, 0x0005};
    ASSERT_TRUE(args_are(&script.req[0], GEN3_HCMD_ARM_WRITE, arm, 6));
    ASSERT_TRUE(args_are(&script.req[1], GEN3_HCMD_DISARM, NULL, 0));
    const uint16_t s9 = 9;
    ASSERT_TRUE(args_are(&script.req[2], GEN3_HCMD_SECTOR_CRC32, &s9, 1));
    const uint16_t s15 = 15;
    ASSERT_TRUE(args_are(&script.req[3], GEN3_HCMD_ERASE_SECTOR, &s15, 1));
    const uint16_t write[] = {16, GEN3_HFLAG_DEFER_COMMIT};
    ASSERT_TRUE(args_are(&script.req[4], GEN3_HCMD_WRITE_SECTOR, write, 2));
    ASSERT_TRUE(script.req[4].data == img);
    ASSERT_EQ(script.req[4].data_words, 2048);
    const uint16_t copy[] = {3, 17, GEN3_HFLAG_DEFER_COMMIT, 0x0002, 0x0001};
    ASSERT_TRUE(args_are(&script.req[5], GEN3_HCMD_COPY_SECTOR, copy, 5));
    const uint16_t s18 = 18;
    ASSERT_TRUE(args_are(&script.req[6], GEN3_HCMD_COMMIT_SECTOR, &s18, 1));
    const uint16_t rcrc[] = {0x0000, 0x0800, 0x0000, 0x0004};
    ASSERT_TRUE(args_are(&script.req[7], GEN3_HCMD_ROM_CRC32, rcrc, 4));
    for(uint32_t i = 0; i < SCRIPT_MAX; i++) {
        ASSERT_EQ(script.req[i].seq, i + 1);
        ASSERT_EQ(script.req[i].resp_max_words, 2);
        ASSERT_TRUE(script.req[i].resp == cli.resp);
        if(i != 4) ASSERT_EQ(script.req[i].data_words, 0);
    }
    ASSERT_EQ(script.req[3].busy_us, GEN3_HCLI_BUSY_FLASH_US);
    ASSERT_EQ(script.req[4].busy_us, GEN3_HCLI_BUSY_FLASH_US);
    ASSERT_EQ(script.req[5].busy_us, GEN3_HCLI_BUSY_FLASH_US);
    ASSERT_EQ(script.req[6].busy_us, 0);
    /* The flags word follows defer_commit */
    script_begin();
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 0);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 0);
    gen3_hcli_write_sector(&cli, 16, false, img);
    gen3_hcli_copy_sector(&cli, 3, 17, false, 0);
    ASSERT_EQ(script.req[0].args[1], 0);
    ASSERT_EQ(script.req[1].args[2], 0);
    /* The responses with a payload */
    script_begin();
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 4);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 102);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 5);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 2048);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 256);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 300);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 1);
    script_add(Gen3HlinkFailNone, GEN3_HST_OK, 0);
    ASSERT_EQ(gen3_hcli_connect(&cli, NULL), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_cart_info(&cli, &info), Gen3HcliOk);
    Gen3HProbe probe;
    ASSERT_EQ(gen3_hcli_save_probe(&cli, &probe), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_read_sector(&cli, 7, buf), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_read_footers(&cli, footers), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x08123456u, 300, buf), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x08000002u, 1, buf), Gen3HcliOk);
    ASSERT_EQ(gen3_hcli_rom_read(&cli, 0x08000002u, 0, buf), Gen3HcliOk);
    ASSERT_TRUE(args_are(&script.req[0], GEN3_HCMD_HELLO, NULL, 0));
    ASSERT_EQ(script.req[0].resp_max_words, 4);
    ASSERT_TRUE(args_are(&script.req[1], GEN3_HCMD_CART_INFO, NULL, 0));
    ASSERT_EQ(script.req[1].resp_max_words, 102);
    ASSERT_EQ(script.req[1].busy_us, GEN3_HCLI_BUSY_SCAN_US);
    ASSERT_TRUE(args_are(&script.req[2], GEN3_HCMD_SAVE_PROBE, NULL, 0));
    ASSERT_EQ(script.req[2].resp_max_words, 5);
    const uint16_t s7 = 7;
    ASSERT_TRUE(args_are(&script.req[3], GEN3_HCMD_READ_SECTOR, &s7, 1));
    ASSERT_TRUE(script.req[3].resp == buf);
    ASSERT_EQ(script.req[3].resp_max_words, 2048);
    ASSERT_TRUE(args_are(&script.req[4], GEN3_HCMD_READ_FOOTERS, NULL, 0));
    ASSERT_TRUE(script.req[4].resp == cli.resp);
    ASSERT_EQ(script.req[4].resp_max_words, 256);
    const uint16_t rr[] = {0x3456, 0x0812, 300};
    ASSERT_TRUE(args_are(&script.req[5], GEN3_HCMD_ROM_READ, rr, 3));
    ASSERT_TRUE(script.req[5].resp == buf);
    ASSERT_EQ(script.req[5].resp_max_words, 300);
    /* Fewer than 2 words: into the client's own buffer, which has room for a status response */
    ASSERT_TRUE(script.req[6].resp == cli.resp);
    ASSERT_EQ(script.req[6].resp_max_words, 2);
    ASSERT_TRUE(script.req[7].resp == cli.resp);
    ASSERT_EQ(script.req[7].resp_max_words, 2);
    for(uint32_t i = 0; i < SCRIPT_MAX; i++) {
        ASSERT_EQ(script.req[i].seq, i + 1);
        if(i != 1) ASSERT_EQ(script.req[i].busy_us, 0);
    }
    ASSERT_EQ(script.syncs, 1);
}

int main(void) {
    RUN(test_hcli_codes_match_helper);
    RUN(test_fake_helper_words);
    RUN(test_fake_helper_busy_by_time);
    RUN(test_fake_helper_rx_timeout);
    RUN(test_fake_helper_unplug_corrupt);
    RUN(test_fake_helper_power_cut_restart);
    RUN(test_fake_helper_normal_busy_is_notready);
    RUN(test_hcli_connect);
    RUN(test_hcli_cart_info);
    RUN(test_hcli_probe);
    RUN(test_hcli_read_sector);
    RUN(test_hcli_read_footers);
    RUN(test_hcli_sector_crc32);
    RUN(test_hcli_rom_read);
    RUN(test_hcli_rom_crc32);
    RUN(test_hcli_write_commands);
    RUN(test_hcli_status_error);
    RUN(test_hcli_resend_after_corrupt_reply);
    RUN(test_hcli_write_resend_not_reexecuted);
    RUN(test_hcli_lost_responses_not_rerun);
    RUN(test_hcli_link_lost);
    RUN(test_hcli_busy_per_command);
    RUN(test_hcli_connect_drains_tail);
    RUN(test_hcli_too_long_then_hello);
    RUN(test_hcli_normal_roundtrip);
    RUN(test_hcli_normal_busy_flash);
    RUN(test_hcli_normal_unplug);
    RUN(test_hcli_normal_unplug_mid_request);
    RUN(test_hcli_normal_unplug_mid_response);
    RUN(test_hcli_normal_resend_after_stall);
    RUN(test_hcli_resend_rules);
    RUN(test_hcli_response_checks);
    RUN(test_hcli_requests);
    TEST_EXIT();
}
