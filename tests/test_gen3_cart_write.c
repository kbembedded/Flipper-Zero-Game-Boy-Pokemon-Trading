#include "test.h"
#include "bulk_file.h"
#include "fake_gba.h"
#include "fake_helper.h"
#include "fake_save.h"
#include "fake_store.h"
#include "gen3_golden.h"
#include "helper_cmd.h"
#include <hp_save.h>
#include <stdbool.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_cart_screen.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_save.h>

/* The safe write, the backup after it, restore and reconnect, through the client and the real
 * packet pump against the helper's real core on the fake GBA (fake_helper), with a RAM store. The
 * default cart is test_gen3_cart.c's: English Ruby (AXVE version 0), 1 MiB of ROM with
 * FLASH1M_V103, an MX chip; its save in slot A with counter 4 and id 0 at its first sector, slot B
 * absent; trainer BRENDAN 12345/54321; a party of 3; box 0 named BOX 1; the golden Treecko at box 1
 * slot 19. The store holds 64 MiB.
 *
 * After every helper command that may have changed the flash, a watch reads the fake flash as the
 * game would (pokeruby: src/save.c: GetSaveValidStatus, by gen3_save_analyze with the game's own
 * section sizes): it must load either the save as it was or the whole new one, and nothing else.
 * At every point the sweeps cut, a second oracle, the helper's hp_save_active_slot, written apart
 * from gen3_save, must name the same slot. */

#define MIB 0x100000u
#define SECTOR_LEN 4096u
#define DATA_LEN 0xF80u
#define WORK_LEN (14u * SECTOR_LEN)
#define SAVE_LEN 131072u
#define MX 0x09C2u
#define FOREVER 10000000u /* an unplug longer than any test */
#define LIB_OFF 0xB0000u /* FLASH1M_V103 in the ROM */
#define LIB_STRING "FLASH1M_V103"
#define STAMP "20261025-143005"
#define STAMP2 "20261025-150000"
#define STAMP3 "20261025-160000"
#define STAMP4 "20261025-170000"
#define BACKUP_NAME "backups/AXVE_12345_" STAMP ".sav"
#define WRITE_NAME "backups/AXVE_12345_" STAMP2 ".sav"
#define OTHER_NAME "backups/other.sav"
#define NONE 0xFFu
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
static uint8_t before[FAKE_SAVE_LEN];
static uint8_t hdr[0xC0];
static uint8_t party[3][100];
static Gen3Editor ed;
static volatile bool cancel;

/* ---- Observers ---- */

/* The progress the jobs report, and what a test does at a step */
typedef struct {
    uint32_t calls[Gen3CartStepRomVerify + 1];
    uint32_t last_done[Gen3CartStepRomVerify + 1];
    uint32_t last_total[Gen3CartStepRomVerify + 1];
    Gen3CartStep unplug_step; /* None: never */
    uint32_t unplug_at; /* once done reaches this: the cable out from unplug_after transfers on */
    uint32_t unplug_after;
    uint32_t unplug_len;
    Gen3CartStep corrupt_step; /* None: never */
    uint32_t corrupt_at; /* once done reaches this, reads of corrupt_off of corrupt_name differ */
    const char* corrupt_name;
    uint32_t corrupt_off;
} Prog;

static Prog prog;

static void prog_cb(void* ctx, Gen3CartStep step, uint32_t done, uint32_t total) {
    Prog* p = ctx;
    p->calls[step]++;
    p->last_done[step] = done;
    p->last_total[step] = total;
    if(p->unplug_step != Gen3CartStepNone && step == p->unplug_step && done >= p->unplug_at) {
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + p->unplug_after, p->unplug_len);
        p->unplug_step = Gen3CartStepNone;
    }
    if(p->corrupt_step != Gen3CartStepNone && step == p->corrupt_step && done >= p->corrupt_at) {
        fake_store_corrupt_read(&store, p->corrupt_name, p->corrupt_off);
        p->corrupt_step = Gen3CartStepNone;
    }
}

/* The watch: the game's view of the flash after each command */
typedef enum {
    ViewOld, /* the save as it was: old_slot with old_counter and want_old's data; or, with
              * old_none, no save the game loads */
    ViewNew, /* new_slot with new_counter and want_new's data */
    ViewOther,
} View;

typedef struct {
    bool on;
    Gen3LinkGame game;
    bool old_none;
    uint8_t old_slot, new_slot;
    uint32_t old_counter, new_counter;
    uint32_t last_ops; /* erases + byte programs at the last view: only they change the flash */
    uint32_t views, old_seen, new_seen, violations;
} Watch;

static Watch watch;
static uint8_t want_old[14][DATA_LEN];
static uint8_t want_new[14][DATA_LEN];
static Gen3SaveSectorInfo view_info[28];

static uint8_t* flash(void) {
    return fake_gba_flash(&gba);
}

#define VIEW_ALL 0xFFu

/* The game's view; the sectors read again are all of them, or only `only` when no other changed
 * since the last view */
static View game_view_of(uint32_t only) {
    for(uint32_t s = 0; s < 28; s++) {
        if(only != VIEW_ALL && s != only) continue;
        gen3_save_sector_info(watch.game, flash() + s * SECTOR_LEN, &view_info[s]);
    }
    Gen3SaveSlots v;
    gen3_save_analyze(view_info, &v);
    if(v.result != Gen3SaveOk && v.result != Gen3SaveError) {
        return watch.old_none ? ViewOld : ViewOther;
    }
    bool is_old = !watch.old_none && v.active == watch.old_slot &&
                  v.save_counter == watch.old_counter;
    bool is_new = v.active == watch.new_slot && v.save_counter == watch.new_counter;
    for(uint32_t k = 0; k < 14 && (is_old || is_new); k++) {
        if(v.phys[k] == NONE) return ViewOther;
        const uint8_t* d = flash() + v.phys[k] * SECTOR_LEN;
        is_old = is_old && memcmp(d, want_old[k], DATA_LEN) == 0;
        is_new = is_new && memcmp(d, want_new[k], DATA_LEN) == 0;
    }
    return is_old ? ViewOld : is_new ? ViewNew : ViewOther;
}

static View game_view(void) {
    return game_view_of(VIEW_ALL);
}

static View watch_count(View v) {
    watch.views++;
    if(v == ViewOld) {
        watch.old_seen++;
    } else if(v == ViewNew) {
        watch.new_seen++;
    } else {
        watch.violations++;
    }
    return v;
}

/* A view counted by the watch, every sector read again */
static View watch_view(void) {
    return watch_count(game_view());
}

/* The second oracle: the slot the game loads by gen3_save_analyze, and by the helper's
 * hp_save_active_slot, which reads the chip through the helper's own flash code with the game's
 * section sizes. The helper reads the chip as the GBA would after a power cycle, so a power cut's
 * freeze is lifted for it, in the chip itself, then the chip as it was is put back: nothing of
 * those reads stays. true when both name the same slot, or none. */
static FakeGba oracle_gba;

static bool oracles_agree(void) {
    for(uint32_t s = 0; s < 28; s++) {
        gen3_save_sector_info(watch.game, flash() + s * SECTOR_LEN, &view_info[s]);
    }
    Gen3SaveSlots v;
    gen3_save_analyze(view_info, &v);
    const int ours = v.result == Gen3SaveOk || v.result == Gen3SaveError ? v.active : -1;
    oracle_gba = gba;
    fake_gba_power_cycle(&gba);
    const int helper =
        hp_save_active_slot(watch.game == Gen3LinkGameFRLG ? HpSaveGameFRLG : HpSaveGameRS);
    gba = oracle_gba;
    if(ours != helper) printf("  oracles differ: gen3_save %d, helper %d\n", ours, helper);
    return ours == helper;
}

static uint32_t flash_ops(void) {
    FakeGbaCounts n;
    fake_gba_counts(&gba, &n);
    return n.erases + n.programs;
}

/* After every helper command: a view, unless no erase or program ran since the last one. Only the
 * command's own sector can have changed (a power cut stops in that one too), so the views after
 * the first read only it again. */
static void watch_check(uint8_t code, uint16_t arg0) {
    if(!watch.on) return;
    const uint32_t ops = flash_ops();
    if(watch.views && ops == watch.last_ops) return;
    watch.last_ops = ops;
    uint32_t only = VIEW_ALL;
    if(watch.views) {
        switch(code) {
        case GEN3_HCMD_ERASE_SECTOR:
        case GEN3_HCMD_WRITE_SECTOR:
        case GEN3_HCMD_COMMIT_SECTOR:
            only = arg0;
            break;
        case GEN3_HCMD_COPY_SECTOR:
            only = fh.hp.req[1];
            break;
        default:
            break;
        }
    }
    watch_count(game_view_of(only));
}

/* The reference run's points: transfers and flash ops from the write's start, after each ERASE,
 * the last WRITE or COPY, and each COMMIT */
#define POINTS 40u
typedef struct {
    bool on;
    uint32_t start_xfer, start_ops;
    uint32_t n;
    uint32_t xfer[POINTS];
    uint32_t ops[POINTS];
    bool last_is_program;
} Ref;

static Ref ref;

static void ref_point(bool program) {
    if(!ref.on) return;
    if(!(program && ref.last_is_program)) ref.n++;
    if(ref.n > POINTS) return;
    ref.xfer[ref.n - 1] = fake_helper_transfers(&fh) - ref.start_xfer;
    ref.ops[ref.n - 1] = flash_ops() - ref.start_ops;
    ref.last_is_program = program;
}

/* The fake helper's command hook: counts each code and the sectors SECTOR_CRC32 read; when the
 * unplug_nth run of unplug_code is done, the cable out at once for unplug_len transfers; the
 * reference points; the watch */
typedef struct {
    uint32_t calls[256];
    uint32_t crc_sectors; /* bit s: SECTOR_CRC32 of sector s ran */
    uint32_t copy_dst; /* bit s: COPY_SECTOR to sector s ran */
    uint8_t unplug_code;
    uint32_t unplug_nth;
    uint32_t unplug_len;
    uint8_t act_code; /* when the act_nth run of act_code is done, act() */
    uint32_t act_nth;
    void (*act)(void);
    /* When the first ERASE_SECTOR ran: the Erase reports so far, and the last one's done */
    uint32_t erase_reports;
    uint32_t erase_first_done;
} Hook;

static Hook hook;

static void hook_cb(void* ctx, uint8_t code, uint16_t arg0) {
    Hook* k = ctx;
    k->calls[code]++;
    if(code == GEN3_HCMD_ERASE_SECTOR && k->calls[code] == 1) {
        k->erase_reports = prog.calls[Gen3CartStepErase];
        k->erase_first_done = prog.last_done[Gen3CartStepErase];
    }
    if(code == GEN3_HCMD_SECTOR_CRC32 && arg0 < 32) k->crc_sectors |= 1u << arg0;
    if(code == GEN3_HCMD_COPY_SECTOR && fh.hp.req[1] < 32) k->copy_dst |= 1u << fh.hp.req[1];
    if(code == GEN3_HCMD_ERASE_SECTOR || code == GEN3_HCMD_COMMIT_SECTOR) ref_point(false);
    if(code == GEN3_HCMD_WRITE_SECTOR || code == GEN3_HCMD_COPY_SECTOR) ref_point(true);
    if(k->unplug_code && code == k->unplug_code && k->calls[code] == k->unplug_nth) {
        k->unplug_code = 0;
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh), k->unplug_len);
    }
    if(k->act_code && code == k->act_code && k->calls[code] == k->act_nth) {
        k->act_code = 0;
        k->act();
    }
    watch_check(code, arg0);
}

/* A transport around the fake helper's that counts exchanges, and can answer the fake_nth fake_code
 * itself, without the helper, with fake_status and fake_n words (fail code fake_fail first); or let
 * the helper run the over_nth over_code, then answer it with over_status and over_n words (fail
 * code over_fail first), as a response that went wrong in the helper or on the way would */
typedef struct {
    Gen3HTransport inner;
    uint32_t exchanges;
    uint8_t fake_code;
    uint32_t fake_nth;
    uint32_t fake_seen;
    uint8_t fake_status;
    uint16_t fake_n;
    uint16_t fake_fail;
    uint8_t over_code;
    uint32_t over_nth;
    uint32_t over_seen;
    uint8_t over_status;
    uint16_t over_n;
    uint16_t over_fail;
} Tap;

static Tap tap;

static void tap_answer(const Gen3HlinkReq* req, uint16_t fail, uint16_t n) {
    memset(req->resp, 0, 2u * n);
    if(n >= 1) {
        req->resp[0] = (uint8_t)fail;
        req->resp[1] = (uint8_t)(fail >> 8);
    }
}

static Gen3HlinkFail
    tap_exchange(void* ctx, const Gen3HlinkReq* req, uint8_t* status, uint16_t* resp_words) {
    Tap* t = ctx;
    t->exchanges++;
    if(t->fake_code && req->code == t->fake_code && ++t->fake_seen == t->fake_nth) {
        t->fake_code = 0;
        tap_answer(req, t->fake_fail, t->fake_n);
        *status = t->fake_status;
        *resp_words = t->fake_n;
        return Gen3HlinkFailNone;
    }
    const Gen3HlinkFail f = t->inner.exchange(t->inner.ctx, req, status, resp_words);
    if(f == Gen3HlinkFailNone && t->over_code && req->code == t->over_code &&
       ++t->over_seen == t->over_nth) {
        t->over_code = 0;
        tap_answer(req, t->over_fail, t->over_n);
        *status = t->over_status;
        *resp_words = t->over_n;
    }
    return f;
}

static bool tap_sync(void* ctx, uint8_t need, uint32_t timeout_us) {
    Tap* t = ctx;
    return t->inner.sync(t->inner.ctx, need, timeout_us);
}

/* ---- Fixtures ---- */

static uint8_t phys(uint8_t slot, uint8_t id) {
    return (uint8_t)fake_save_phys(&spec, slot, id);
}

/* The default save in img, with spec's game; the party edits go through the editor */
static bool save_for(FakeGame game) {
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
    return save_for(FakeGameRS);
}

/* The default save, and in slot B an older one: counter 3, id 0 at its sixth sector */
static bool save_with_older_b(void) {
    if(!default_save()) return false;
    spec.slot_present[1] = true;
    spec.counter[1] = 3;
    spec.first[1] = 5;
    fake_save_build(img2, &spec);
    memcpy(img + 14 * SECTOR_LEN, img2 + 14 * SECTOR_LEN, 14 * SECTOR_LEN);
    return true;
}

/* Inserts a cart with img on its save chip; a fresh helper, transport, client, store and cart. */
static void insert_cart(const char* title, const char* code, uint8_t version, FakeSaveKind kind) {
    fake_gba_init(&gba);
    helper_cmd_header(hdr, gba.own, title, code, version);
    fake_gba_rom(&gba, hdr, MIB);
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
    memset(&watch, 0, sizeof(watch));
    memset(&ref, 0, sizeof(ref));
    cancel = false;
    const Gen3CartProgress p = {
        .cb = prog_cb,
        .ctx = &prog,
        .cancel = &cancel,
    };
    gen3_cart_init(&cart, &cli, &api, buf, &p);
}

static void insert(const char* code, FakeSaveKind kind) {
    insert_cart("POKEMON RUBY", code, 0, kind);
}

static bool backed_up(void) {
    if(!default_save()) return false;
    insert("AXVE", FakeSaveFlash);
    return gen3_cart_connect(&cart) == Gen3CartOk && gen3_cart_backup(&cart, STAMP) == Gen3CartOk;
}

static const uint8_t* file(const char* name, uint32_t* size) {
    *size = 0;
    return fake_store_file(&store, name, size);
}

/* The .sav named holds the fake flash as it is, and no temporary backup is left */
static bool sav_is_flash(const char* name) {
    uint32_t size;
    const uint8_t* s = file(name, &size);
    return s && size == SAVE_LEN && memcmp(s, flash(), SAVE_LEN) == 0 &&
           fake_store_count(&store, ".tmp") == 0;
}

/* Puts a whole file into the store */
static bool put_file(const char* name, const uint8_t* data, uint32_t len) {
    void* f = api.open(api.ctx, name, Gen3StoreCreate);
    if(!f) return false;
    bool ok = true;
    for(uint32_t off = 0; off < len && ok; off += SECTOR_LEN) {
        const uint32_t n = len - off < SECTOR_LEN ? len - off : SECTOR_LEN;
        ok = api.write(api.ctx, f, off, data + off, n);
    }
    return api.close(api.ctx, f) && ok;
}

/* The golden Treecko at the level, through the editor */
static bool treecko(uint8_t level, Gen3Pkm* out) {
    Gen3Pkm p;
    if(!gen3_save_mon_to_pkm(party[0], 100, &p) || !gen3_edit_load(&ed, &p)) return false;
    gen3_edit_set_level(&ed, level);
    *out = ed.pkm;
    return true;
}

/* Party slot 0 at level 50 */
static bool edit_party(void) {
    Gen3Pkm p;
    return treecko(50, &p) && gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p) == Gen3CartOk;
}

static uint32_t calls(uint8_t code) {
    return hook.calls[code];
}

static FakeGbaCounts counts(void) {
    FakeGbaCounts n;
    fake_gba_counts(&gba, &n);
    return n;
}

static bool no_flash_ops(void) {
    const FakeGbaCounts n = counts();
    return n.erases + n.programs + n.commits + n.chip_erases == 0;
}

/* The watch, before a write: the save the backup holds (the flash's loaded slot), and after it the
 * working copy's 14 sectors in the other slot with the counter one on */
static void watch_write(void) {
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    watch = (Watch){
        .on = true,
        .game = gen3_cart_id(&cart)->game,
        .old_slot = s->active,
        .old_counter = s->save_counter,
        .new_slot = (uint8_t)((s->save_counter + 1u) % 2u),
        .new_counter = s->save_counter + 1u,
    };
    uint32_t size;
    const uint8_t* w = file(GEN3_CART_WORK_NAME, &size);
    for(uint32_t k = 0; k < 14; k++) {
        memcpy(want_old[k], flash() + s->phys[k] * SECTOR_LEN, DATA_LEN);
        if(w && size == WORK_LEN) memcpy(want_new[k], w + k * SECTOR_LEN, DATA_LEN);
    }
}

/* The watch's new save: a backup file's slot, as a restore puts it, its bytes past each section
 * zero */
static void watch_new_from(const uint8_t* sav, const FakeSaveSpec* s, uint8_t slot, uint32_t ctr) {
    watch.new_slot = (uint8_t)(ctr % 2u);
    watch.new_counter = ctr;
    for(uint8_t k = 0; k < 14; k++) {
        memcpy(want_new[k], sav + fake_save_phys(s, slot, k) * SECTOR_LEN, DATA_LEN);
        const uint16_t size = fake_save_size(s->game, k);
        memset(want_new[k] + size, 0, DATA_LEN - size);
    }
}

/* The working copy holds the slot's 14 sectors of the flash, id k at sector at[k] */
static bool work_is_flash(const uint8_t at[14]) {
    uint32_t size;
    const uint8_t* w = file(GEN3_CART_WORK_NAME, &size);
    if(!w || size != WORK_LEN) return false;
    for(uint32_t k = 0; k < 14; k++) {
        if(memcmp(w + k * SECTOR_LEN, flash() + at[k] * SECTOR_LEN, SECTOR_LEN) != 0) return false;
    }
    return true;
}

/* Sector s of the flash: id, counter, and whether the game takes it */
static bool sector_is(uint8_t s, uint16_t id, uint32_t counter) {
    Gen3SaveSectorInfo i;
    gen3_save_sector_info(Gen3LinkGameRS, flash() + s * SECTOR_LEN, &i);
    return i.valid && i.full_ok && i.f.id == id && i.f.counter == counter;
}

/* The Pokemon the flash holds at that party or box slot of the slot whose id k is at at[k] */
static bool flash_mon(const uint8_t at[14], uint8_t box, uint8_t slot, uint8_t out[100]) {
    Gen3SaveSpan sp[2];
    const uint8_t n = gen3_save_mon_spans(Gen3LinkGameRS, box, slot, sp);
    if(n == 0) return false;
    uint32_t o = 0;
    for(uint8_t i = 0; i < n; i++) {
        memcpy(out + o, flash() + at[sp[i].id] * SECTOR_LEN + sp[i].off, sp[i].len);
        o += sp[i].len;
    }
    return true;
}

/* id k at 14 * slot + (k + first) % 14 */
static void slot_at(uint8_t slot, uint8_t first, uint8_t at[14]) {
    for(uint8_t k = 0; k < 14; k++) at[k] = (uint8_t)(14u * slot + (k + first) % 14u);
}

/* After the run: the new save, backed up, the edits gone */
static bool written_and_backed_up(void) {
    return game_view() == ViewNew && gen3_cart_backup_current(&cart) &&
           gen3_cart_changes(&cart) == 0 && sav_is_flash(gen3_cart_backup_name(&cart)) &&
           fake_store_count(&store, ".tmp") == 0 && cart.wslot == NONE;
}

/* ---- Snapshots, for the sweeps ---- */

static FakeGba snap_gba;
static FakeHelper snap_fh;
static Gen3Hcli snap_cli;
static Gen3Cart snap_cart;
static FakeStore snap_store;
static Watch snap_watch;

static void snap_take(void) {
    snap_gba = gba;
    snap_fh = fh;
    snap_cli = cli;
    snap_cart = cart;
    snap_watch = watch;
    fake_store_copy(&snap_store, &store);
}

/* The state at snap_take, with a fresh transport (its transfer cap counts from here) and fresh
 * observers */
static void snap_restore(void) {
    gba = snap_gba;
    fh = snap_fh;
    cli = snap_cli;
    cart = snap_cart;
    fake_store_copy(&store, &snap_store);
    tap.inner = fake_helper_transport(&fh, &pump);
    tap.exchanges = 0;
    tap.fake_code = 0;
    tap.over_code = 0;
    memset(&hook, 0, sizeof(hook));
    memset(&prog, 0, sizeof(prog));
    memset(&ref, 0, sizeof(ref));
    watch = snap_watch;
    watch.last_ops = flash_ops();
    watch.views = 0;
}

/* ---- The write ---- */

TEST(test_write_party_edit) {
    ASSERT_TRUE(backed_up());
    memcpy(before, flash(), SAVE_LEN);
    ASSERT_TRUE(edit_party());
    watch_write();
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_TRUE(watch.old_seen > 0);
    ASSERT_EQ(game_view(), ViewNew);
    /* slot B: id k at 14 + (k + 1) % 14, counter 5 */
    uint8_t b[14];
    slot_at(1, 1, b);
    for(uint8_t k = 0; k < 14; k++) ASSERT_TRUE(sector_is(b[k], k, 5));
    /* slot A and sectors 28..31 as they were */
    ASSERT_MEM_EQ(flash(), before, 14 * SECTOR_LEN);
    ASSERT_MEM_EQ(flash() + 28 * SECTOR_LEN, before + 28 * SECTOR_LEN, 4 * SECTOR_LEN);
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    ASSERT_EQ(s->result, Gen3SaveOk);
    ASSERT_EQ(s->active, 1);
    ASSERT_EQ(s->save_counter, 5);
    ASSERT_TRUE(s->strict_ok);
    ASSERT_FALSE(s->padded);
    for(uint8_t k = 0; k < 14; k++) ASSERT_EQ(s->phys[k], b[k]);
    /* party 0 at level 50 on the cart */
    uint8_t mon[100];
    ASSERT_TRUE(flash_mon(b, GEN3_SAVE_PARTY, 0, mon));
    Gen3Pkm p;
    ASSERT_TRUE(gen3_save_mon_to_pkm(mon, 100, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldLevel), 50);
    /* one upload, 13 copies; 14 erases and commits */
    ASSERT_EQ(calls(GEN3_HCMD_WRITE_SECTOR), 1);
    ASSERT_EQ(calls(GEN3_HCMD_COPY_SECTOR), 13);
    ASSERT_EQ(calls(GEN3_HCMD_ERASE_SECTOR), 14);
    ASSERT_EQ(calls(GEN3_HCMD_COMMIT_SECTOR), 14);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 1);
    ASSERT_EQ(counts().commits, 14);
    ASSERT_EQ(counts().chip_erases, 0);
    /* a second .sav, the cart as it is now, and the working copy built from it */
    ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), WRITE_NAME);
    ASSERT_TRUE(sav_is_flash(WRITE_NAME));
    ASSERT_TRUE(work_is_flash(b));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(cart.dirty, 0);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    ASSERT_EQ(cart.wslot, NONE);
    ASSERT_FALSE(gen3_cart_pending(&cart));
    /* the result says to power-cycle the GBA, which ends the helper: a reconnect first */
    ASSERT_FALSE(gen3_cart_linked(&cart));
    Gen3SaveTrainer t;
    ASSERT_TRUE(gen3_cart_trainer(&cart, &t));
    ASSERT_STR_EQ(t.name, "BRENDAN");
    /* the working copy reads the edit */
    size_t len = 0;
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 0, mon, &len), Gen3CartOk);
    ASSERT_TRUE(gen3_save_mon_to_pkm(mon, 100, &p));
    ASSERT_EQ(gen3_pkm_get(&p, Gen3FieldLevel), 50);
    /* the steps, in full */
    ASSERT_EQ(prog.last_done[Gen3CartStepPrepare], 15);
    ASSERT_EQ(prog.last_done[Gen3CartStepErase], 14);
    ASSERT_EQ(prog.last_done[Gen3CartStepProgram], 14);
    ASSERT_EQ(prog.last_done[Gen3CartStepCommit], 14);
    ASSERT_EQ(prog.last_done[Gen3CartStepVerify], 15);
    ASSERT_EQ(prog.last_done[Gen3CartStepPostBackup], 96);
    ASSERT_EQ(prog.last_total[Gen3CartStepPostBackup], 96);
}

TEST(test_write_box_straddle) {
    ASSERT_TRUE(backed_up());
    Gen3Pkm p;
    ASSERT_TRUE(treecko(42, &p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    watch_write();
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(calls(GEN3_HCMD_WRITE_SECTOR), 2);
    ASSERT_EQ(calls(GEN3_HCMD_COPY_SECTOR), 12);
    uint8_t b[14];
    slot_at(1, 1, b);
    /* ids 5 and 6 were uploaded, the rest copied */
    ASSERT_EQ(hook.copy_dst & ((1u << b[5]) | (1u << b[6])), 0);
    uint8_t mon[100];
    ASSERT_TRUE(flash_mon(b, 1, 19, mon));
    uint8_t want[100];
    gen3_save_pkm_to_mon(&p, 80, want);
    ASSERT_MEM_EQ(mon, want, 80);
    size_t len = 0;
    ASSERT_EQ(gen3_cart_mon_get(&cart, 1, 19, mon, &len), Gen3CartOk);
    ASSERT_MEM_EQ(mon, want, 80);
}

TEST(test_write_twice) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    Gen3Pkm p;
    ASSERT_TRUE(treecko(60, &p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    watch_write();
    ASSERT_EQ(watch.new_slot, 0);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP3), Gen3CartOk);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
    /* slot A again, counter 6, id 0 two on */
    uint8_t a[14];
    slot_at(0, 2, a);
    for(uint8_t k = 0; k < 14; k++) ASSERT_TRUE(sector_is(a[k], k, 6));
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    ASSERT_EQ(s->active, 0);
    ASSERT_EQ(s->save_counter, 6);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 3);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    /* both edits are in the save */
    uint8_t mon[100];
    ASSERT_TRUE(flash_mon(a, GEN3_SAVE_PARTY, 0, mon));
    Gen3Pkm back;
    ASSERT_TRUE(gen3_save_mon_to_pkm(mon, 100, &back));
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldLevel), 50);
    ASSERT_TRUE(flash_mon(a, 1, 19, mon));
    ASSERT_TRUE(gen3_save_mon_to_pkm(mon, 80, &back));
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldLevel), 60);
}

TEST(test_write_nothing) {
    ASSERT_TRUE(backed_up());
    const uint32_t xfers = fake_helper_transfers(&fh);
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartNothing);
    ASSERT_EQ(fake_helper_transfers(&fh), xfers);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_TRUE(no_flash_ops());
}

TEST(test_write_sd_full_precheck) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    /* the .sav and the working copy, and one byte short of a backup and a sector */
    store.capacity = SAVE_LEN + WORK_LEN + 135167u;
    const uint32_t xfers = fake_helper_transfers(&fh);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartSdFull);
    ASSERT_EQ(fake_helper_transfers(&fh), xfers);
    ASSERT_TRUE(no_flash_ops());
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    store.capacity = SAVE_LEN + WORK_LEN + 135168u;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
}

TEST(test_write_requires_backup) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    fake_store_fail_write_after(&store, 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP2), Gen3CartSdError);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartRefused);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_TRUE(no_flash_ops());
}

TEST(test_write_refused_not_strict) {
    ASSERT_TRUE(default_save());
    fake_save_set_counter(img, phys(0, 13), 2);
    insert("AXVE", FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
    ASSERT_TRUE(edit_party());
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartRefused);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
    ASSERT_TRUE(no_flash_ops());
}

/* A loaded sector with a byte past its section that isn't zero, its checksum over the section
 * right: the game loads it, the helper wouldn't keep it, the Flipper doesn't write */
TEST(test_write_refused_padded) {
    ASSERT_TRUE(default_save());
    img[phys(0, 0) * SECTOR_LEN + 0x890] = 0x5A;
    insert("AXVE", FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
    ASSERT_TRUE(gen3_cart_slots(&cart)->padded);
    ASSERT_TRUE(edit_party());
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartRefused);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 0);
    ASSERT_TRUE(no_flash_ops());
}

TEST(test_write_refused_emerald) {
    ASSERT_TRUE(save_for(FakeGameE));
    insert_cart("POKEMON EMER", "BPEE", 0, FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_browsable(&cart));
    ASSERT_EQ(gen3_cart_party_count(&cart), 3);
    ASSERT_TRUE(edit_party());
    Gen3CartRefuse why = Gen3CartRefuseNone;
    ASSERT_FALSE(gen3_cart_writable(&cart, &why));
    ASSERT_EQ(why, Gen3CartRefuseEmerald);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartRefused);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
    ASSERT_TRUE(no_flash_ops());
}

/* Carts never written: no erase, program or commit reaches them, and on those the helper can't read
 * not even a bank select */
TEST(test_write_refused_no_flash_ops) {
    static const struct {
        const char* code;
        FakeSaveKind save;
        bool other_lib;
        Gen3CartResult backup;
        Gen3CartRefuse why;
    } carts[] = {
        {"AXVE", FakeSaveSram, false, Gen3CartRefused, Gen3CartRefuseSram},
        {"AXVE", FakeSaveNone, false, Gen3CartRefused, Gen3CartRefuseChip},
        {"AXVE", FakeSaveFlash, true, Gen3CartOk, Gen3CartRefuseLibrary},
        {"AXVD", FakeSaveFlash, false, Gen3CartOk, Gen3CartRefuseRegion},
    };
    for(uint32_t i = 0; i < sizeof(carts) / sizeof(carts[0]); i++) {
        ASSERT_TRUE(default_save());
        insert(carts[i].code, carts[i].save);
        if(carts[i].other_lib) fake_gba_rom_string(&gba, 0x50000, "SRAM_V113");
        ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
        ASSERT_EQ(gen3_cart_id(&cart)->why, carts[i].why);
        ASSERT_EQ(gen3_cart_backup(&cart, STAMP), carts[i].backup);
        if(carts[i].backup == Gen3CartOk) ASSERT_TRUE(edit_party());
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartRefused);
        ASSERT_FALSE(gen3_cart_restorable(&cart));
        ASSERT_EQ(gen3_cart_restore(&cart, BACKUP_NAME, STAMP2), Gen3CartRefused);
        ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
        const FakeGbaCounts n = counts();
        ASSERT_EQ(n.erases + n.programs + n.commits + n.chip_erases, 0);
        if(carts[i].save != FakeSaveFlash) ASSERT_EQ(n.bank_selects, 0);
        if(carts[i].save == FakeSaveSram) ASSERT_EQ(n.sram_writes, 2);
    }
}

/* Slot B holds an older save with a broken sector: the write goes over it */
TEST(test_write_over_corrupt_inactive) {
    ASSERT_TRUE(save_with_older_b());
    fake_save_break_checksum(img, phys(1, 3));
    insert("AXVE", FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_EQ(gen3_cart_slots(&cart)->result, Gen3SaveError);
    ASSERT_EQ(gen3_cart_slots(&cart)->active, 0);
    ASSERT_TRUE(edit_party());
    watch_write();
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
    ASSERT_EQ(gen3_cart_slots(&cart)->result, Gen3SaveOk);
    ASSERT_EQ(gen3_cart_slots(&cart)->active, 1);
}

/* The save kept changed since the backup, its footer with it: nothing is armed */
TEST(test_write_cart_changed) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    flash()[phys(0, 3) * SECTOR_LEN + 0x10] ^= 0x01;
    fake_save_fix(flash(), phys(0, 3), FakeGameRS);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartChanged);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
    ASSERT_TRUE(no_flash_ops());
    /* the backup is no longer the cart's: no write until a new one */
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartRefused);
    bool dropped = false;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP3, &dropped), Gen3CartOk);
    ASSERT_TRUE(dropped);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
}

/* A change that keeps every footer and both checksums: two words of slot A's id 2, one up and one
 * down. Only SECTOR_CRC32 shows it, before arming. */
TEST(test_write_cart_changed_same_footer) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    snap_take();
    for(uint8_t id = 0; id < 14; id++) {
        snap_restore();
        uint8_t* d = flash() + phys(0, id) * SECTOR_LEN;
        uint32_t w0 = (uint32_t)d[0] | (uint32_t)d[1] << 8 | (uint32_t)d[2] << 16 |
                      (uint32_t)d[3] << 24;
        uint32_t w1 = (uint32_t)d[4] | (uint32_t)d[5] << 8 | (uint32_t)d[6] << 16 |
                      (uint32_t)d[7] << 24;
        w0 += 1u;
        w1 -= 1u;
        for(uint32_t i = 0; i < 4; i++) {
            d[i] = (uint8_t)(w0 >> (8 * i));
            d[4 + i] = (uint8_t)(w1 >> (8 * i));
        }
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartChanged);
        ASSERT_TRUE((hook.crc_sectors >> phys(0, id)) & 1u);
        ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
        ASSERT_TRUE(no_flash_ops());
        ASSERT_FALSE(gen3_cart_backup_current(&cart));
    }
}

/* A save in slot B since the backup, which the backup doesn't hold: with counter 5 the game loads
 * it, as it would a finished write, but it holds other data; with counter 6 it still loads slot A
 * (the counter's parity), but not with the counter backed up; with counter 3, an older save, the
 * game still loads slot A as backed up, but slot B is no longer what the backup has there. Each is
 * another save: nothing is armed, and it stays byte for byte. */
TEST(test_write_game_saved_since) {
    static const uint32_t counters[] = {5, 6, 3};
    for(uint32_t i = 0; i < 3; i++) {
        ASSERT_TRUE(backed_up());
        ASSERT_TRUE(edit_party());
        FakeSaveSpec s = spec;
        s.slot_present[1] = true;
        s.counter[1] = counters[i];
        s.first[1] = 1;
        fake_save_build(img2, &s);
        memcpy(flash() + 14 * SECTOR_LEN, img2 + 14 * SECTOR_LEN, 14 * SECTOR_LEN);
        memcpy(before, flash(), SAVE_LEN);
        hook.crc_sectors = 0;
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartChanged);
        ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
        ASSERT_TRUE(no_flash_ops());
        ASSERT_MEM_EQ(flash(), before, SAVE_LEN);
        ASSERT_FALSE(gen3_cart_backup_current(&cart));
        /* the counter-5 save was read on the cart against the write's first image; the others
         * needed only their footers */
        ASSERT_EQ(hook.crc_sectors, i == 0 ? 1u << 15 : 0u);
    }
}

/* A flash program that keeps failing: FlashError, the save as it was, the helper no longer armed */
TEST(test_write_flash_error) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    uint8_t b[14];
    slot_at(1, 1, b);
    fake_gba_fail_program(&gba, b[1], 0, 99);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartFlashError);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewOld);
    ASSERT_EQ(calls(GEN3_HCMD_COMMIT_SECTOR), 0);
    ASSERT_EQ(calls(GEN3_HCMD_DISARM), 1);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_EQ(gen3_hcli_erase_sector(&cli, b[0]), Gen3HcliStatusError);
    ASSERT_EQ(cli.last_status, GEN3_HST_NOT_ARMED);
    gba.fail_program_times = 0;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
    /* a backup describes slot B as it is: no slot is left unchecked */
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    fake_gba_fail_program(&gba, b[1], 0, 99);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartFlashError);
    ASSERT_EQ(cart.wslot, 1);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP3), Gen3CartOk);
    ASSERT_EQ(cart.wslot, NONE);
}

/* A commit is never sent again: a status but OK is answered by reading the sector. When it holds
 * its image the write goes on; when it doesn't, the commit's own result, and the arming ends. */
TEST(test_write_commit_status) {
    static const struct {
        bool ran; /* the helper ran the commit; its answer went wrong after */
        uint8_t status;
        uint16_t fail;
        Gen3CartResult want;
    } cases[] = {
        {true, GEN3_HST_REFUSED, 6, Gen3CartOk},
        {true, GEN3_HST_REFUSED, 7, Gen3CartOk},
        {true, GEN3_HST_NOT_ERASED, 0, Gen3CartOk},
        {true, GEN3_HST_NOT_ARMED, 0, Gen3CartOk},
        {false, GEN3_HST_REFUSED, 7, Gen3CartRefused},
        {false, GEN3_HST_REFUSED, 6, Gen3CartRefused},
        {false, GEN3_HST_NOT_ERASED, 0, Gen3CartFlashError},
        {false, GEN3_HST_PROGRAM_FAIL, 0, Gen3CartFlashError},
    };
    for(uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASSERT_TRUE(backed_up());
        ASSERT_TRUE(edit_party());
        watch_write();
        /* the third commit: run by the helper, or answered without it */
        if(cases[i].ran) {
            tap.over_code = GEN3_HCMD_COMMIT_SECTOR;
            tap.over_nth = 3;
            tap.over_status = cases[i].status;
            tap.over_fail = cases[i].fail;
            tap.over_n = 2;
        } else {
            tap.fake_code = GEN3_HCMD_COMMIT_SECTOR;
            tap.fake_nth = 3;
            tap.fake_status = cases[i].status;
            tap.fake_fail = cases[i].fail;
            tap.fake_n = 2;
        }
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), cases[i].want);
        ASSERT_EQ(watch.violations, 0);
        if(cases[i].want == Gen3CartOk) {
            ASSERT_EQ(game_view(), ViewNew);
            ASSERT_EQ(calls(GEN3_HCMD_COMMIT_SECTOR), 14);
            ASSERT_TRUE(sav_is_flash(WRITE_NAME));
            ASSERT_EQ(gen3_cart_changes(&cart), 0);
            continue;
        }
        ASSERT_EQ(game_view(), ViewOld);
        ASSERT_EQ(calls(GEN3_HCMD_COMMIT_SECTOR), 2);
        ASSERT_EQ(calls(GEN3_HCMD_DISARM), 1);
        ASSERT_TRUE(gen3_cart_backup_current(&cart));
        ASSERT_EQ(gen3_cart_changes(&cart), 1);
        uint8_t b[14];
        slot_at(1, 1, b);
        ASSERT_EQ(gen3_hcli_erase_sector(&cli, b[0]), Gen3HcliStatusError);
        ASSERT_EQ(cli.last_status, GEN3_HST_NOT_ARMED);
        /* written again from the start, over two sectors of slot B made valid */
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
        ASSERT_EQ(watch.violations, 0);
        ASSERT_EQ(game_view(), ViewNew);
        ASSERT_EQ(counts().erases, 28);
    }
}

/* A sound answer the client can't take, to a write-class command the helper ran: what it did is
 * unknown, so no job talks to the helper until a reconnect, and the next write reads the cart */
TEST(test_write_bad_response_relinks) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    tap.over_code = GEN3_HCMD_ERASE_SECTOR;
    tap.over_nth = 2;
    tap.over_status = GEN3_HST_OK;
    tap.over_n = 3;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartHelperError);
    ASSERT_FALSE(cart.linked);
    ASSERT_EQ(cart.relink_us, GEN3_HCLI_BUSY_FLASH_US + GEN3_HCLI_SYNC_US);
    ASSERT_EQ(calls(GEN3_HCMD_DISARM), 0);
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
}

/* The card gives an image back otherwise than the write's preparing read it, in a byte no checksum
 * covers: caught before the upload, nothing committed */
TEST(test_write_upload_differs) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    prog.corrupt_step = Gen3CartStepErase;
    prog.corrupt_at = 14;
    prog.corrupt_name = GEN3_CART_WORK_NAME;
    prog.corrupt_off = 1u * SECTOR_LEN + 0xFA0u;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartVerifyFailed);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewOld);
    ASSERT_EQ(calls(GEN3_HCMD_WRITE_SECTOR), 0);
    ASSERT_EQ(calls(GEN3_HCMD_COMMIT_SECTOR), 0);
    ASSERT_EQ(calls(GEN3_HCMD_DISARM), 1);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    fake_store_corrupt_read(&store, NULL, 0);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(game_view(), ViewNew);
}

/* The working copy can't take its sectors back while preparing: no link traffic, and it is no
 * longer trusted */
TEST(test_write_prepare_sd_error) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    fake_store_fail_write_after(&store, 3);
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartSdError);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartRefused);
    fake_store_fail_write_after(&store, 1000);
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartOk);
    ASSERT_TRUE(edit_party());
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
}

/* ---- Lost links and power cuts ---- */

/* The cable out for good right after the 14th COMMIT ran: the slot is whole. The reconnect finds
 * the write done, each new sector holding its image: it writes nothing, makes the backup the write
 * didn't, named with the write's stamp, and the edit is on the cart. Nothing is left to write. */
TEST(test_write_retry_after_commit) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 14;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    fake_helper_replug(&fh);
    const FakeGbaCounts n = counts();
    const uint32_t arms = calls(GEN3_HCMD_ARM_WRITE);
    const uint32_t crcs = calls(GEN3_HCMD_SECTOR_CRC32);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedWrite);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), arms);
    /* the 14 new sectors, then all 32 for the new backup */
    ASSERT_EQ(calls(GEN3_HCMD_SECTOR_CRC32), crcs + 14 + 32);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_TRUE(written_and_backed_up());
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), WRITE_NAME);
    ASSERT_FALSE(gen3_cart_pending(&cart));
    ASSERT_FALSE(gen3_cart_linked(&cart));
    uint8_t b[14];
    slot_at(1, 1, b);
    ASSERT_TRUE(work_is_flash(b));
    ASSERT_EQ(gen3_cart_write(&cart, STAMP4), Gen3CartNothing);
}

/* The cable out for good right after the 7th COMMIT's response: 7 sectors of slot B valid, so the
 * game still loads A. After the reconnect the write starts again from the arming. */
TEST(test_write_retry_mid_commit) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    prog.unplug_step = Gen3CartStepCommit;
    prog.unplug_at = 7;
    prog.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(calls(GEN3_HCMD_COMMIT_SECTOR), 7);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewOld);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedNone);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 2);
    ASSERT_EQ(counts().erases, 28);
    ASSERT_EQ(calls(GEN3_HCMD_COMMIT_SECTOR), 21);
    ASSERT_EQ(counts().commits, 21);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
    ASSERT_TRUE(sav_is_flash(WRITE_NAME));
}

/* 30 transfers lost in the middle of the WRITE_SECTOR upload: sent again, the write goes on */
TEST(test_write_short_glitch) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    /* after COPY of id 0, the upload of id 1 */
    prog.unplug_step = Gen3CartStepProgram;
    prog.unplug_at = 1;
    prog.unplug_after = 1000;
    prog.unplug_len = 30;
    const uint32_t resends = cli.resends;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_TRUE(cli.resends >= resends + 1u);
    ASSERT_EQ(calls(GEN3_HCMD_WRITE_SECTOR), 1);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
}

/* Acts on the flash after the last commit, as a cart that changed on its own would */
static uint8_t act_k; /* the id whose new sector new_sector_differs changes */

static void new_sector_differs(void) {
    uint8_t b[14];
    slot_at(1, 1, b);
    flash()[b[act_k] * SECTOR_LEN + 0x100] ^= 0x01;
    fake_save_fix(flash(), b[act_k], FakeGameRS);
}

static void old_slot_newer(void) {
    for(uint8_t k = 0; k < 14; k++) fake_save_set_counter(flash(), phys(0, k), 6);
}

static uint8_t act_sector; /* the sector other_sector_differs changes */

static void other_sector_differs(void) {
    flash()[act_sector * SECTOR_LEN + 0x10] ^= 0x01;
}

/* The check after the commits: a new sector, any of the 14, that doesn't hold its image, its
 * footer valid; or every new sector right, but the game loading the other slot. VerifyFailed
 * either way; the backup stays (the slot it holds wasn't written), and no new one is made. */
TEST(test_write_verify_fails) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    snap_take();
    for(uint8_t i = 0; i <= 14; i++) {
        snap_restore();
        act_k = i;
        hook.act_code = GEN3_HCMD_COMMIT_SECTOR;
        hook.act_nth = 14;
        hook.act = i < 14 ? new_sector_differs : old_slot_newer;
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartVerifyFailed);
        ASSERT_TRUE(gen3_cart_backup_current(&cart));
        ASSERT_STR_EQ(gen3_cart_backup_name(&cart), BACKUP_NAME);
        ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
        ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
        ASSERT_EQ(gen3_cart_changes(&cart), 1);
        /* the slot kept, then the new one up to the sector that differs */
        ASSERT_EQ(calls(GEN3_HCMD_SECTOR_CRC32), 14 + (i < 14 ? i + 1 : 14));
    }
}

/* A write lost right after its last commit, then a new sector, any of the 14, changed on the cart
 * (its footer valid) before the reconnect: the game loads that slot with the write's counter, but
 * it isn't the write's own save. The reconnect stops, Changed, writing nothing: the backup and the
 * edit go, and so does the write's claim on the slot. Nothing is written until a new backup. */
TEST(test_write_written_differs) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    snap_take();
    for(uint8_t k = 0; k < 14; k++) {
        snap_restore();
        hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
        hook.unplug_nth = 14;
        hook.unplug_len = FOREVER;
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
        act_k = k;
        new_sector_differs();
        fake_helper_replug(&fh);
        const FakeGbaCounts n = counts();
        const uint32_t crcs = calls(GEN3_HCMD_SECTOR_CRC32);
        ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartChanged);
        /* the new sectors up to the one that differs */
        ASSERT_EQ(calls(GEN3_HCMD_SECTOR_CRC32), crcs + k + 1u);
        ASSERT_EQ(counts().erases, n.erases);
        ASSERT_EQ(counts().programs, n.programs);
        ASSERT_FALSE(gen3_cart_backup_current(&cart));
        ASSERT_FALSE(gen3_cart_pending(&cart));
        ASSERT_EQ(gen3_cart_changes(&cart), 0);
        ASSERT_EQ(cart.dirty, 0);
        ASSERT_FALSE(gen3_cart_work_ok(&cart));
        ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedNone);
        ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
        ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartRefused);
    }
}

/* The GBA loses power: the helper replies to nothing until fake_helper_restart */
static void power_off(void) {
    fh.dead = true;
}

/* The op count at which a power cut stops the 8th commit byte: 14 erases, 14 sectors of 4,095
 * bytes, 7 commit bytes */
#define CUT_IN_COMMIT8 (14u + 14u * 4095u + 7u)

/* The GBA switched on again after a power cut in a write, and the helper sent: a whole connect,
 * then attach, which settles the write */
static Gen3CartResult power_back_on(bool* dropped) {
    fake_helper_replug(&fh);
    fake_helper_restart(&fh);
    const Gen3CartResult r = gen3_cart_connect(&cart);
    return r == Gen3CartOk ? gen3_cart_attach(&cart, STAMP3, dropped) : r;
}

/* ... and finds it cut short before its last commit: still pending, with the backup and the edit,
 * and no Pokemon opened or put until it is written */
static void back_on_pending(void) {
    bool dropped = true;
    ASSERT_EQ(power_back_on(&dropped), Gen3CartOk);
    ASSERT_FALSE(dropped);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedNone);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), BACKUP_NAME);
    Gen3Pkm p;
    ASSERT_TRUE(treecko(70, &p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartRefused);
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
}

/* ... and finds it done: finished with nothing written, the new save backed up with the write's
 * stamp, the edit on the cart and nothing left to write */
static void back_on_finished(void) {
    bool dropped = true;
    const FakeGbaCounts n = counts();
    ASSERT_EQ(power_back_on(&dropped), Gen3CartOk);
    ASSERT_FALSE(dropped);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedWrite);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_TRUE(written_and_backed_up());
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), WRITE_NAME);
    ASSERT_FALSE(gen3_cart_linked(&cart));
    ASSERT_EQ(gen3_cart_write(&cart, STAMP4), Gen3CartNothing);
}

/* The game played and saved into slot B, with counter 5, after a power cut in the write: a whole
 * save there, as the game writes it */
static void game_saved_into_b(void) {
    FakeSaveSpec s = spec;
    s.slot_present[1] = true;
    s.counter[1] = 5;
    s.first[1] = 1;
    s.seed ^= 0x00010000u;
    fake_save_build(img2, &s);
    memcpy(flash() + 14 * SECTOR_LEN, img2 + 14 * SECTOR_LEN, 14 * SECTOR_LEN);
}

/* A power cut in a write, then a whole connect: attach compares the save the game loaded (slot A),
 * not the slot the write armed, and then settles the write. Cut after the 14th commit, it is found
 * done and finished, nothing erased again; cut in the 8th commit, it stays pending, and the next
 * write writes the slot anew; and if the game saved into it meanwhile, attach finds the game's
 * save there, writes nothing, and backs that up, the edit dropped. */
TEST(test_attach_after_power_cut) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    snap_take();
    hook.act_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.act_nth = 14;
    hook.act = power_off;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(game_view(), ViewNew);
    back_on_finished();
    ASSERT_EQ(watch.violations, 0);
    /* in the 8th commit */
    snap_restore();
    fake_gba_power_cut_after(&gba, CUT_IN_COMMIT8);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(game_view(), ViewOld);
    back_on_pending();
    ASSERT_EQ(gen3_cart_write(&cart, STAMP4), Gen3CartOk);
    ASSERT_EQ(counts().erases, 28);
    ASSERT_TRUE(written_and_backed_up());
    ASSERT_EQ(watch.violations, 0);
    /* in the 8th commit, then the game played and saved */
    snap_restore();
    watch.on = false;
    fake_gba_power_cut_after(&gba, CUT_IN_COMMIT8);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    fake_helper_restart(&fh);
    game_saved_into_b();
    memcpy(before, flash(), SAVE_LEN);
    const FakeGbaCounts n = counts();
    const uint32_t crcs = calls(GEN3_HCMD_SECTOR_CRC32);
    bool dropped = false;
    ASSERT_EQ(power_back_on(&dropped), Gen3CartOk);
    ASSERT_TRUE(dropped);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedNone);
    /* the first new sector differed; then the new backup's 32 */
    ASSERT_EQ(calls(GEN3_HCMD_SECTOR_CRC32), crcs + 1 + 32);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 1);
    ASSERT_MEM_EQ(flash(), before, SAVE_LEN);
    ASSERT_FALSE(gen3_cart_pending(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
    ASSERT_EQ(gen3_cart_slots(&cart)->active, 1);
    ASSERT_EQ(gen3_cart_slots(&cart)->save_counter, 5);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    ASSERT_TRUE(gen3_cart_linked(&cart));
}

/* The link lost for 1,200 transfers right after the 5th COMMIT ran: LinkLost after 5 commits, none
 * sent after it, and no job talks to the helper until a reconnect, which tries for a commit's
 * budget plus a sync */
TEST(test_write_commit_link_lost) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 5;
    hook.unplug_len = 1200;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(calls(GEN3_HCMD_COMMIT_SECTOR), 5);
    ASSERT_EQ(counts().commits, 5);
    ASSERT_FALSE(cart.linked);
    ASSERT_EQ(cart.relink_us, GEN3_HLINK_BUSY_US + GEN3_HCLI_SYNC_US);
    ASSERT_EQ(cart.wslot, 1);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewOld);
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(counts().erases, 28);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_TRUE(written_and_backed_up());
}

/* A write whose slot the game loads, not yet backed up, then Discard while the cart is linked:
 * the edits go, and the write's claim on its slot. The footers, read again with no slot skipped,
 * show that slot changed, so the backup goes too: the Flipper never shows the old save while the
 * game loads the new one. A whole connect then backs up the cart as it is, the edited save the game
 * loads; a new edit writes over the old slot. A check that failed with every commit done leaves
 * the cart so, and linked; a write cut right after its 14th commit doesn't, as the reconnect or
 * attach that comes first finds it done and finishes it (test_write_retry_after_commit,
 * test_attach_after_power_cut). */
TEST(test_discard_after_write) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    snap_take();
    for(uint32_t k = 0; k < 14; k += 13) {
        snap_restore();
        hook.act_code = GEN3_HCMD_COMMIT_SECTOR;
        hook.act_nth = 14;
        act_k = (uint8_t)k;
        hook.act = new_sector_differs;
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartVerifyFailed);
        ASSERT_EQ(cart.wslot, 1);
        ASSERT_TRUE(gen3_cart_pending(&cart));
        ASSERT_TRUE(gen3_cart_linked(&cart));
        const uint32_t footers = calls(GEN3_HCMD_READ_FOOTERS);
        ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartChanged);
        ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), footers + 1);
        ASSERT_EQ(gen3_cart_changes(&cart), 0);
        ASSERT_EQ(cart.dirty, 0);
        ASSERT_EQ(cart.wslot, NONE);
        ASSERT_FALSE(gen3_cart_backup_current(&cart));
        ASSERT_FALSE(gen3_cart_browsable(&cart));
        ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
        ASSERT_EQ(gen3_cart_write(&cart, STAMP4), Gen3CartRefused);
        /* the game loads the new slot */
        Gen3SaveSectorInfo info[28];
        for(uint32_t s = 0; s < 28; s++) {
            gen3_save_sector_info(Gen3LinkGameRS, flash() + s * SECTOR_LEN, &info[s]);
        }
        Gen3SaveSlots v;
        gen3_save_analyze(info, &v);
        ASSERT_EQ(v.active, 1);
        ASSERT_EQ(v.save_counter, 5);
        fake_helper_restart(&fh);
        ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
        bool dropped = true;
        ASSERT_EQ(gen3_cart_attach(&cart, STAMP3, &dropped), Gen3CartOk);
        ASSERT_FALSE(dropped);
        ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
        ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
        const Gen3SaveSlots* s = gen3_cart_slots(&cart);
        ASSERT_EQ(s->active, 1);
        ASSERT_EQ(s->save_counter, 5);
        /* the edit is in the working copy now: a new one goes to slot A, counter 6 */
        Gen3Pkm p;
        ASSERT_TRUE(treecko(70, &p));
        ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
        ASSERT_EQ(gen3_cart_write(&cart, STAMP4), Gen3CartOk);
        ASSERT_EQ(gen3_cart_slots(&cart)->active, 0);
        ASSERT_EQ(gen3_cart_slots(&cart)->save_counter, 6);
        ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    }
}

/* Discard while linked after a write that changed nothing on the cart (the cable out right after
 * the arming): its slot is as backed up, so the backup stays, and the working copy is the backup's
 * again. Not linked after a write that committed: no footers to read, so Discard ends the write's
 * claim on its slot, and the next reconnect compares that slot and finds the cart Changed. */
TEST(test_discard_after_arming) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    hook.unplug_code = GEN3_HCMD_ARM_WRITE;
    hook.unplug_nth = 1;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(cart.wslot, 1);
    ASSERT_TRUE(no_flash_ops());
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    const uint32_t footers = calls(GEN3_HCMD_READ_FOOTERS);
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartOk);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), footers + 1);
    ASSERT_EQ(cart.wslot, NONE);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    uint8_t at[14];
    slot_at(0, 0, at);
    ASSERT_TRUE(work_is_flash(at));

    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 14;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_FALSE(gen3_cart_linked(&cart));
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartOk);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_EQ(cart.wslot, NONE);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartChanged);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
}

/* The first of slot B's sectors that the write committed, with its counter 5 */
static uint8_t committed_sector(void) {
    for(uint8_t s = 14; s < 28; s++) {
        Gen3SaveSectorInfo i;
        gen3_save_sector_info(Gen3LinkGameRS, flash() + s * SECTOR_LEN, &i);
        if(i.valid && i.f.counter == 5) return s;
    }
    return NONE;
}

/* What test_reconnect_slot_rules changes on the cart before the reconnect */
typedef enum {
    SlotRuleNone,
    SlotRuleNotValid, /* a sector of the write's slot: the signature, not valid, counter 3 */
    SlotRuleCounterN, /* a sector of the write's slot: the backup's counter N (4) */
    SlotRuleKeptSlot, /* a sector of the slot kept: the write's counter N + 1 (5) */
} SlotRule;

/* A write cut after its 7th commit (slot B, counter 5), the change, then a reconnect. ok: the
 * backup is still current afterwards. */
static Gen3CartResult reconnect_after(SlotRule rule, bool* ok) {
    *ok = false;
    if(!backed_up() || !edit_party()) return Gen3CartBadArg;
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 7;
    hook.unplug_len = FOREVER;
    if(gen3_cart_write(&cart, STAMP2) != Gen3CartLinkLost) return Gen3CartBadArg;
    if(cart.wslot != 1 || cart.wcounter != 5) return Gen3CartBadArg;
    const uint8_t s = committed_sector();
    if(s == NONE) return Gen3CartBadArg;
    switch(rule) {
    case SlotRuleNone:
        break;
    case SlotRuleNotValid:
        fake_save_set_counter(flash(), s, 3);
        fake_save_break_checksum(flash(), s);
        break;
    case SlotRuleCounterN:
        fake_save_set_counter(flash(), s, 4);
        break;
    case SlotRuleKeptSlot:
        fake_save_set_counter(flash(), phys(0, 3), 5);
        break;
    }
    fake_helper_replug(&fh);
    const Gen3CartResult r = gen3_cart_reconnect(&cart);
    *ok = gen3_cart_backup_current(&cart);
    return r;
}

/* While a write's slot is skipped, a sector there stands only if it is the backup's, has no
 * signature, or has the write's counter N + 1; the slot kept must be the backup's exactly. So the
 * signature on a sector that isn't valid (not "no signature"), the backup's own counter N in the
 * write's slot (not N + 1), and the write's counter in the slot kept (the rule is the write's slot
 * only) are each another save: Changed, the backup dropped. */
TEST(test_reconnect_slot_rules) {
    bool current = false;
    ASSERT_EQ(reconnect_after(SlotRuleNone, &current), Gen3CartOk);
    ASSERT_TRUE(current);
    ASSERT_EQ(reconnect_after(SlotRuleNotValid, &current), Gen3CartChanged);
    ASSERT_FALSE(current);
    ASSERT_EQ(reconnect_after(SlotRuleCounterN, &current), Gen3CartChanged);
    ASSERT_FALSE(current);
    ASSERT_EQ(reconnect_after(SlotRuleKeptSlot, &current), Gen3CartChanged);
    ASSERT_FALSE(current);
}

/* ---- A write left pending, and what settles it ---- */

static uint8_t orig[100];
static size_t orig_len;
static uint8_t mon_buf[100];
static Gen3Pkm scratch;

/* No Pokemon is opened or put while a write is pending, through any of the four ways: the working
 * copy and the edits counted stay as they were */
static void puts_refused(void) {
    uint32_t size;
    const uint8_t* w = file(GEN3_CART_WORK_NAME, &size);
    ASSERT_TRUE(w != NULL && size == WORK_LEN);
    static uint8_t work[WORK_LEN];
    memcpy(work, w, WORK_LEN);
    const uint16_t changes = gen3_cart_changes(&cart);
    const uint16_t dirty = cart.dirty;
    /* a Pokemon (party 1), an empty box slot, the party slot after the last */
    ASSERT_EQ(
        gen3_cart_mon_open(&cart, &ed, GEN3_SAVE_PARTY, 1, orig, &orig_len, &scratch),
        Gen3CartMonPending);
    ASSERT_EQ(gen3_cart_mon_open(&cart, &ed, 2, 0, orig, &orig_len, &scratch), Gen3CartMonPending);
    ASSERT_EQ(
        gen3_cart_mon_open(&cart, &ed, GEN3_SAVE_PARTY, 3, orig, &orig_len, &scratch),
        Gen3CartMonPending);
    Gen3Pkm p;
    ASSERT_TRUE(treecko(70, &p));
    ASSERT_EQ(gen3_cart_mon_place(&cart, &ed, 2, 0, true, mon_buf), Gen3CartMonPending);
    ASSERT_EQ(
        gen3_cart_mon_place(&cart, &ed, GEN3_SAVE_PARTY, 3, false, mon_buf), Gen3CartMonPending);
    /* the reason is the write, before whether the slot is free */
    ASSERT_EQ(
        gen3_cart_mon_place(&cart, &ed, GEN3_SAVE_PARTY, 0, false, mon_buf), Gen3CartMonPending);
    /* one opened before at level 60 (orig its bytes), at level 70 in the editor now */
    Gen3Pkm o;
    ASSERT_TRUE(treecko(60, &o));
    gen3_save_pkm_to_mon(&o, 80, orig);
    ASSERT_TRUE(treecko(70, &p));
    bool put = true;
    ASSERT_EQ(
        gen3_cart_mon_close(&cart, &ed, 1, 19, orig, 80, mon_buf, &scratch, &put),
        Gen3CartMonPending);
    ASSERT_FALSE(put);
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartRefused);
    ASSERT_EQ(gen3_cart_mon_put(&cart, GEN3_SAVE_PARTY, 0, &p), Gen3CartRefused);
    ASSERT_EQ(gen3_cart_changes(&cart), changes);
    ASSERT_EQ(cart.dirty, dirty);
    ASSERT_TRUE(gen3_cart_work_ok(&cart));
    w = file(GEN3_CART_WORK_NAME, &size);
    ASSERT_TRUE(w != NULL && size == WORK_LEN);
    ASSERT_MEM_EQ(w, work, WORK_LEN);
}

/* A write cut short before its last commit (the cable out right after the 7th) is still pending
 * after the reconnect, which writes nothing: the backup and the edit stay, no Pokemon is opened or
 * put, and the next write redoes it. A cut before the arming leaves nothing pending. */
TEST(test_resolve_pending_kept) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 7;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    puts_refused();
    fake_helper_replug(&fh);
    const FakeGbaCounts n = counts();
    const uint32_t crcs = calls(GEN3_HCMD_SECTOR_CRC32);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedNone);
    ASSERT_EQ(calls(GEN3_HCMD_SECTOR_CRC32), crcs);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    ASSERT_TRUE(gen3_cart_linked(&cart));
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), BACKUP_NAME);
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    puts_refused();
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(counts().erases, 28);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_TRUE(written_and_backed_up());
    ASSERT_FALSE(gen3_cart_pending(&cart));
    /* nothing pending: a Pokemon is put again */
    Gen3Pkm p;
    ASSERT_TRUE(treecko(70, &p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    /* the cable out in the footer check, before anything is armed: nothing pending */
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    hook.unplug_code = GEN3_HCMD_READ_FOOTERS;
    hook.unplug_nth = 1;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_FALSE(gen3_cart_pending(&cart));
    ASSERT_TRUE(treecko(70, &p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
}

/* A write cut short in its 8th commit (the link), then a whole save of the game's in that slot
 * with the write's counter: the game loads it, but it isn't the write's. The reconnect writes
 * nothing, and drops the backup and the edits: Changed, with nothing pending. */
TEST(test_resolve_game_saved_reconnect) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 8;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    game_saved_into_b();
    memcpy(before, flash(), SAVE_LEN);
    fake_helper_replug(&fh);
    const FakeGbaCounts n = counts();
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartChanged);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedNone);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_MEM_EQ(flash(), before, SAVE_LEN);
    ASSERT_FALSE(gen3_cart_pending(&cart));
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(cart.dirty, 0);
    ASSERT_FALSE(gen3_cart_work_ok(&cart));
    ASSERT_TRUE(gen3_cart_linked(&cart));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP4), Gen3CartRefused);
    /* the hub's Back up again: the game's save */
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP3), Gen3CartOk);
    ASSERT_EQ(gen3_cart_slots(&cart)->active, 1);
    ASSERT_EQ(gen3_cart_slots(&cart)->save_counter, 5);
}

/* The sequence that once dropped everything: a write cut after its last commit (the link lost in
 * the new backup), a reconnect, a new edit, a write. The reconnect finishes the first write, so
 * the new edit goes onto the new save; the write after it needs a reconnect, as after every write
 * that ended Ok, and then writes it. Both edits end on the cart. */
TEST(test_resolve_cut_after_commits_then_edit) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    prog.unplug_step = Gen3CartStepPostBackup;
    prog.unplug_at = 40;
    prog.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedWrite);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    Gen3Pkm p;
    ASSERT_TRUE(treecko(60, &p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP3), Gen3CartLinkLost);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedNone);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP3), Gen3CartOk);
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    /* slot A, counter 6: the first edit (party 1 at level 50) and the second (box 1 slot 19) */
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    ASSERT_EQ(s->active, 0);
    ASSERT_EQ(s->save_counter, 6);
    uint8_t mon[100];
    Gen3Pkm back;
    ASSERT_TRUE(flash_mon(s->phys, GEN3_SAVE_PARTY, 0, mon));
    ASSERT_TRUE(gen3_save_mon_to_pkm(mon, 100, &back));
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldLevel), 50);
    ASSERT_TRUE(flash_mon(s->phys, 1, 19, mon));
    ASSERT_TRUE(gen3_save_mon_to_pkm(mon, 80, &back));
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldLevel), 60);
}

static bool put_other(void);
static FakeSaveSpec other_spec(void);

/* A write cut right after its 14th commit, whose settling the reconnect couldn't finish (the
 * helper answered its first SECTOR_CRC32 with an error, the link still up); then a restore whose
 * file reads otherwise after its check, which stops in its step 3 with three of its own image
 * CRCs taken. The next reconnect checks the write against the CRCs it armed with, and finishes it;
 * it isn't Changed. */
TEST(test_resolve_uses_own_crcs) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 14;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(game_view(), ViewNew);
    fake_helper_replug(&fh);
    tap.over_code = GEN3_HCMD_SECTOR_CRC32;
    tap.over_seen = 0;
    tap.over_nth = 1;
    tap.over_status = GEN3_HST_BAD_ARG;
    tap.over_n = 2;
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartHelperError);
    ASSERT_TRUE(gen3_cart_linked(&cart));
    ASSERT_TRUE(gen3_cart_pending(&cart));
    ASSERT_TRUE(put_other());
    prog.corrupt_step = Gen3CartStepRestoreCheck;
    prog.corrupt_at = 28;
    prog.corrupt_name = OTHER_NAME;
    const FakeSaveSpec o = other_spec();
    prog.corrupt_off = fake_save_phys(&o, 1, 3) * SECTOR_LEN + 0x40u;
    const uint32_t arms = calls(GEN3_HCMD_ARM_WRITE);
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP3), Gen3CartBadBackup);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), arms);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    const FakeGbaCounts n = counts();
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedWrite);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_TRUE(written_and_backed_up());
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), WRITE_NAME);
    ASSERT_EQ(watch.violations, 0);
}

/* A write cut right after its 14th commit, whose settling the reconnect couldn't finish (the
 * helper answered its first SECTOR_CRC32 with an error, the link still up); then a restore of
 * another file, which finds the slot written (the game loads it, with the counter both share) and
 * loses the link in its check of that slot, at the first SECTOR_CRC32. It never found its own
 * images there, so the write's record stays: the next reconnect checks the write's own images,
 * finds them, and finishes it. Not Changed: the edit is on the cart and in the new backup. */
TEST(test_resolve_after_restore_check_lost) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 14;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(game_view(), ViewNew);
    fake_helper_replug(&fh);
    tap.over_code = GEN3_HCMD_SECTOR_CRC32;
    tap.over_seen = 0;
    tap.over_nth = 1;
    tap.over_status = GEN3_HST_BAD_ARG;
    tap.over_n = 2;
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartHelperError);
    ASSERT_TRUE(gen3_cart_linked(&cart));
    ASSERT_TRUE(gen3_cart_pending(&cart));
    ASSERT_TRUE(put_other());
    hook.unplug_code = GEN3_HCMD_SECTOR_CRC32;
    hook.unplug_nth = calls(GEN3_HCMD_SECTOR_CRC32) + 1u;
    hook.unplug_len = FOREVER;
    const uint32_t arms = calls(GEN3_HCMD_ARM_WRITE);
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP3), Gen3CartLinkLost);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), arms);
    ASSERT_EQ(hook.unplug_code, 0);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    fake_helper_replug(&fh);
    const FakeGbaCounts n = counts();
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedWrite);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_TRUE(written_and_backed_up());
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), WRITE_NAME);
    ASSERT_EQ(watch.violations, 0);
    /* the edit, party 1 at level 50, in slot B with counter 5 */
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    ASSERT_EQ(s->active, 1);
    ASSERT_EQ(s->save_counter, 5);
    uint8_t mon[100];
    Gen3Pkm back;
    ASSERT_TRUE(flash_mon(s->phys, GEN3_SAVE_PARTY, 0, mon));
    ASSERT_TRUE(gen3_save_mon_to_pkm(mon, 100, &back));
    ASSERT_EQ(gen3_pkm_get(&back, Gen3FieldLevel), 50);
}

/* A write or a restore that ends Ok leaves the cart unlinked: its result says to power-cycle the
 * GBA, which ends the helper, so a job needing it answers LinkLost without a word on the link,
 * until a reconnect. One that ends otherwise (WrittenNoBackup, here) stays linked, to back up. */
TEST(test_write_ok_unlinks) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    ASSERT_TRUE(gen3_cart_linked(&cart));
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_FALSE(gen3_cart_linked(&cart));
    const uint32_t exchanges = tap.exchanges;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_restore(&cart, BACKUP_NAME, STAMP3), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP3), Gen3CartLinkLost);
    ASSERT_EQ(tap.exchanges, exchanges);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_linked(&cart));
    ASSERT_EQ(gen3_cart_restore(&cart, BACKUP_NAME, STAMP3), Gen3CartOk);
    ASSERT_FALSE(gen3_cart_linked(&cart));
    /* WrittenNoBackup */
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    fake_store_corrupt_read(&store, GEN3_CART_BACKUP_TMP, 20u * SECTOR_LEN + 5u);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartWrittenNoBackup);
    ASSERT_TRUE(gen3_cart_linked(&cart));
}

/* The hub's items as the hub scene reads them from the cart */
static Gen3CartScreenHub hub_now(void) {
    Gen3CartScreenHubIn in;
    Gen3CartScreenHub h;
    gen3_cart_screen_hub_in(&cart, &in);
    gen3_cart_screen_hub(&in, &h);
    return h;
}

/* After "Power-cycle the GBA to play" (a write or a restore that ended Ok), the hub offers
 * Reconnect, not Restore backup or Copy ROM, which would fail on the helper the power cycle ends;
 * nor Write changes for an edit made then, which waits for the reconnect too, while Discard
 * changes doesn't. After the reconnect, all of them again. While a job runs (no cart), no item. */
TEST(test_hub_after_write) {
    ASSERT_TRUE(backed_up());
    Gen3CartScreenHub h = hub_now();
    ASSERT_FALSE(h.reconnect);
    ASSERT_TRUE(h.restore);
    ASSERT_TRUE(h.rom);
    ASSERT_TRUE(edit_party());
    h = hub_now();
    ASSERT_TRUE(h.write);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    h = hub_now();
    ASSERT_TRUE(h.reconnect);
    ASSERT_FALSE(h.restore);
    ASSERT_FALSE(h.rom);
    ASSERT_TRUE(h.browse);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.backup);
    Gen3Pkm p;
    ASSERT_TRUE(treecko(60, &p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
    h = hub_now();
    ASSERT_TRUE(h.reconnect);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.writing_off);
    ASSERT_TRUE(h.discard);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    h = hub_now();
    ASSERT_FALSE(h.reconnect);
    ASSERT_TRUE(h.write);
    ASSERT_TRUE(h.discard);
    ASSERT_TRUE(h.restore);
    ASSERT_TRUE(h.rom);
    ASSERT_EQ(gen3_cart_restore(&cart, BACKUP_NAME, STAMP3), Gen3CartOk);
    h = hub_now();
    ASSERT_TRUE(h.reconnect);
    ASSERT_FALSE(h.restore);
    ASSERT_FALSE(h.rom);
    Gen3CartScreenHubIn in;
    gen3_cart_screen_hub_in(NULL, &in);
    ASSERT_TRUE(in.busy);
    gen3_cart_screen_hub(&in, &h);
    ASSERT_FALSE(h.reconnect || h.restore || h.rom || h.browse || h.write);
}

/* A restore cut short (the cable out right after its 7th commit), with no edits: pending, so no
 * Pokemon is opened or put. The hub offers Discard changes, with Reconnect while unlinked, and
 * once reconnected (still pending: the game loads the old save) with Restore backup, but never
 * Write changes; the text under "Can't edit" names only those. A discard ends it: slot B holds 7
 * sectors the backup doesn't, so the backup goes too (Changed), and the hub offers a new one. */
TEST(test_hub_pending_restore) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(put_other());
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 7;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP2), Gen3CartLinkLost);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(
        gen3_cart_mon_open(&cart, &ed, GEN3_SAVE_PARTY, 1, orig, &orig_len, &scratch),
        Gen3CartMonPending);
    Gen3CartScreenHub h = hub_now();
    ASSERT_TRUE(h.browse);
    ASSERT_TRUE(h.reconnect);
    ASSERT_TRUE(h.discard);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.restore);
    ASSERT_STR_EQ(
        gen3_cart_screen_mon_text(Gen3CartMonPending, &h),
        "Finish the write\nfirst: Reconnect or\nDiscard changes.");
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedNone);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    h = hub_now();
    ASSERT_TRUE(h.browse);
    ASSERT_FALSE(h.reconnect);
    ASSERT_TRUE(h.discard);
    ASSERT_FALSE(h.write);
    ASSERT_TRUE(h.restore);
    ASSERT_STR_EQ(
        gen3_cart_screen_mon_text(Gen3CartMonPending, &h),
        "Finish the restore\nfirst: Restore backup\nor Discard changes.");
    const FakeGbaCounts n = counts();
    ASSERT_EQ(gen3_cart_discard(&cart), Gen3CartChanged);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_FALSE(gen3_cart_pending(&cart));
    h = hub_now();
    ASSERT_FALSE(h.discard);
    ASSERT_TRUE(h.backup);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP3), Gen3CartOk);
    ASSERT_FALSE(gen3_cart_pending(&cart));
    Gen3Pkm p;
    ASSERT_TRUE(treecko(70, &p));
    ASSERT_EQ(gen3_cart_mon_put(&cart, 1, 19, &p), Gen3CartOk);
}

/* Erase 0 of 14 is reported before the first erase runs, so the screen says not to power off for
 * that one as well */
TEST(test_write_reports_erase_first) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(hook.erase_reports, 1);
    ASSERT_EQ(hook.erase_first_done, 0);
    ASSERT_EQ(prog.calls[Gen3CartStepErase], 15);
    ASSERT_EQ(prog.last_done[Gen3CartStepErase], 14);
}

/* A write cut in its 8th commit, then the GBA off and another cart in: the same game and header,
 * its loaded slot A byte for byte the first cart's, but an older save of its own in slot B. Attach
 * sees in slot B what neither the backup, the write nor a game save after it put there: a new
 * backup first, which holds that older save, and only then a write over it. */
TEST(test_attach_other_cart_same_save) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    fake_gba_power_cut_after(&gba, CUT_IN_COMMIT8);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(cart.wslot, 1);
    FakeSaveSpec s = spec;
    s.slot_present[1] = true;
    s.counter[1] = 3;
    s.first[1] = 5;
    fake_save_build(img2, &s);
    memcpy(flash() + 14 * SECTOR_LEN, img2 + 14 * SECTOR_LEN, 14 * SECTOR_LEN);
    fake_helper_restart(&fh);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    const FakeGbaCounts n = counts();
    bool dropped = false;
    ASSERT_EQ(gen3_cart_attach(&cart, STAMP3, &dropped), Gen3CartOk);
    ASSERT_TRUE(dropped);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "backups/AXVE_12345_" STAMP3 ".sav");
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    ASSERT_MEM_EQ(flash() + 14 * SECTOR_LEN, img2 + 14 * SECTOR_LEN, 14 * SECTOR_LEN);
    ASSERT_EQ(cart.wslot, NONE);
    ASSERT_EQ(gen3_cart_slots(&cart)->result, Gen3SaveOk);
    /* with that backup, the write goes over the older save */
    ASSERT_TRUE(edit_party());
    ASSERT_EQ(gen3_cart_write(&cart, STAMP4), Gen3CartOk);
    uint32_t size;
    const uint8_t* sav = file("backups/AXVE_12345_" STAMP3 ".sav", &size);
    ASSERT_TRUE(sav != NULL && size == SAVE_LEN);
    ASSERT_MEM_EQ(sav + 14 * SECTOR_LEN, img2 + 14 * SECTOR_LEN, 14 * SECTOR_LEN);
}

/* While a write's slot is skipped, the slot the backup's save is in is still compared: after a cut
 * in the 8th commit, a footer of slot A changed means another save. Attach backs it up, the edit
 * dropped; reconnect says Changed. */
TEST(test_kept_slot_compared) {
    for(uint32_t i = 0; i < 2; i++) {
        ASSERT_TRUE(backed_up());
        ASSERT_TRUE(edit_party());
        if(i == 0) {
            fake_gba_power_cut_after(&gba, CUT_IN_COMMIT8);
        } else {
            hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
            hook.unplug_nth = 7;
            hook.unplug_len = FOREVER;
        }
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
        ASSERT_EQ(cart.wslot, 1);
        fake_save_set_counter(flash(), phys(0, 13), 2);
        fake_helper_replug(&fh);
        if(i == 0) {
            fake_helper_restart(&fh);
            ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
            bool dropped = false;
            ASSERT_EQ(gen3_cart_attach(&cart, STAMP3, &dropped), Gen3CartOk);
            ASSERT_TRUE(dropped);
            ASSERT_EQ(gen3_cart_changes(&cart), 0);
            ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
            ASSERT_EQ(fake_store_count(&store, ".sav"), 2);
        } else {
            ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartChanged);
            ASSERT_FALSE(gen3_cart_backup_current(&cart));
            ASSERT_EQ(gen3_cart_changes(&cart), 1);
        }
        ASSERT_EQ(cart.wslot, NONE);
        ASSERT_EQ(gen3_cart_write(&cart, STAMP4), Gen3CartRefused);
    }
}

/* Every new sector checked, but a sector the write never touches (any of the old slot's, or of
 * 28..31) isn't the backup's: written and checked, but no new backup */
TEST(test_post_write_backup_cart_differs) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    snap_take();
    /* each sector the write doesn't touch: slot A (the old save, now the fallback) and 28..31 */
    for(uint8_t s = 0; s < 32; s++) {
        if(s >= 14 && s < 28) continue;
        snap_restore();
        act_sector = s;
        hook.act_code = GEN3_HCMD_COMMIT_SECTOR;
        hook.act_nth = 14;
        hook.act = other_sector_differs;
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartWrittenNoBackup);
        ASSERT_EQ(watch.violations, 0);
        ASSERT_EQ(game_view(), ViewNew);
        ASSERT_FALSE(gen3_cart_backup_current(&cart));
        ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
        ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
        ASSERT_EQ(gen3_cart_changes(&cart), 0);
        if(s == 0 || s == 31) {
            ASSERT_EQ(gen3_cart_backup(&cart, STAMP3), Gen3CartOk);
            ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
        }
    }
}

/* The link lost while the new backup is checked on the cart: written, not yet backed up. The
 * reconnect finds it written and makes the backup. */
TEST(test_write_link_lost_in_backup) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    prog.unplug_step = Gen3CartStepPostBackup;
    prog.unplug_at = 40;
    prog.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(game_view(), ViewNew);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), BACKUP_NAME);
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    fake_helper_replug(&fh);
    const uint32_t erases = counts().erases;
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedWrite);
    ASSERT_EQ(counts().erases, erases);
    ASSERT_TRUE(written_and_backed_up());
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), WRITE_NAME);
    ASSERT_EQ(watch.violations, 0);
}

/* The reference write, from the snapshot: its points, transfers and flash ops */
static bool reference_run(uint32_t* xfers, uint32_t* ops) {
    snap_restore();
    ref.on = true;
    ref.start_xfer = fake_helper_transfers(&fh);
    ref.start_ops = flash_ops();
    const Gen3CartResult r = gen3_cart_write(&cart, STAMP2);
    ref.on = false;
    *xfers = fake_helper_transfers(&fh) - ref.start_xfer;
    *ops = flash_ops() - ref.start_ops;
    /* 14 erases, the last program, 14 commits */
    return r == Gen3CartOk && ref.n == 29 && watch.violations == 0;
}

/* Every point of a sweep: start, start + step, ... under end, then p - 1, p, p + 1 for each of the
 * reference run's points */
static uint32_t sweep_points(
    uint32_t step,
    uint32_t start,
    uint32_t end,
    const uint32_t* at,
    uint32_t n,
    uint32_t* out,
    uint32_t max) {
    uint32_t count = 0;
    for(uint32_t d = start; d < end && count < max; d += step) out[count++] = d;
    for(uint32_t i = 0; i < n; i++) {
        for(uint32_t j = 0; j < 3 && count < max; j++) {
            if(at[i] + j >= 1u) out[count++] = at[i] + j - 1u;
        }
    }
    return count;
}

/* The cable out for good at d transfers into the write, for every d: LinkLost or Ok, and the game
 * loads the old save or the new one at every step, both oracles naming the same slot at the cut.
 * Then the cable back and a reconnect: a cut once the game loads the new save (past the 14th
 * commit) is found done and finished by it; one before stays pending, and the write again
 * finishes it. On the Normal wire (the GBC cable pulled, or the GBA switched off) every slot with
 * the cable out is not ready, which the pump takes as BUSY, so a request can also end at its BUSY
 * budget: BusyTimeout, lost too. */
static void link_drop_sweep(MlinkWire wire) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    fake_helper_set_wire(&fh, wire);
    snap_take();
    uint32_t xfers = 0, ops = 0;
    ASSERT_TRUE(reference_run(&xfers, &ops));
    static uint32_t points[400];
    const uint32_t n = sweep_points(400, 400, xfers + 1u, ref.xfer, ref.n, points, 400);
    uint32_t lost = 0, finished = 0;
    for(uint32_t i = 0; i < n; i++) {
        snap_restore();
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + points[i], FOREVER);
        const Gen3CartResult r = gen3_cart_write(&cart, STAMP2);
        const bool busy_lost = wire == MlinkWireNormal && r == Gen3CartBusyTimeout;
        if(r != Gen3CartLinkLost && r != Gen3CartOk && !busy_lost) {
            printf("  d=%u: write %d\n", (unsigned)points[i], r);
            ASSERT_TRUE(false);
        }
        ASSERT_EQ(watch.violations, 0);
        ASSERT_TRUE(oracles_agree());
        const View cut = game_view();
        fake_helper_replug(&fh);
        if(r == Gen3CartOk) {
            ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartNothing);
        } else {
            lost++;
            ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
            if(cut == ViewNew) {
                finished++;
                ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedWrite);
                ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartNothing);
            } else {
                /* pending once it armed; before, nothing is */
                ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedNone);
                const Gen3CartResult again = gen3_cart_write(&cart, STAMP2);
                if(again != Gen3CartOk) {
                    printf("  d=%u: write again %d\n", (unsigned)points[i], again);
                }
                ASSERT_EQ(again, Gen3CartOk);
            }
        }
        ASSERT_EQ(watch.violations, 0);
        ASSERT_TRUE(written_and_backed_up());
    }
    /* every point is inside the write: each was lost, then finished, those past the 14th commit
     * (the check and the new backup) by the reconnect */
    ASSERT_TRUE(n >= 100u);
    ASSERT_EQ(lost, n);
    ASSERT_TRUE(finished >= 3u && finished < n);
}

TEST(test_write_link_drop_sweep) {
    link_drop_sweep(MlinkWireMulti);
}

TEST(test_write_link_drop_sweep_normal) {
    link_drop_sweep(MlinkWireNormal);
}

/* The next session after a power cut that left the game loading `cut`: the GBA on again, the
 * helper sent, a whole connect and attach. The attach finds a write past its last commit done and
 * finishes it, writing nothing; it keeps one cut before that pending, with its edit, and the write
 * again finishes it. Either way the game then loads the new save, backed up, by both oracles, with
 * nothing half erased under it, and after one more power cycle too. */
static void next_session(View cut, uint32_t k) {
    if(cut == ViewNew) {
        back_on_finished();
    } else {
        back_on_pending();
        ASSERT_EQ(watch_view(), ViewOld);
        const Gen3CartResult again = gen3_cart_write(&cart, STAMP4);
        if(again != Gen3CartOk) printf("  k=%u: write again %d\n", (unsigned)k, again);
        ASSERT_EQ(again, Gen3CartOk);
        ASSERT_TRUE(written_and_backed_up());
    }
    ASSERT_EQ(watch.violations, 0);
    ASSERT_TRUE(oracles_agree());
    ASSERT_FALSE(gba.weak_erase);
    fake_gba_power_cycle(&gba);
    ASSERT_EQ(game_view(), ViewNew);
}

/* The GBA's power cut after k flash ops of the write, for every k: LinkLost, and the game loads the
 * old save or the new one, at every step and after, both oracles naming the same slot at the cut.
 * Slot B holds an older save, so a cut erase or program leaves something half done. At each of the
 * reference run's points the op the cut stops in, an erase, the last program or a commit byte, is
 * also left half done: a sector reading 0xFF but weakly erased, or a byte with only some of its
 * bits programmed. After every cut the GBA is switched on again and the helper sent
 * (next_session): a write cut before its last commit stays pending, with its edit, and finishes on
 * its next run, its erase making a weak sector whole. Past the last flash op, in the check and the
 * new backup, a cut leaves the new save, which the next session's attach finds done: it backs it
 * up and erases nothing. */
TEST(test_write_power_cut_sweep) {
    ASSERT_TRUE(save_with_older_b());
    insert("AXVE", FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(edit_party());
    watch_write();
    snap_take();
    uint32_t xfers = 0, ops = 0;
    ASSERT_TRUE(reference_run(&xfers, &ops));
    /* the runs below start each from the snapshot, which clears ref */
    static uint32_t at[POINTS];
    const uint32_t ats = ref.n;
    memcpy(at, ref.ops, sizeof(at));
    const uint32_t after_commits = ref.xfer[ats - 1u];
    static uint32_t points[200];
    const uint32_t grid = sweep_points(1500, 0, ops, NULL, 0, points, 200);
    const uint32_t n = grid + sweep_points(1, 0, 0, at, ats, points + grid, 200 - grid);
    /* 14 erases, 14 sectors of 4095 bytes programmed (the commit byte left), 14 commit bytes */
    ASSERT_EQ(ops, 14u + 14u * 4095u + 14u);
    uint32_t runs = 0;
    for(uint32_t i = 0; i < n; i++) {
        if(points[i] >= ops) continue;
        snap_restore();
        fake_gba_power_cut_after(&gba, points[i]);
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
        ASSERT_TRUE(gba.frozen);
        ASSERT_EQ(watch.violations, 0);
        const View cut = watch_view();
        ASSERT_TRUE(cut == ViewOld || cut == ViewNew);
        ASSERT_TRUE(oracles_agree());
        next_session(cut, points[i]);
        runs++;
    }
    uint32_t halves = 0;
    for(uint32_t p = 0; p < ats; p++) {
        const uint32_t k = at[p] - 1u;
        snap_restore();
        fake_gba_power_cut_partial(&gba, k);
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
        ASSERT_TRUE(gba.frozen);
        ASSERT_EQ(watch.violations, 0);
        const View cut = watch_view();
        ASSERT_TRUE(cut == ViewOld || cut == ViewNew);
        ASSERT_TRUE(oracles_agree());
        if(p < GEN3_SAVE_SLOT_SECTORS) ASSERT_TRUE(gba.weak_erase);
        next_session(cut, k);
        halves++;
    }
    /* past the last flash op, in the check and the new backup: a cut every 100 transfers from the
     * 14th commit, and at the write's last transfer. With no flash op left, the GBA off is the
     * helper hearing nothing more until it is sent again. The last transfers may follow the last
     * response the write needs: then it is done, Ok. */
    static uint32_t late[64];
    uint32_t lates = 0;
    uint32_t late_lost = 0;
    for(uint32_t d = after_commits; d < xfers && lates < 63u; d += 100u) late[lates++] = d;
    late[lates++] = xfers - 1u;
    for(uint32_t i = 0; i < lates; i++) {
        snap_restore();
        fake_helper_unplug_at(&fh, fake_helper_transfers(&fh) + late[i], FOREVER);
        const Gen3CartResult r = gen3_cart_write(&cart, STAMP2);
        ASSERT_EQ(watch.violations, 0);
        ASSERT_EQ(watch_view(), ViewNew);
        ASSERT_TRUE(oracles_agree());
        if(r == Gen3CartOk) {
            ASSERT_TRUE(written_and_backed_up());
            continue;
        }
        ASSERT_EQ(r, Gen3CartLinkLost);
        late_lost++;
        next_session(ViewNew, late[i]);
    }
    ASSERT_TRUE(runs >= 100u);
    ASSERT_EQ(halves, 29);
    ASSERT_TRUE(lates >= 10u);
    ASSERT_TRUE(late_lost + 1u >= lates);
}

/* ---- The new backup ---- */

/* The card holds the old backup and the working copy but not a new backup: written and checked,
 * not backed up; no write until a backup */
TEST(test_post_write_backup_sd_full) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    store.capacity = SAVE_LEN + WORK_LEN + 100000u;
    fake_store_report_free(&store, 64u * MIB);
    const uint32_t writes = store.writes;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartWrittenNoBackup);
    /* the working copy's 14 sectors, and nothing of a backup that couldn't fit */
    ASSERT_EQ(store.writes - writes, 14);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "");
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(cart.dirty, 0);
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    /* the working copy is the new slot */
    uint8_t b[14];
    slot_at(1, 1, b);
    ASSERT_TRUE(work_is_flash(b));
    /* nothing to browse, so nothing to edit, until a backup */
    ASSERT_FALSE(edit_party());
    ASSERT_EQ(gen3_cart_write(&cart, STAMP3), Gen3CartRefused);
    /* with room again, a backup of the new save */
    store.capacity = 64u * MIB;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP3), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
}

/* The new backup comes back from the card otherwise than it was written */
TEST(test_post_write_backup_read_back) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    watch_write();
    fake_store_corrupt_read(&store, GEN3_CART_BACKUP_TMP, 20u * SECTOR_LEN + 5u);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartWrittenNoBackup);
    ASSERT_EQ(game_view(), ViewNew);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 0);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 1);
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    fake_store_corrupt_read(&store, NULL, 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP3), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
}

/* ---- Reconnect ---- */

/* Lost in a ROM copy, then the same cart and save: the backup and the edits stay */
TEST(test_reconnect_same) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(edit_party());
    prog.unplug_step = Gen3CartStepRomRead;
    prog.unplug_at = 8192;
    prog.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 1);
    ASSERT_EQ(calls(GEN3_HCMD_READ_SECTOR), 32);
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    /* with no backup there is nothing to compare */
    ASSERT_TRUE(backed_up());
    fake_store_fail_write_after(&store, 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP2), Gen3CartSdError);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 0);
}

/* After a lost link, a footer of the save kept, or the cart, isn't the backup's: Changed, the
 * backup dropped, the edits counted until a new one */
TEST(test_reconnect_changed) {
    for(uint32_t i = 0; i < 3; i++) {
        ASSERT_TRUE(backed_up());
        ASSERT_TRUE(edit_party());
        prog.unplug_step = Gen3CartStepRomRead;
        prog.unplug_at = 8192;
        prog.unplug_len = FOREVER;
        ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
        if(i == 0) {
            fake_save_set_counter(flash(), phys(0, 13), 2);
        } else if(i == 1) {
            /* a sector of slot B that no write armed: every footer counts */
            flash()[20u * SECTOR_LEN + 0xFF4] = 0x00;
        } else {
            helper_cmd_header(hdr, gba.own, "POKEMON RUBY", "AXVE", 1);
            fake_gba_rom(&gba, hdr, MIB);
            fake_gba_rom_string(&gba, LIB_OFF, LIB_STRING);
        }
        fake_helper_replug(&fh);
        ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartChanged);
        ASSERT_FALSE(gen3_cart_backup_current(&cart));
        ASSERT_EQ(gen3_cart_changes(&cart), 1);
        ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartRefused);
        bool dropped = false;
        ASSERT_EQ(gen3_cart_attach(&cart, STAMP2, &dropped), Gen3CartOk);
        ASSERT_TRUE(dropped);
        ASSERT_EQ(gen3_cart_changes(&cart), 0);
        ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
        ASSERT_TRUE(no_flash_ops());
    }
}

/* A write lost right after its first erase, slot B holding an older save: the reconnect relinks
 * after the erase's budget, and doesn't compare slot B, which the write armed and changed; the
 * write then starts again */
TEST(test_reconnect_after_erase) {
    ASSERT_TRUE(save_with_older_b());
    insert("AXVE", FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_EQ(gen3_cart_slots(&cart)->result, Gen3SaveOk);
    ASSERT_EQ(gen3_cart_slots(&cart)->active, 0);
    ASSERT_TRUE(edit_party());
    watch_write();
    hook.unplug_code = GEN3_HCMD_ERASE_SECTOR;
    hook.unplug_nth = 1;
    hook.unplug_len = 600;
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(cart.relink_us, GEN3_HCLI_BUSY_FLASH_US + GEN3_HCLI_SYNC_US);
    ASSERT_EQ(cart.wslot, 1);
    ASSERT_EQ(watch.violations, 0);
    /* slot B's first sector is erased now */
    uint8_t b[14];
    slot_at(1, 1, b);
    ASSERT_EQ(flash()[b[0] * SECTOR_LEN + 0xFF8], 0xFF);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
    ASSERT_EQ(gen3_cart_changes(&cart), 1);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 2);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
    /* with no write armed, a change in the slot the game doesn't load is another save */
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_TRUE(edit_party());
    prog.unplug_step = Gen3CartStepRomRead;
    prog.unplug_at = 8192;
    prog.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_rom_dump(&cart, false), Gen3CartLinkLost);
    ASSERT_EQ(cart.wslot, NONE);
    flash()[0xFF8] = 0xFF;
    fake_helper_replug(&fh);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartChanged);
}

/* ---- Restore ---- */

/* Backup X, then a write (Y), then X restored: X's data in slot A with counter 6, Y kept in B */
TEST(test_restore_ok) {
    ASSERT_TRUE(backed_up());
    static uint8_t x[SAVE_LEN];
    uint32_t size;
    memcpy(x, file(BACKUP_NAME, &size), SAVE_LEN);
    ASSERT_TRUE(edit_party());
    ASSERT_EQ(gen3_cart_write(&cart, STAMP2), Gen3CartOk);
    ASSERT_TRUE(gen3_cart_restorable(&cart));
    Gen3SaveTrainer t;
    bool has = false;
    ASSERT_EQ(gen3_cart_restore_check(&cart, BACKUP_NAME, &t, &has), Gen3CartOk);
    ASSERT_TRUE(has);
    /* the write left the cart unlinked: the GBA is to be power-cycled to play */
    ASSERT_EQ(gen3_cart_restore(&cart, BACKUP_NAME, STAMP3), Gen3CartLinkLost);
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    watch_write();
    watch_new_from(x, &spec, 0, 6);
    memset(&hook, 0, sizeof(hook));
    ASSERT_EQ(gen3_cart_restore(&cart, BACKUP_NAME, STAMP3), Gen3CartOk);
    ASSERT_FALSE(gen3_cart_linked(&cart));
    ASSERT_EQ(watch.violations, 0);
    ASSERT_TRUE(watch.old_seen > 0);
    ASSERT_EQ(game_view(), ViewNew);
    /* every sector uploaded */
    ASSERT_EQ(calls(GEN3_HCMD_WRITE_SECTOR), 14);
    ASSERT_EQ(calls(GEN3_HCMD_COPY_SECTOR), 0);
    uint8_t a[14];
    slot_at(0, 2, a);
    for(uint8_t k = 0; k < 14; k++) ASSERT_TRUE(sector_is(a[k], k, 6));
    const Gen3SaveSlots* s = gen3_cart_slots(&cart);
    ASSERT_EQ(s->active, 0);
    ASSERT_EQ(s->save_counter, 6);
    /* the file as it was; a third .sav, the cart now */
    const uint8_t* xf = file(BACKUP_NAME, &size);
    ASSERT_TRUE(xf != NULL && size == SAVE_LEN);
    ASSERT_MEM_EQ(xf, x, SAVE_LEN);
    ASSERT_EQ(fake_store_count(&store, ".sav"), 3);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    ASSERT_TRUE(work_is_flash(a));
    for(uint8_t k = 0; k < 14; k++) {
        ASSERT_MEM_EQ(flash() + a[k] * SECTOR_LEN, x + phys(0, k) * SECTOR_LEN, DATA_LEN);
    }
    /* the party as backed up */
    uint8_t mon[100];
    size_t len = 0;
    ASSERT_EQ(gen3_cart_mon_get(&cart, GEN3_SAVE_PARTY, 0, mon, &len), Gen3CartOk);
    ASSERT_MEM_EQ(mon, party[0], 100);
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
}

/* A valid backup file, slot B with counter 9 and id 0 at its fifth sector, in the store */
static FakeSaveSpec other_spec(void) {
    return (FakeSaveSpec){
        .game = FakeGameRS,
        .slot_present = {false, true},
        .counter = {0, 9},
        .first = {0, 4},
        .seed = 0x0BAC0009u,
        .no_mons = true,
    };
}

static bool put_other(void) {
    const FakeSaveSpec s = other_spec();
    fake_save_build(img2, &s);
    fake_save_put_trainer(img2, &s, 1, "MAY", 1, 22222, 33333);
    return put_file(OTHER_NAME, img2, SAVE_LEN);
}

/* A cart with no save its game loads takes the backup's own counter and rotation */
static void restore_onto_no_save(void) {
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_EQ(gen3_cart_id(&cart)->why, Gen3CartRefuseSave);
    ASSERT_TRUE(gen3_cart_id(&cart)->can_write);
    ASSERT_FALSE(gen3_cart_writable(&cart, NULL));
    /* not writable, and restorable all the same: can_write and a current backup */
    ASSERT_TRUE(gen3_cart_restorable(&cart));
    ASSERT_TRUE(put_other());
    Gen3SaveTrainer t;
    bool has = false;
    ASSERT_EQ(gen3_cart_restore_check(&cart, OTHER_NAME, &t, &has), Gen3CartOk);
    ASSERT_STR_EQ(t.name, "MAY");
    watch = (Watch){.on = true, .game = Gen3LinkGameRS, .old_none = true};
    const FakeSaveSpec s = other_spec();
    watch_new_from(img2, &s, 1, 9);
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP2), Gen3CartOk);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
    const Gen3SaveSlots* v = gen3_cart_slots(&cart);
    ASSERT_EQ(v->active, 1);
    ASSERT_EQ(v->save_counter, 9);
    ASSERT_EQ(v->phys[0], 18);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    Gen3SaveTrainer now;
    ASSERT_TRUE(gen3_cart_trainer(&cart, &now));
    ASSERT_EQ(now.tid, 22222);
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    /* sectors 28..31 never written */
    ASSERT_MEM_EQ(flash() + 28 * SECTOR_LEN, img + 28 * SECTOR_LEN, 4 * SECTOR_LEN);
}

TEST(test_restore_onto_corrupt_save) {
    ASSERT_TRUE(save_with_older_b());
    fake_save_break_checksum(img, phys(0, 6));
    fake_save_break_checksum(img, phys(1, 2));
    insert("AXVE", FakeSaveFlash);
    restore_onto_no_save();
}

TEST(test_restore_onto_empty_cart) {
    memset(img, 0xFF, sizeof(img));
    insert("AXVE", FakeSaveFlash);
    restore_onto_no_save();
}

/* A save with a byte past each section of its 14: the restore clears them, so the helper keeps the
 * slot when the next write arms */
TEST(test_restore_padded_backup) {
    ASSERT_TRUE(backed_up());
    static uint8_t x[SAVE_LEN];
    uint32_t size;
    memcpy(x, file(BACKUP_NAME, &size), SAVE_LEN);
    for(uint8_t k = 0; k < 14; k++)
        x[phys(0, k) * SECTOR_LEN + fake_save_size(FakeGameRS, k)] = 0x5A;
    ASSERT_TRUE(put_file(OTHER_NAME, x, SAVE_LEN));
    Gen3SaveTrainer t;
    bool has = false;
    ASSERT_EQ(gen3_cart_restore_check(&cart, OTHER_NAME, &t, &has), Gen3CartOk);
    watch_write();
    watch_new_from(x, &spec, 0, 5);
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP2), Gen3CartOk);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_EQ(game_view(), ViewNew);
    uint8_t b[14];
    slot_at(1, 1, b);
    for(uint8_t k = 0; k < 14; k++) {
        const uint16_t from = fake_save_size(FakeGameRS, k);
        const uint8_t* d = flash() + b[k] * SECTOR_LEN;
        for(uint32_t i = from; i < 0xFF4u; i++) ASSERT_EQ(d[i], 0);
        ASSERT_TRUE(sector_is(b[k], k, 5));
    }
    ASSERT_FALSE(gen3_cart_slots(&cart)->padded);
    ASSERT_TRUE(gen3_cart_writable(&cart, NULL));
    /* the file as it was */
    const uint8_t* xf = file(OTHER_NAME, &size);
    ASSERT_MEM_EQ(xf, x, SAVE_LEN);
    ASSERT_TRUE(edit_party());
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_write(&cart, STAMP3), Gen3CartOk);
    ASSERT_EQ(gen3_cart_slots(&cart)->active, 0);
}

TEST(test_restore_wrong_game_backup) {
    ASSERT_TRUE(backed_up());
    FakeSaveSpec s = other_spec();
    s.game = FakeGameFRLG;
    fake_save_build(img2, &s);
    ASSERT_TRUE(put_file(OTHER_NAME, img2, SAVE_LEN));
    Gen3SaveTrainer t;
    bool has = true;
    ASSERT_EQ(gen3_cart_restore_check(&cart, OTHER_NAME, &t, &has), Gen3CartBadBackup);
    ASSERT_FALSE(has);
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP2), Gen3CartRefused);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 0);
    ASSERT_TRUE(no_flash_ops());
}

TEST(test_restore_bad_size) {
    ASSERT_TRUE(backed_up());
    uint32_t size;
    ASSERT_TRUE(put_file(OTHER_NAME, file(BACKUP_NAME, &size), 65536u));
    Gen3SaveTrainer t;
    bool has = true;
    ASSERT_EQ(gen3_cart_restore_check(&cart, OTHER_NAME, &t, &has), Gen3CartBadBackup);
    ASSERT_FALSE(has);
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP2), Gen3CartRefused);
    /* no such file; the temporary backup, which the restore's own backup would overwrite; a name
     * too long */
    ASSERT_EQ(gen3_cart_restore_check(&cart, "backups/none.sav", &t, &has), Gen3CartSdError);
    ASSERT_EQ(gen3_cart_restore(&cart, "backups/none.sav", STAMP2), Gen3CartSdError);
    ASSERT_TRUE(put_file(GEN3_CART_BACKUP_TMP, file(BACKUP_NAME, &size), SAVE_LEN));
    ASSERT_EQ(gen3_cart_restore_check(&cart, GEN3_CART_BACKUP_TMP, &t, &has), Gen3CartBadArg);
    ASSERT_EQ(gen3_cart_restore(&cart, GEN3_CART_BACKUP_TMP, STAMP2), Gen3CartRefused);
    ASSERT_EQ(
        gen3_cart_restore_check(
            &cart, "backups/AXVE_12345_20261025-143005-0123456789-abcdefgh.sav", &t, &has),
        Gen3CartBadArg);
    ASSERT_TRUE(no_flash_ops());
}

TEST(test_restore_check_trainer) {
    ASSERT_TRUE(backed_up());
    Gen3SaveTrainer t;
    bool has = false;
    ASSERT_EQ(gen3_cart_restore_check(&cart, BACKUP_NAME, &t, &has), Gen3CartOk);
    ASSERT_TRUE(has);
    ASSERT_STR_EQ(t.name, "BRENDAN");
    ASSERT_EQ(t.tid, 12345);
    ASSERT_EQ(t.sid, 54321);
    ASSERT_EQ(prog.last_done[Gen3CartStepRestoreCheck], 28);
}

/* The file gives a sector back otherwise once the restore checked it: each image is checked again
 * as it is taken, so nothing is armed */
TEST(test_restore_file_changes) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(put_other());
    prog.corrupt_step = Gen3CartStepRestoreCheck;
    prog.corrupt_at = 28;
    prog.corrupt_name = OTHER_NAME;
    const FakeSaveSpec s = other_spec();
    prog.corrupt_off = fake_save_phys(&s, 1, 3) * SECTOR_LEN + 0x40u;
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP2), Gen3CartBadBackup);
    ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 0);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
    ASSERT_TRUE(no_flash_ops());
    ASSERT_TRUE(gen3_cart_backup_current(&cart));
}

/* A restore onto an empty cart lost right after its 14th commit: the game loads the restored save
 * now, so the reconnect finds it written, erases nothing, and makes the backup from the file,
 * named with the restore's stamp; the save's trainer is the file's */
TEST(test_restore_retry_after_commit) {
    memset(img, 0xFF, sizeof(img));
    insert("AXVE", FakeSaveFlash);
    ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
    ASSERT_TRUE(put_other());
    watch = (Watch){.on = true, .game = Gen3LinkGameRS, .old_none = true};
    const FakeSaveSpec s = other_spec();
    watch_new_from(img2, &s, 1, 9);
    hook.unplug_code = GEN3_HCMD_COMMIT_SECTOR;
    hook.unplug_nth = 14;
    hook.unplug_len = FOREVER;
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP2), Gen3CartLinkLost);
    ASSERT_EQ(game_view(), ViewNew);
    ASSERT_TRUE(gen3_cart_pending(&cart));
    fake_helper_replug(&fh);
    const FakeGbaCounts n = counts();
    ASSERT_EQ(gen3_cart_reconnect(&cart), Gen3CartOk);
    ASSERT_EQ(gen3_cart_finished(&cart), Gen3CartFinishedRestore);
    ASSERT_EQ(counts().erases, n.erases);
    ASSERT_EQ(counts().programs, n.programs);
    ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 1);
    ASSERT_EQ(watch.violations, 0);
    ASSERT_TRUE(written_and_backed_up());
    ASSERT_STR_EQ(gen3_cart_backup_name(&cart), "backups/AXVE_22222_" STAMP2 ".sav");
    ASSERT_EQ(gen3_cart_slots(&cart)->save_counter, 9);
    Gen3SaveTrainer t;
    ASSERT_TRUE(gen3_cart_trainer(&cart, &t));
    ASSERT_STR_EQ(t.name, "MAY");
    ASSERT_FALSE(gen3_cart_linked(&cart));
}

/* A restore whose new backup has no room: restored and checked; the working copy, which held the
 * save before it, isn't used again */
TEST(test_restore_no_backup) {
    ASSERT_TRUE(backed_up());
    ASSERT_TRUE(put_other());
    store.capacity = SAVE_LEN + WORK_LEN + SAVE_LEN + 100000u;
    fake_store_report_free(&store, 64u * MIB);
    ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP2), Gen3CartWrittenNoBackup);
    ASSERT_FALSE(gen3_cart_backup_current(&cart));
    ASSERT_FALSE(gen3_cart_restorable(&cart));
    ASSERT_FALSE(cart.work_ok);
    ASSERT_FALSE(gen3_cart_browsable(&cart));
    ASSERT_EQ(fake_store_count(&store, ".tmp"), 0);
    store.capacity = 64u * MIB;
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP3), Gen3CartOk);
    ASSERT_TRUE(sav_is_flash(gen3_cart_backup_name(&cart)));
    Gen3SaveTrainer t;
    ASSERT_TRUE(gen3_cart_trainer(&cart, &t));
    ASSERT_EQ(t.tid, 22222);
}

/* A restore needs a current backup of the cart, and a save on it the Flipper may keep or none */
TEST(test_restore_refused) {
    ASSERT_TRUE(backed_up());
    fake_store_fail_write_after(&store, 0);
    ASSERT_EQ(gen3_cart_backup(&cart, STAMP2), Gen3CartSdError);
    fake_store_fail_write_after(&store, 1000);
    ASSERT_FALSE(gen3_cart_restorable(&cart));
    ASSERT_EQ(gen3_cart_restore(&cart, BACKUP_NAME, STAMP2), Gen3CartRefused);
    /* a save the game loads but that isn't strict, or is padded: neither a slot to keep nor no
     * save */
    for(uint32_t i = 0; i < 2; i++) {
        ASSERT_TRUE(default_save());
        if(i == 0) {
            fake_save_set_counter(img, phys(0, 13), 2);
        } else {
            img[phys(0, 0) * SECTOR_LEN + 0x890] = 0x5A;
        }
        insert("AXVE", FakeSaveFlash);
        ASSERT_EQ(gen3_cart_connect(&cart), Gen3CartOk);
        ASSERT_EQ(gen3_cart_backup(&cart, STAMP), Gen3CartOk);
        ASSERT_EQ(gen3_cart_slots(&cart)->padded, i == 1);
        ASSERT_TRUE(gen3_cart_id(&cart)->can_write);
        ASSERT_TRUE(gen3_cart_backup_current(&cart));
        ASSERT_FALSE(gen3_cart_restorable(&cart));
        ASSERT_TRUE(put_other());
        ASSERT_EQ(gen3_cart_restore(&cart, OTHER_NAME, STAMP2), Gen3CartRefused);
        ASSERT_EQ(calls(GEN3_HCMD_READ_FOOTERS), 0);
        ASSERT_EQ(calls(GEN3_HCMD_ARM_WRITE), 0);
        ASSERT_TRUE(no_flash_ops());
    }
}

int main(void) {
    if(!bulk_file_open()) return 1;
    RUN(test_write_party_edit);
    RUN(test_write_box_straddle);
    RUN(test_write_twice);
    RUN(test_write_nothing);
    RUN(test_write_sd_full_precheck);
    RUN(test_write_requires_backup);
    RUN(test_write_refused_not_strict);
    RUN(test_write_refused_padded);
    RUN(test_write_refused_emerald);
    RUN(test_write_refused_no_flash_ops);
    RUN(test_write_over_corrupt_inactive);
    RUN(test_write_cart_changed);
    RUN(test_write_cart_changed_same_footer);
    RUN(test_write_game_saved_since);
    RUN(test_write_flash_error);
    RUN(test_write_commit_status);
    RUN(test_write_bad_response_relinks);
    RUN(test_write_upload_differs);
    RUN(test_write_prepare_sd_error);
    RUN(test_write_retry_after_commit);
    RUN(test_write_retry_mid_commit);
    RUN(test_write_short_glitch);
    RUN(test_write_verify_fails);
    RUN(test_write_written_differs);
    RUN(test_attach_after_power_cut);
    RUN(test_write_commit_link_lost);
    RUN(test_discard_after_write);
    RUN(test_discard_after_arming);
    RUN(test_reconnect_slot_rules);
    RUN(test_resolve_pending_kept);
    RUN(test_resolve_game_saved_reconnect);
    RUN(test_resolve_cut_after_commits_then_edit);
    RUN(test_resolve_uses_own_crcs);
    RUN(test_resolve_after_restore_check_lost);
    RUN(test_write_ok_unlinks);
    RUN(test_hub_after_write);
    RUN(test_hub_pending_restore);
    RUN(test_write_reports_erase_first);
    RUN(test_attach_other_cart_same_save);
    RUN(test_kept_slot_compared);
    RUN(test_post_write_backup_cart_differs);
    RUN(test_write_link_lost_in_backup);
    RUN(test_write_link_drop_sweep);
    RUN(test_write_link_drop_sweep_normal);
    RUN(test_write_power_cut_sweep);
    RUN(test_post_write_backup_sd_full);
    RUN(test_post_write_backup_read_back);
    RUN(test_reconnect_same);
    RUN(test_reconnect_changed);
    RUN(test_reconnect_after_erase);
    RUN(test_restore_ok);
    RUN(test_restore_onto_corrupt_save);
    RUN(test_restore_onto_empty_cart);
    RUN(test_restore_padded_backup);
    RUN(test_restore_wrong_game_backup);
    RUN(test_restore_bad_size);
    RUN(test_restore_check_trainer);
    RUN(test_restore_file_changes);
    RUN(test_restore_retry_after_commit);
    RUN(test_restore_no_backup);
    RUN(test_restore_refused);
    TEST_EXIT();
}
