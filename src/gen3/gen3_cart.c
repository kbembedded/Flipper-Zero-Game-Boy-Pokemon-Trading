#include <stdio.h>
#include <string.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_data.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_text.h>
#include <src/gen3/gen3_trade_block.h>

#define CART_ROM_BASE 0x08000000u /* the cart ROM on the GBA's bus (multiboot-and-save.md §3.1) */
#define CART_HEADER_LEN sizeof(((Gen3Cart*)0)->header)
#define CART_INFOS (GEN3_SAVE_SLOT_SECTORS * 2u) /* sectors 0..27: the two slots */
/* A backup name already taken gets -2 .. -99 */
#define CART_NAME_TRIES 99u
/* The longest backup name around its stamp: backups/XXXX_nosave_<stamp>-99.sav */
#define CART_BACKUP_NAME_FIXED (sizeof(GEN3_CART_BACKUP_DIR "/XXXX_nosave_-99.sav") - 1u)
#define CART_STAMP_MAX (GEN3_CART_NAME_MAX - 1u - CART_BACKUP_NAME_FIXED)
#define CART_ROM_FORMAT GEN3_CART_ROM_DIR "/%s_v%u.gba"
/* The header bytes a ROM copy's .part is tied to (cart_part_name) */
#define CART_PART_ID_OFF 0xA0u
#define CART_PART_ID_LEN 0x20u

_Static_assert(GEN3_CART_STAMP_MAX <= CART_STAMP_MAX, "a stamp fits a backup name");
_Static_assert(GEN3_CART_STAMP_MAX >= sizeof("YYYYMMDD-HHMMSS") - 1u, "the session's stamp fits");
_Static_assert(
    GEN3_CART_ROM_REGION % GEN3_CART_BUF_SIZE == 0,
    "a region is whole ROM_READs of one buffer");
_Static_assert(GEN3_CART_BUF_SIZE == GEN3_SAVE_SECTOR_SIZE, "the buffer holds a sector");
_Static_assert(sizeof(((Gen3HCartInfo*)0)->header) == CART_HEADER_LEN, "the header copy");
_Static_assert(CART_PART_ID_OFF + CART_PART_ID_LEN <= CART_HEADER_LEN, "a .part's header bytes");
/* roms/XXXX_v255.gba.01234567.part */
_Static_assert(
    sizeof(GEN3_CART_ROM_DIR "/XXXX_v255.gba.01234567.part") <= GEN3_CART_NAME_MAX,
    "a .part's name fits");

const char* gen3_cart_result_text(Gen3CartResult r) {
    /* At most 3 lines of 21 characters: the result screen starts its text at y 14 and may show
     * two buttons */
    static const char* const texts[] = {
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
    return (unsigned)r < sizeof(texts) / sizeof(texts[0]) ? texts[r] : "";
}

/* ---- Progress, results ---- */

static void cart_step(Gen3Cart* c, Gen3CartStep step, uint32_t done, uint32_t total) {
    if(c->progress.cb) c->progress.cb(c->progress.ctx, step, done, total);
}

static bool cart_cancelled(const Gen3Cart* c) {
    return c->progress.cancel && *c->progress.cancel;
}

/* No job talks to the helper until a new connect, which keeps trying for the budget of the
 * command whose end is unknown plus a sync: the helper finishes that command on its own first */
static void cart_unlink(Gen3Cart* c, uint32_t busy_us) {
    const uint32_t relink = busy_us + GEN3_HCLI_SYNC_US;
    c->linked = false;
    if(c->relink_us < relink) c->relink_us = relink;
}

/* A client call's result as a job's. A lost link, or a helper busy past the command's budget,
 * needs a new connect. On a flash step any status but REFUSED is a FlashError. */
static Gen3CartResult cart_link(Gen3Cart* c, Gen3HcliResult r, uint32_t busy_us, bool flash) {
    switch(r) {
    case Gen3HcliOk:
        return Gen3CartOk;
    case Gen3HcliLinkLost:
    case Gen3HcliBusyTimeout:
        cart_unlink(c, busy_us);
        return r == Gen3HcliLinkLost ? Gen3CartLinkLost : Gen3CartBusyTimeout;
    case Gen3HcliStatusError:
        /* the status only matters once the call says it came */
        if(c->cli->last_status == GEN3_HST_REFUSED) return Gen3CartRefused;
        return flash ? Gen3CartFlashError : Gen3CartHelperError;
    case Gen3HcliBadResponse:
        break;
    }
    return Gen3CartHelperError;
}

/* A read-class command: GEN3_HLINK_BUSY_US */
static Gen3CartResult cart_read(Gen3Cart* c, Gen3HcliResult r) {
    return cart_link(c, r, GEN3_HLINK_BUSY_US, false);
}

/* A write-class command (ARM_WRITE, ERASE_SECTOR .. COMMIT_SECTOR). A sound response the client
 * can't take leaves what the helper did unknown: like a lost one, it needs a new connect, and then
 * the write's next run reads the cart to find out. */
static Gen3CartResult cart_wlink(Gen3Cart* c, Gen3HcliResult r, uint32_t busy_us, bool flash) {
    if(r != Gen3HcliBadResponse) return cart_link(c, r, busy_us, flash);
    cart_unlink(c, busy_us);
    return Gen3CartHelperError;
}

/* A write of `need` bytes failed: the card is full, or failing */
static Gen3CartResult cart_sd_fail(Gen3Cart* c, uint32_t need) {
    return c->store.free_bytes(c->store.ctx) < need ? Gen3CartSdFull : Gen3CartSdError;
}

/* ---- The cart and its backup ---- */

/* The save the backup analyzed has a slot the game loads, with all 14 ids */
static bool cart_loads(const Gen3Cart* c) {
    if(!c->analyzed) return false;
    if(c->slots.result != Gen3SaveOk && c->slots.result != Gen3SaveError) return false;
    for(uint32_t k = 0; k < GEN3_SAVE_SLOT_SECTORS; k++) {
        if(c->slots.phys[k] == GEN3_SAVE_NO_SECTOR) return false;
    }
    return true;
}

/* The identification's reason, or Save when the analyzed save isn't one to write. can_write
 * stays: a restore may still write a cart whose save is empty or damaged. */
static void cart_set_why(Gen3Cart* c) {
    c->id.why = c->cart_why;
    if(c->id.why == Gen3CartRefuseNone && c->analyzed &&
       (!c->slots.strict_ok || c->slots.padded)) {
        c->id.why = Gen3CartRefuseSave;
    }
}

/* The backup no longer describes the cart, nor does anything a write left unchecked against it */
static void cart_drop_backup(Gen3Cart* c) {
    c->backup_current = false;
    c->backup_name[0] = '\0';
    c->analyzed = false;
    c->has_trainer = false;
    c->wslot = GEN3_SAVE_NO_SECTOR;
    cart_set_why(c);
}

/* The edits go with the save they were made on: none counted, and no working copy until a new
 * backup makes one */
static void cart_drop_edits(Gen3Cart* c) {
    c->dirty = 0;
    memset(c->changed, 0, sizeof(c->changed));
    c->work_ok = false;
}

void gen3_cart_init(
    Gen3Cart* c,
    Gen3Hcli* cli,
    const Gen3Store* store,
    uint8_t* buf,
    const Gen3CartProgress* p) {
    memset(c, 0, sizeof(*c));
    c->cli = cli;
    c->store = *store;
    c->buf = buf;
    c->wslot = GEN3_SAVE_NO_SECTOR;
    if(p) c->progress = *p;
}

/* HELLO. After a lost command the helper finishes it first, BUSY and then sending the response
 * nobody reads, and only then answers READY: each try's sync waits up to GEN3_HCLI_SYNC_US, so it
 * tries until relink_us has passed. A HELLO that came means the helper is done with it. It doesn't
 * link the cart: only a whole connect does. */
static Gen3CartResult cart_hello(Gen3Cart* c) {
    const uint32_t tries =
        c->relink_us ? (c->relink_us + GEN3_HCLI_SYNC_US - 1u) / GEN3_HCLI_SYNC_US : 1u;
    Gen3HcliResult r = Gen3HcliLinkLost;
    for(uint32_t t = 0; t < tries; t++) {
        if(cart_cancelled(c)) return Gen3CartCancelled;
        r = gen3_hcli_connect(c->cli, &c->hello);
        if(r == Gen3HcliOk) {
            c->relink_us = 0;
            return Gen3CartOk;
        }
        if(r != Gen3HcliLinkLost && r != Gen3HcliBusyTimeout) break;
    }
    return cart_read(c, r);
}

void gen3_cart_expect_boot(Gen3Cart* c) {
    if(c->relink_us < GEN3_CART_BOOT_US) c->relink_us = GEN3_CART_BOOT_US;
}

Gen3CartResult gen3_cart_connect(Gen3Cart* c) {
    cart_step(c, Gen3CartStepIdentify, 0, 3);
    /* Unlinked until the end: after a cancel or an error midway, info and id may describe
     * different carts, and no job may run on them */
    c->linked = false;
    Gen3CartResult r = cart_hello(c);
    if(r != Gen3CartOk) return r;
    cart_step(c, Gen3CartStepIdentify, 1, 3);
    if(cart_cancelled(c)) return Gen3CartCancelled;
    r = cart_link(c, gen3_hcli_cart_info(c->cli, &c->info), GEN3_HCLI_BUSY_SCAN_US, false);
    if(r != Gen3CartOk) return r;
    if(memcmp(c->info.header, c->header, CART_HEADER_LEN) != 0) cart_drop_backup(c);
    cart_step(c, Gen3CartStepIdentify, 2, 3);
    if(cart_cancelled(c)) return Gen3CartCancelled;
    Gen3HProbe probe;
    r = cart_read(c, gen3_hcli_save_probe(c->cli, &probe));
    if(r != Gen3CartOk) return r;
    gen3_cart_identify(&c->info, &probe, &c->id);
    c->cart_why = c->id.why;
    if(probe.flash_id != c->flash_id) cart_drop_backup(c);
    cart_set_why(c);
    c->linked = true;
    cart_step(c, Gen3CartStepIdentify, 3, 3);
    return Gen3CartOk;
}

/* ---- Backup ---- */

/* Every sector into the temporary file, with its CRC; then the cart reads each one again
 * (SECTOR_CRC32), and the card gives back the same bytes: a backup verified three ways, the
 * packet CRC being the first. Each sector's footer is kept for gen3_cart_attach. */
static Gen3CartResult cart_backup_read(Gen3Cart* c) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreCreate);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    for(uint8_t s = 0; s < GEN3_SAVE_SECTORS && r == Gen3CartOk; s++) {
        r = cart_read(c, gen3_hcli_read_sector(c->cli, s, c->buf));
        if(r != Gen3CartOk) break;
        c->crc[s] = gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE);
        if(s < CART_INFOS) gen3_save_sector_info(c->id.game, c->buf, &c->infos[s]);
        if(!st->write(st->ctx, f, s * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = cart_sd_fail(c, GEN3_SAVE_SECTOR_SIZE);
            break;
        }
        cart_step(c, Gen3CartStepBackupRead, s + 1u, GEN3_SAVE_SECTORS);
        if(cart_cancelled(c)) r = Gen3CartCancelled;
    }
    if(!st->close(st->ctx, f) && r == Gen3CartOk) r = Gen3CartSdError;
    for(uint8_t s = 0; s < GEN3_SAVE_SECTORS && r == Gen3CartOk; s++) {
        uint32_t crc = 0;
        r = cart_read(c, gen3_hcli_sector_crc32(c->cli, s, &crc));
        if(r == Gen3CartOk && crc != c->crc[s]) r = Gen3CartVerifyFailed;
        cart_step(c, Gen3CartStepBackupVerify, s + 1u, 2u * GEN3_SAVE_SECTORS);
        if(r == Gen3CartOk && cart_cancelled(c)) r = Gen3CartCancelled;
    }
    if(r != Gen3CartOk) return r;
    f = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    for(uint8_t s = 0; s < GEN3_SAVE_SECTORS && r == Gen3CartOk; s++) {
        if(!st->read(st->ctx, f, s * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = Gen3CartSdError;
        } else if(gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE) != c->crc[s]) {
            r = Gen3CartVerifyFailed;
        }
        cart_step(c, Gen3CartStepBackupVerify, GEN3_SAVE_SECTORS + s + 1u, 2u * GEN3_SAVE_SECTORS);
    }
    st->close(st->ctx, f);
    return r;
}

/* The slots, the trainer from the loaded slot's id 0, and Save as the reason not to write a save
 * that isn't strict or is padded */
static Gen3CartResult cart_backup_analyze(Gen3Cart* c) {
    if(!c->id.can_browse) return Gen3CartOk;
    cart_step(c, Gen3CartStepAnalyze, 0, 1);
    gen3_save_analyze(c->infos, &c->slots);
    c->analyzed = true;
    cart_set_why(c);
    const uint8_t sec0 = c->slots.phys[0];
    const bool loads = c->slots.result == Gen3SaveOk || c->slots.result == Gen3SaveError;
    if(loads && sec0 != GEN3_SAVE_NO_SECTOR) {
        const Gen3Store* st = &c->store;
        void* f = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreRead);
        if(!f) return Gen3CartSdError;
        const bool ok =
            st->read(st->ctx, f, sec0 * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE);
        st->close(st->ctx, f);
        if(!ok) return Gen3CartSdError;
        gen3_save_trainer(c->buf, &c->trainer);
        c->has_trainer = true;
    }
    cart_step(c, Gen3CartStepAnalyze, 1, 1);
    return Gen3CartOk;
}

/* The temporary file becomes backups/<code>_<tid | nosave>_<stamp>.sav, with -2, -3, ... if that
 * name is taken; from now on it is the current backup */
static Gen3CartResult cart_backup_keep(Gen3Cart* c, const char* stamp) {
    const Gen3Store* st = &c->store;
    char tid[8];
    if(c->has_trainer) {
        snprintf(tid, sizeof(tid), "%05u", (unsigned)c->trainer.tid);
    } else {
        snprintf(tid, sizeof(tid), "nosave");
    }
    for(uint32_t n = 1; n <= CART_NAME_TRIES; n++) {
        char more[12] = "";
        if(n > 1) snprintf(more, sizeof(more), "-%u", (unsigned)n);
        snprintf(
            c->backup_name,
            sizeof(c->backup_name),
            GEN3_CART_BACKUP_DIR "/%s_%s_%s%s.sav",
            c->id.code,
            tid,
            stamp,
            more);
        uint32_t size;
        if(st->size(st->ctx, c->backup_name, &size)) continue;
        if(!st->rename(st->ctx, GEN3_CART_BACKUP_TMP, c->backup_name)) break;
        c->backup_current = true;
        memcpy(c->header, c->info.header, CART_HEADER_LEN);
        c->flash_id = c->id.flash_id;
        return Gen3CartOk;
    }
    c->backup_name[0] = '\0';
    return Gen3CartSdError;
}

/* The card gives back each sector of the working copy as the backup's CRC says it is */
static Gen3CartResult cart_check_work(Gen3Cart* c) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, GEN3_CART_WORK_NAME, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t k = 0; k < GEN3_SAVE_SLOT_SECTORS && r == Gen3CartOk; k++) {
        if(!st->read(st->ctx, f, k * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = Gen3CartSdError;
        } else if(gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE) != c->crc[c->slots.phys[k]]) {
            r = Gen3CartVerifyFailed;
        }
    }
    st->close(st->ctx, f);
    return r;
}

/* The working copy anew from the backup: the loaded slot's 14 sectors in id order, no edits,
 * read back */
static Gen3CartResult cart_build_work(Gen3Cart* c) {
    const Gen3Store* st = &c->store;
    c->work_ok = false;
    void* in = st->open(st->ctx, c->backup_name, Gen3StoreRead);
    if(!in) return Gen3CartSdError;
    void* out = st->open(st->ctx, GEN3_CART_WORK_NAME, Gen3StoreCreate);
    if(!out) {
        st->close(st->ctx, in);
        return Gen3CartSdError;
    }
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t k = 0; k < GEN3_SAVE_SLOT_SECTORS && r == Gen3CartOk; k++) {
        const uint32_t at = c->slots.phys[k] * GEN3_SAVE_SECTOR_SIZE;
        if(!st->read(st->ctx, in, at, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = Gen3CartSdError;
        } else if(!st->write(
                      st->ctx, out, k * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = cart_sd_fail(c, GEN3_SAVE_SECTOR_SIZE);
        }
    }
    st->close(st->ctx, in);
    if(!st->close(st->ctx, out) && r == Gen3CartOk) r = Gen3CartSdError;
    if(r == Gen3CartOk) r = cart_check_work(c);
    if(r != Gen3CartOk) {
        st->remove(st->ctx, GEN3_CART_WORK_NAME);
        return r;
    }
    c->dirty = 0;
    memset(c->changed, 0, sizeof(c->changed));
    c->work_ok = true;
    return Gen3CartOk;
}

Gen3CartResult gen3_cart_backup(Gen3Cart* c, const char* stamp) {
    if(!c->id.can_read_save) return Gen3CartRefused;
    if(!stamp || strlen(stamp) > GEN3_CART_STAMP_MAX) return Gen3CartBadArg;
    if(!c->linked) return Gen3CartLinkLost;
    const Gen3Store* st = &c->store;
    cart_drop_backup(c);
    if(st->free_bytes(st->ctx) < GEN3_CART_BACKUP_SPACE) return Gen3CartSdFull;
    Gen3CartResult r = cart_backup_read(c);
    if(r == Gen3CartOk) r = cart_backup_analyze(c);
    if(r == Gen3CartOk) r = cart_backup_keep(c, stamp);
    if(r != Gen3CartOk) {
        st->remove(st->ctx, GEN3_CART_BACKUP_TMP);
        cart_drop_backup(c);
        return r;
    }
    /* The edits are gone with the save they were made on; a save the game loads gets a new
     * working copy. If that fails, the backup isn't current, but its file, verified, stays:
     * nothing deletes a backup. */
    cart_drop_edits(c);
    if(!(c->id.can_browse && cart_loads(c))) {
        st->remove(st->ctx, GEN3_CART_WORK_NAME);
        return Gen3CartOk;
    }
    r = cart_build_work(c);
    if(r != Gen3CartOk) cart_drop_backup(c);
    return r;
}

/* The game's choice is a slot it loads */
static bool cart_slots_load(const Gen3SaveSlots* s) {
    return s->result == Gen3SaveOk || s->result == Gen3SaveError;
}

/* READ_FOOTERS, as seen and seen_slots: each of sectors 0..27 as the helper sees it, valid with
 * the signature, an id under 14 and its checksum over all 0xF80 data bytes. A footer doesn't give
 * the checksum over the game's section size, but past it a sector the game wrote is zero, so for
 * the game's own sectors the two agree. */
static Gen3CartResult cart_footer_view(Gen3Cart* c) {
    const Gen3CartResult r = cart_read(c, gen3_hcli_read_footers(c->cli, c->footers));
    if(r != Gen3CartOk) return r;
    for(uint32_t s = 0; s < CART_INFOS; s++) {
        const Gen3HFooter* h = &c->footers[s];
        Gen3SaveSectorInfo* v = &c->seen[s];
        v->f.id = h->id;
        v->f.checksum = h->checksum;
        v->f.signature = h->signature;
        v->f.counter = h->counter;
        v->full_ok = h->full_checksum == h->checksum;
        v->valid = h->signature == GEN3_SAVE_SIGNATURE && h->id < GEN3_SAVE_SLOT_SECTORS &&
                   v->full_ok;
    }
    gen3_save_analyze(c->seen, &c->seen_slots);
    return Gen3CartOk;
}

/* READ_FOOTERS showed sector s as the backup has it: id, checksum, signature, counter, and
 * whether the checksum over all 0xF80 bytes holds */
static bool cart_sector_same(const Gen3Cart* c, uint32_t s) {
    const Gen3SaveSectorInfo* v = &c->seen[s];
    const Gen3SaveSectorInfo* b = &c->infos[s];
    return v->f.id == b->f.id && v->f.checksum == b->f.checksum &&
           v->f.signature == b->f.signature && v->f.counter == b->f.counter &&
           v->full_ok == b->full_ok;
}

/* READ_FOOTERS showed sector s, in a slot a write with counter `counter` may have changed, holding
 * only what the backup has there, or what that write or the game put there: the backup's footer;
 * no valid signature (erased, or an image whose commit byte isn't programmed yet: the game skips
 * it); or the signature with that counter, as the write's own sectors carry it, and as the game's
 * next save after the backup's does too. Any other sector with the signature (another cart's save,
 * say) is not the backup's to lose. */
static bool cart_sector_may_stand(const Gen3Cart* c, uint32_t s, uint32_t counter) {
    const Gen3SaveSectorInfo* v = &c->seen[s];
    return cart_sector_same(c, s) || v->f.signature != GEN3_SAVE_SIGNATURE ||
           v->f.counter == counter;
}

/* ... all of sectors 0..27, those of slot skip only as cart_sector_may_stand (GEN3_SAVE_NO_SECTOR:
 * none skipped) */
static bool cart_footers_same(const Gen3Cart* c, uint8_t skip, uint32_t counter) {
    for(uint32_t s = 0; s < CART_INFOS; s++) {
        const bool ok = s / GEN3_SAVE_SLOT_SECTORS == skip ? cart_sector_may_stand(c, s, counter) :
                                                             cart_sector_same(c, s);
        if(!ok) return false;
    }
    return true;
}

/* *same: READ_FOOTERS shows the cart as the backup has it, except that the slot of a write or
 * restore not yet backed up (wslot) may hold, besides the backup's sectors, sectors without the
 * signature and sectors with the write's counter (cart_sector_may_stand): the write's own, cut
 * short or finished, or, after a power cut, a game save, which carries the same counter. The
 * other slot, the one the backup's save is in, must be exactly as backed up. The write's next run
 * then reads the slot again before anything is armed: its own whole save there is found written
 * and isn't written again, any other save there is Changed, and anything less is written over. */
static Gen3CartResult cart_still_backed_up(Gen3Cart* c, bool* same) {
    *same = false;
    const Gen3CartResult r = cart_footer_view(c);
    if(r == Gen3CartOk) *same = cart_footers_same(c, c->wslot, c->wcounter);
    return r;
}

/* Settles a write or restore left pending, once the footers showed the cart as backed up but for
 * its slot (with the write section below) */
static Gen3CartResult cart_resolve(Gen3Cart* c);

Gen3CartResult gen3_cart_attach(Gen3Cart* c, const char* stamp, bool* dropped) {
    *dropped = false;
    c->finished = Gen3CartFinishedNone;
    if(!c->linked) return Gen3CartLinkLost;
    const bool pending = c->dirty != 0 || gen3_cart_changes(c) > 0;
    if(!c->id.can_read_save) {
        /* Nothing to back up, and so nothing to browse, write or restore: each needs
         * can_read_save. The cart's info and its ROM copy are still there. Edits made on the
         * cart that was in before are gone. */
        cart_drop_backup(c);
        cart_drop_edits(c);
        *dropped = pending;
        return Gen3CartOk;
    }
    /* The connect already found the same header and flash id, or the backup isn't current */
    if(c->backup_current) {
        bool same = false;
        Gen3CartResult r = cart_still_backed_up(c, &same);
        if(r != Gen3CartOk) return r;
        if(same) {
            r = cart_resolve(c);
            /* Changed: the game saved over a pending write, which took the backup and the edits
             * with it; the cart as it is now gets a new backup */
            if(r != Gen3CartChanged) return r;
        }
    }
    const Gen3CartResult r = gen3_cart_backup(c, stamp);
    if(r == Gen3CartOk) *dropped = pending;
    return r;
}

Gen3CartResult gen3_cart_discard(Gen3Cart* c) {
    if(!c->backup_current || !c->id.can_browse || !cart_loads(c)) return Gen3CartBadArg;
    /* The edits go, and with them the claim of a write of them not yet backed up on its slot. If
     * that write finished, the game loads it now: so the slot is compared now, with nothing
     * skipped, while the link lets it, else by the next attach or reconnect. A cart that isn't as
     * backed up keeps neither the backup nor the working copy built from it. */
    if(c->wslot != GEN3_SAVE_NO_SECTOR && c->linked) {
        const Gen3CartResult r = cart_footer_view(c);
        if(r != Gen3CartOk) return r;
        if(!cart_footers_same(c, GEN3_SAVE_NO_SECTOR, 0)) {
            cart_drop_backup(c);
            cart_drop_edits(c);
            return Gen3CartChanged;
        }
    }
    c->wslot = GEN3_SAVE_NO_SECTOR;
    return cart_build_work(c);
}

/* ---- What the backup says ---- */

bool gen3_cart_linked(const Gen3Cart* c) {
    return c->linked;
}

Gen3CartFinished gen3_cart_finished(const Gen3Cart* c) {
    return c->finished;
}

bool gen3_cart_pending(const Gen3Cart* c) {
    return c->wslot != GEN3_SAVE_NO_SECTOR;
}

const Gen3CartId* gen3_cart_id(const Gen3Cart* c) {
    return &c->id;
}

const Gen3SaveSlots* gen3_cart_slots(const Gen3Cart* c) {
    return &c->slots;
}

bool gen3_cart_trainer(const Gen3Cart* c, Gen3SaveTrainer* out) {
    if(!c->has_trainer) return false;
    *out = c->trainer;
    return true;
}

const char* gen3_cart_backup_name(const Gen3Cart* c) {
    return c->backup_current ? c->backup_name : "";
}

bool gen3_cart_backup_current(const Gen3Cart* c) {
    return c->backup_current;
}

bool gen3_cart_browsable(const Gen3Cart* c) {
    return c->id.can_browse && c->backup_current && cart_loads(c) && c->work_ok;
}

bool gen3_cart_work_ok(const Gen3Cart* c) {
    return c->work_ok;
}

bool gen3_cart_writable(const Gen3Cart* c, Gen3CartRefuse* why) {
    const bool ok = c->id.can_write && gen3_cart_browsable(c) && c->slots.strict_ok &&
                    !c->slots.padded;
    if(!why) return ok;
    if(ok) {
        *why = Gen3CartRefuseNone;
    } else {
        *why = c->id.why != Gen3CartRefuseNone ? c->id.why : Gen3CartRefuseSave;
    }
    return ok;
}

/* ---- The working copy ---- */

/* The working copy's bytes of n spans, back to back in data; read or written in one opening */
static bool
    cart_work_io(Gen3Cart* c, bool write, const Gen3SaveSpan* sp, uint8_t n, uint8_t* data) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, GEN3_CART_WORK_NAME, write ? Gen3StoreUpdate : Gen3StoreRead);
    if(!f) return false;
    bool ok = true;
    uint32_t at = 0;
    for(uint8_t i = 0; i < n && ok; i++) {
        const uint32_t off = sp[i].id * GEN3_SAVE_SECTOR_SIZE + sp[i].off;
        ok = write ? st->write(st->ctx, f, off, data + at, sp[i].len) :
                     st->read(st->ctx, f, off, data + at, sp[i].len);
        at += sp[i].len;
    }
    return st->close(st->ctx, f) && ok;
}

/* The party count as the save stores it */
static Gen3CartResult cart_party_count(Gen3Cart* c, uint8_t* count) {
    const Gen3SaveSpan sp = gen3_save_party_count_span(c->id.game);
    if(!gen3_cart_browsable(c) || sp.len != 1) return Gen3CartBadArg;
    return cart_work_io(c, false, &sp, 1, count) ? Gen3CartOk : Gen3CartSdError;
}

uint8_t gen3_cart_party_count(Gen3Cart* c) {
    uint8_t n = 0;
    if(cart_party_count(c, &n) != Gen3CartOk) return 0;
    return n < GEN3_SAVE_PARTY_MAX ? n : GEN3_SAVE_PARTY_MAX;
}

bool gen3_cart_box_name(Gen3Cart* c, uint8_t box, char out[9]) {
    out[0] = '\0';
    const Gen3SaveSpan sp = gen3_save_box_name_span(box);
    uint8_t raw[9];
    if(!gen3_cart_browsable(c) || sp.len != sizeof(raw)) return false;
    if(!cart_work_io(c, false, &sp, 1, raw)) return false;
    gen3_text_decode(raw, sizeof(raw), out, 9);
    return true;
}

/* The changed map's bit of a slot gen3_save_mon_spans accepted */
static uint16_t cart_mon_bit(uint8_t box, uint8_t slot) {
    if(box == GEN3_SAVE_PARTY) return slot;
    return (uint16_t)(GEN3_SAVE_PARTY_MAX + box * GEN3_SAVE_BOX_SLOTS + slot);
}

Gen3CartResult gen3_cart_mon_get(
    Gen3Cart* c,
    uint8_t box,
    uint8_t slot,
    uint8_t out[GEN3_PARTY_SIZE],
    size_t* len) {
    Gen3SaveSpan sp[2];
    const uint8_t n = gen3_cart_browsable(c) ? gen3_save_mon_spans(c->id.game, box, slot, sp) : 0;
    if(n == 0) return Gen3CartBadArg;
    if(!cart_work_io(c, false, sp, n, out)) return Gen3CartSdError;
    *len = box == GEN3_SAVE_PARTY ? GEN3_PARTY_SIZE : GEN3_BOX_SIZE;
    return Gen3CartOk;
}

Gen3CartResult gen3_cart_mon_put(Gen3Cart* c, uint8_t box, uint8_t slot, const Gen3Pkm* p) {
    Gen3SaveSpan sp[3];
    uint8_t n = gen3_cart_browsable(c) ? gen3_save_mon_spans(c->id.game, box, slot, sp) : 0;
    if(n == 0) return Gen3CartBadArg;
    /* The write pending writes the working copy as it was given: settled first */
    if(gen3_cart_pending(c)) return Gen3CartRefused;
    /* the Pokemon, then for an append the new count, written in one go */
    uint8_t mon[GEN3_PARTY_SIZE + 1];
    const size_t len = box == GEN3_SAVE_PARTY ? GEN3_PARTY_SIZE : GEN3_BOX_SIZE;
    gen3_save_pkm_to_mon(p, len, mon);
    if(box == GEN3_SAVE_PARTY) {
        uint8_t count = 0;
        const Gen3CartResult r = cart_party_count(c, &count);
        if(r != Gen3CartOk) return r;
        if(slot > count) return Gen3CartBadArg;
        if(slot == count) {
            sp[n++] = gen3_save_party_count_span(c->id.game);
            mon[len] = (uint8_t)(count + 1u);
        }
    }
    /* Marked first: a failed write is a change too, which only gen3_cart_discard undoes, so that
     * Discard and the warning on leaving show even when this was the first edit */
    for(uint8_t i = 0; i < n; i++) c->dirty |= (uint16_t)(1u << sp[i].id);
    const uint16_t bit = cart_mon_bit(box, slot);
    c->changed[bit / 8u] |= (uint8_t)(1u << (bit % 8u));
    if(!cart_work_io(c, true, sp, n, mon)) {
        /* a span may be half written: no edit can be trusted to go to the cart now */
        c->work_ok = false;
        return Gen3CartSdError;
    }
    return Gen3CartOk;
}

bool gen3_cart_mon_changed(const Gen3Cart* c, uint8_t box, uint8_t slot) {
    if(box == GEN3_SAVE_PARTY ? slot >= GEN3_SAVE_PARTY_MAX :
                                box >= GEN3_SAVE_BOXES || slot >= GEN3_SAVE_BOX_SLOTS) {
        return false;
    }
    const uint16_t bit = cart_mon_bit(box, slot);
    return (c->changed[bit / 8u] >> (bit % 8u)) & 1u;
}

uint16_t gen3_cart_changes(const Gen3Cart* c) {
    uint16_t n = 0;
    for(uint32_t i = 0; i < sizeof(c->changed); i++) {
        for(uint8_t b = c->changed[i]; b; b &= (uint8_t)(b - 1u)) n++;
    }
    return n;
}

/* ---- Opening and putting a save's Pokemon ---- */

/* The working copy's answer as the editor's side sees it */
static Gen3CartMon cart_mon_result(Gen3CartResult r) {
    switch(r) {
    case Gen3CartOk:
        return Gen3CartMonOk;
    case Gen3CartSdError:
        return Gen3CartMonSdError;
    case Gen3CartRefused: /* gen3_cart_mon_put's while a write is pending */
        return Gen3CartMonPending;
    default:
        return Gen3CartMonBadArg;
    }
}

/* gen3_pkm_check, then the offer rules of a trade to Ruby: what the game takes back as it is */
static Gen3CartMon cart_mon_rules(const Gen3Pkm* p) {
    switch(gen3_pkm_check(p)) {
    case Gen3CheckOk:
        break;
    case Gen3CheckBadEgg:
        return Gen3CartMonBadEgg;
    case Gen3CheckEgg:
        return Gen3CartMonEgg;
    default:
        return Gen3CartMonBadData;
    }
    switch(gen3_trade_offer_check(p)) {
    case Gen3RefuseNone:
        return Gen3CartMonOk;
    case Gen3RefuseMail:
        return Gen3CartMonMail;
    case Gen3RefuseItem:
        return Gen3CartMonItem;
    case Gen3RefuseEgg:
        return Gen3CartMonEgg;
    case Gen3RefuseBadEgg:
        return Gen3CartMonBadEgg;
    default:
        return Gen3CartMonBadData;
    }
}

/* The only way a Pokemon from the editor goes into the working copy: the rules first */
static Gen3CartMon
    cart_mon_put(Gen3Cart* c, const Gen3Pkm* p, uint8_t box, uint8_t slot, bool* put) {
    *put = false;
    const Gen3CartMon m = cart_mon_rules(p);
    if(m != Gen3CartMonOk) return m;
    const Gen3CartResult r = gen3_cart_mon_put(c, box, slot, p);
    *put = r == Gen3CartOk;
    return cart_mon_result(r);
}

/* A slot of this save's layout, while browsable */
static bool cart_mon_slot(const Gen3Cart* c, uint8_t box, uint8_t slot) {
    Gen3SaveSpan sp[2];
    return gen3_cart_browsable(c) && gen3_save_mon_spans(c->id.game, box, slot, sp) > 0;
}

Gen3CartMon gen3_cart_mon_open(
    Gen3Cart* c,
    Gen3Editor* ed,
    uint8_t box,
    uint8_t slot,
    uint8_t orig[GEN3_PARTY_SIZE],
    size_t* orig_len,
    Gen3Pkm* scratch) {
    if(!cart_mon_slot(c, box, slot)) return Gen3CartMonBadArg;
    if(gen3_cart_pending(c)) return Gen3CartMonPending;
    if(box == GEN3_SAVE_PARTY) {
        uint8_t count = 0;
        const Gen3CartResult r = cart_party_count(c, &count);
        if(r != Gen3CartOk) return cart_mon_result(r);
        if(slot == count) return Gen3CartMonEmpty;
        if(slot > count) return Gen3CartMonBadArg;
    }
    size_t len = 0;
    const Gen3CartResult r = gen3_cart_mon_get(c, box, slot, orig, &len);
    if(r != Gen3CartOk) return cart_mon_result(r);
    Gen3MonSummary s;
    gen3_save_mon_summary(orig, len, &s);
    switch(s.kind) {
    case Gen3MonOk:
        break;
    case Gen3MonEmpty:
        /* a party slot below the count holds a Pokemon in the game */
        return box == GEN3_SAVE_PARTY ? Gen3CartMonBadData : Gen3CartMonEmpty;
    case Gen3MonEgg:
        return Gen3CartMonEgg;
    case Gen3MonBad:
        return Gen3CartMonBadEgg;
    }
    if(!gen3_save_mon_to_pkm(orig, len, scratch)) return Gen3CartMonBadData;
    const Gen3CartMon m = cart_mon_rules(scratch);
    if(m != Gen3CartMonOk) return m;
    /* false leaves the editor as it was: nothing of it may reach a slot */
    if(!gen3_edit_load(ed, scratch)) {
        return gen3_bulk_ready() ? Gen3CartMonBadData : Gen3CartMonNoData;
    }
    /* The editor's bytes, not the save's: loading normalised them, and only an edit may change the
     * slot */
    *orig_len = len;
    gen3_save_pkm_to_mon(&ed->pkm, len, orig);
    return Gen3CartMonOk;
}

Gen3CartMon gen3_cart_mon_close(
    Gen3Cart* c,
    Gen3Editor* ed,
    uint8_t box,
    uint8_t slot,
    const uint8_t* orig,
    size_t orig_len,
    uint8_t mon[GEN3_PARTY_SIZE],
    Gen3Pkm* scratch,
    bool* put) {
    *put = false;
    const size_t len = box == GEN3_SAVE_PARTY ? GEN3_PARTY_SIZE : GEN3_BOX_SIZE;
    if(!cart_mon_slot(c, box, slot) || orig_len != len) return Gen3CartMonBadArg;
    gen3_save_pkm_to_mon(&ed->pkm, len, mon);
    if(memcmp(mon, orig, len) == 0) return Gen3CartMonOk;
    /* Only a species change sets MetGame in the editor, which then makes a new Pokemon, met in
     * Ruby: that one is this cart's game's, as a new Pokemon put here is (gen3_cart_mon_place) */
    if(!gen3_save_mon_to_pkm(orig, len, scratch)) return Gen3CartMonBadArg;
    if(gen3_pkm_get(scratch, Gen3FieldSpecies) != gen3_pkm_get(&ed->pkm, Gen3FieldSpecies) ||
       gen3_pkm_get(scratch, Gen3FieldMetGame) != gen3_pkm_get(&ed->pkm, Gen3FieldMetGame)) {
        gen3_pkm_set(&ed->pkm, Gen3FieldMetGame, c->id.met_game);
        gen3_pkm_fix_checksum(&ed->pkm);
    }
    return cart_mon_put(c, &ed->pkm, box, slot, put);
}

Gen3CartMon gen3_cart_mon_place(
    Gen3Cart* c,
    Gen3Editor* ed,
    uint8_t box,
    uint8_t slot,
    bool new_mon,
    uint8_t mon[GEN3_PARTY_SIZE]) {
    if(!cart_mon_slot(c, box, slot)) return Gen3CartMonBadArg;
    if(gen3_cart_pending(c)) return Gen3CartMonPending;
    /* Only a free slot: never over a Pokemon */
    if(box == GEN3_SAVE_PARTY) {
        uint8_t count = 0;
        const Gen3CartResult r = cart_party_count(c, &count);
        if(r != Gen3CartOk) return cart_mon_result(r);
        if(slot != count) return Gen3CartMonTaken;
    } else {
        size_t len = 0;
        const Gen3CartResult r = gen3_cart_mon_get(c, box, slot, mon, &len);
        if(r != Gen3CartOk) return cart_mon_result(r);
        Gen3MonSummary s;
        gen3_save_mon_summary(mon, len, &s);
        if(s.kind != Gen3MonEmpty) return Gen3CartMonTaken;
    }
    if(new_mon) {
        gen3_pkm_set(&ed->pkm, Gen3FieldMetGame, c->id.met_game);
        gen3_pkm_fix_checksum(&ed->pkm);
    }
    bool put = false;
    return cart_mon_put(c, &ed->pkm, box, slot, &put);
}

/* ---- Write and restore ---- */

#define CART_FOOTER_OFF 0xFF4u /* a sector's footer (multiboot-and-save.md §5.2) */
/* A write's free-space check: its new backup, and a sector to spare */
#define CART_WRITE_SPACE (GEN3_SAVE_SIZE + GEN3_CART_BUF_SIZE)
/* Prepare and Verify: the 14 sectors, then the footers */
#define CART_SLOT_STEPS (GEN3_SAVE_SLOT_SECTORS + 1u)
/* PostBackup: each sector composed, checked on the cart, then read back */
#define CART_POST_STEPS (3u * GEN3_SAVE_SECTORS)
#define CART_NO_IMAGE 0xFFu

/* A write: its plan, ARM_WRITE's mode, and where its 14 images come from */
typedef struct {
    Gen3SaveWritePlan plan;
    /* 0: over the slot the game doesn't load, the other one being the save as backed up; 1: a
     * restore onto a cart whose game loads neither slot */
    uint8_t mode;
    /* NULL: the working copy, whose sector k is id k's, a clean one copied on the cart from
     * plan.src[k] instead of uploaded; else a restore's backup file, whose sector plan.src[k] is id
     * k's, every one uploaded */
    const char* from;
} CartWrite;

/* The slot the write keeps: the save the game loads now, or in mode 1 whatever is there */
static uint8_t cart_keep(const CartWrite* w) {
    return (uint8_t)(1u - w->plan.target);
}

static bool cart_uploads(const Gen3Cart* c, const CartWrite* w, uint8_t k) {
    return w->from || ((c->dirty >> k) & 1u);
}

/* Image k into c->buf from f, the source's open file: id k's sector, with the plan's counter; from
 * a backup file, with the bytes past its section zeroed (the game clears them: pokeruby:
 * src/save.c: WriteSingleChunk; the checksum doesn't cover them); from the working copy, an edited
 * one with its checksum anew. It must then be a sector both the game and the helper take as id k:
 * valid, and its checksum the same over all 0xF80 data bytes. */
static Gen3CartResult cart_image(Gen3Cart* c, const CartWrite* w, void* f, uint8_t k) {
    const Gen3Store* st = &c->store;
    const uint32_t at = (uint32_t)(w->from ? w->plan.src[k] : k) * GEN3_SAVE_SECTOR_SIZE;
    if(!st->read(st->ctx, f, at, c->buf, GEN3_SAVE_SECTOR_SIZE)) return Gen3CartSdError;
    const uint16_t size = gen3_save_section_size(c->id.game, k);
    Gen3SaveFooter ft;
    gen3_save_footer_get(c->buf, &ft);
    if(w->from && size != 0) memset(c->buf + size, 0, CART_FOOTER_OFF - size);
    ft.counter = w->plan.counter;
    if(!w->from && ((c->dirty >> k) & 1u)) ft.checksum = gen3_save_checksum(c->buf, size);
    gen3_save_footer_set(c->buf, &ft);
    Gen3SaveSectorInfo info;
    gen3_save_sector_info(c->id.game, c->buf, &info);
    if(info.valid && info.full_ok && info.f.id == k) return Gen3CartOk;
    return w->from ? Gen3CartBadBackup : Gen3CartVerifyFailed;
}

/* Step 3: each image once, and its CRC. The working copy's go back into it, so that it is the new
 * slot; the same again changes nothing, so a run after an interrupted one finds the same CRCs. */
static Gen3CartResult cart_prepare(Gen3Cart* c, const CartWrite* w) {
    const Gen3Store* st = &c->store;
    void* f = st->open(
        st->ctx,
        w->from ? w->from : GEN3_CART_WORK_NAME,
        w->from ? Gen3StoreRead : Gen3StoreUpdate);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    bool torn = false;
    for(uint8_t k = 0; k < GEN3_SAVE_SLOT_SECTORS && r == Gen3CartOk; k++) {
        r = cart_image(c, w, f, k);
        if(r != Gen3CartOk) break;
        if(!w->from &&
           !st->write(st->ctx, f, k * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            torn = true;
            r = Gen3CartSdError;
            break;
        }
        c->exp_crc[k] = gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE);
        cart_step(c, Gen3CartStepPrepare, k + 1u, CART_SLOT_STEPS);
    }
    if(!st->close(st->ctx, f) && !w->from) torn = true;
    if(torn) {
        /* a sector may be half written: no edit can be trusted to go to the cart now */
        c->work_ok = false;
        if(r == Gen3CartOk) r = Gen3CartSdError;
    }
    return r;
}

/* The footers (cart_footer_view) show the write done: the game loads slot T, with the plan's
 * counter, a strict slot */
static bool cart_view_written(const Gen3Cart* c, const CartWrite* w) {
    const Gen3SaveSlots* v = &c->seen_slots;
    return cart_slots_load(v) && v->active == w->plan.target &&
           v->save_counter == w->plan.counter && v->strict_ok;
}

/* The footers show the cart as backed up, but for slot T: the slot kept's footers are the
 * backup's, slot T holds only what the backup, this write (its counter) or the game put there
 * (cart_sector_may_stand), and the game still loads the slot kept with its counter, or in mode 1
 * neither slot. A slot T changed by an interrupted run of this write isn't the game's choice. */
static bool cart_view_fresh(const Gen3Cart* c, const CartWrite* w) {
    if(!cart_footers_same(c, w->plan.target, w->plan.counter)) return false;
    const Gen3SaveSlots* v = &c->seen_slots;
    if(w->mode == 1) return v->result == Gen3SaveEmpty || v->result == Gen3SaveCorrupt;
    return cart_slots_load(v) && v->active == cart_keep(w) &&
           v->save_counter == c->slots.save_counter;
}

/* SECTOR_CRC32 of each sector of the slot kept is the backup's: a change that keeps every footer
 * and checksum shows here, as Changed, before anything is armed */
static Gen3CartResult cart_kept_same(Gen3Cart* c, const CartWrite* w) {
    const uint8_t first = (uint8_t)(cart_keep(w) * GEN3_SAVE_SLOT_SECTORS);
    for(uint8_t s = first; s < first + GEN3_SAVE_SLOT_SECTORS; s++) {
        uint32_t crc = 0;
        const Gen3CartResult r = cart_read(c, gen3_hcli_sector_crc32(c->cli, s, &crc));
        if(r != Gen3CartOk) return r;
        if(crc != c->crc[s]) return Gen3CartChanged;
    }
    return Gen3CartOk;
}

/* *same: SECTOR_CRC32 of each dst[k] is exp_crc[k]. Progress Verify. */
static Gen3CartResult cart_dst_same(Gen3Cart* c, const CartWrite* w, bool* same) {
    *same = false;
    for(uint8_t k = 0; k < GEN3_SAVE_SLOT_SECTORS; k++) {
        uint32_t crc = 0;
        const Gen3CartResult r =
            cart_read(c, gen3_hcli_sector_crc32(c->cli, w->plan.dst[k], &crc));
        if(r != Gen3CartOk) return r;
        if(crc != c->exp_crc[k]) return Gen3CartOk;
        cart_step(c, Gen3CartStepVerify, k + 1u, CART_SLOT_STEPS);
    }
    *same = true;
    return Gen3CartOk;
}

/* Step 7: each image uploaded, after the card gave it back as step 3 read it (only that one is
 * checked on the cart after), or copied on the cart from the slot kept */
static Gen3CartResult cart_program(Gen3Cart* c, const CartWrite* w) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, w->from ? w->from : GEN3_CART_WORK_NAME, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    const Gen3SaveWritePlan* p = &w->plan;
    Gen3CartResult r = Gen3CartOk;
    for(uint8_t k = 0; k < GEN3_SAVE_SLOT_SECTORS && r == Gen3CartOk; k++) {
        if(cart_uploads(c, w, k)) {
            r = cart_image(c, w, f, k);
            if(r == Gen3CartOk && gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE) != c->exp_crc[k]) {
                r = Gen3CartVerifyFailed;
            }
            if(r == Gen3CartOk) {
                r = cart_wlink(
                    c,
                    gen3_hcli_write_sector(c->cli, p->dst[k], true, c->buf),
                    GEN3_HCLI_BUSY_FLASH_US,
                    true);
            }
        } else {
            r = cart_wlink(
                c,
                gen3_hcli_copy_sector(c->cli, p->src[k], p->dst[k], true, p->counter),
                GEN3_HCLI_BUSY_FLASH_US,
                true);
        }
        if(r == Gen3CartOk) cart_step(c, Gen3CartStepProgram, k + 1u, GEN3_SAVE_SLOT_SECTORS);
    }
    st->close(st->ctx, f);
    return r;
}

/* Step 8. A commit is never sent again (multiboot-and-save.md §6.5): any status but OK is answered
 * by reading the sector back; if it holds its image the commit took, and the next one follows. A
 * lost link, or a response the client can't take, leaves the question to the write's next run. */
static Gen3CartResult cart_commit(Gen3Cart* c, const CartWrite* w) {
    for(uint8_t k = 0; k < GEN3_SAVE_SLOT_SECTORS; k++) {
        const uint8_t dst = w->plan.dst[k];
        const Gen3HcliResult h = gen3_hcli_commit_sector(c->cli, dst);
        if(h != Gen3HcliOk) {
            /* mapped at once: the commit's status is the client's last only until the next call */
            const Gen3CartResult r = cart_wlink(c, h, GEN3_HLINK_BUSY_US, true);
            if(h != Gen3HcliStatusError) return r;
            uint32_t crc = 0;
            const Gen3CartResult v = cart_read(c, gen3_hcli_sector_crc32(c->cli, dst, &crc));
            if(v != Gen3CartOk) return v;
            if(crc != c->exp_crc[k]) return r;
        }
        cart_step(c, Gen3CartStepCommit, k + 1u, GEN3_SAVE_SLOT_SECTORS);
    }
    return Gen3CartOk;
}

/* Steps 5-8: arm for slot T, then erase, program and commit its 14 sectors, in passes, so that the
 * slot gets a valid signature only once every sector holds its image (multiboot-and-save.md §5.8) */
static Gen3CartResult cart_flash(Gen3Cart* c, const CartWrite* w) {
    const Gen3SaveWritePlan* p = &w->plan;
    Gen3CartResult r = cart_wlink(
        c,
        gen3_hcli_arm_write(c->cli, c->flash_id, p->target, w->mode, c->slots.save_counter),
        GEN3_HLINK_BUSY_US,
        false);
    /* Before the first erase, so the screen says not to power off while it runs too */
    if(r == Gen3CartOk) cart_step(c, Gen3CartStepErase, 0, GEN3_SAVE_SLOT_SECTORS);
    for(uint8_t k = 0; k < GEN3_SAVE_SLOT_SECTORS && r == Gen3CartOk; k++) {
        r = cart_wlink(
            c, gen3_hcli_erase_sector(c->cli, p->dst[k]), GEN3_HCLI_BUSY_FLASH_US, true);
        if(r == Gen3CartOk) cart_step(c, Gen3CartStepErase, k + 1u, GEN3_SAVE_SLOT_SECTORS);
    }
    if(r == Gen3CartOk) r = cart_program(c, w);
    if(r == Gen3CartOk) r = cart_commit(c, w);
    return r;
}

/* Step 9: each new sector holds its image, and then the footers show the game loading them */
static Gen3CartResult cart_verify(Gen3Cart* c, const CartWrite* w) {
    bool same = false;
    Gen3CartResult r = cart_dst_same(c, w, &same);
    if(r != Gen3CartOk) return r;
    if(!same) return Gen3CartVerifyFailed;
    r = cart_footer_view(c);
    if(r != Gen3CartOk) return r;
    cart_step(c, Gen3CartStepVerify, CART_SLOT_STEPS, CART_SLOT_STEPS);
    return cart_view_written(c, w) ? Gen3CartOk : Gen3CartVerifyFailed;
}

/* The index k of the image that sector s of the cart now holds, or CART_NO_IMAGE */
static uint8_t cart_image_at(const CartWrite* w, uint32_t s) {
    for(uint8_t k = 0; k < GEN3_SAVE_SLOT_SECTORS; k++) {
        if(w->plan.dst[k] == s) return k;
    }
    return CART_NO_IMAGE;
}

/* The CRC-32 sector s of the cart has now: its image's, or the backup's */
static uint32_t cart_now_crc(const Gen3Cart* c, const CartWrite* w, uint32_t s) {
    const uint8_t k = cart_image_at(w, s);
    return k == CART_NO_IMAGE ? c->crc[s] : c->exp_crc[k];
}

/* Step 10's file: the backup's 32 sectors, then the 14 images over theirs, every one checked
 * against its CRC as it is read; at most 2 files open at once */
static Gen3CartResult cart_post_compose(Gen3Cart* c, const CartWrite* w) {
    const Gen3Store* st = &c->store;
    if(st->free_bytes(st->ctx) < GEN3_SAVE_SIZE) return Gen3CartSdFull;
    void* in = st->open(st->ctx, c->backup_name, Gen3StoreRead);
    if(!in) return Gen3CartSdError;
    void* out = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreCreate);
    if(!out) {
        st->close(st->ctx, in);
        return Gen3CartSdError;
    }
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t s = 0; s < GEN3_SAVE_SECTORS && r == Gen3CartOk; s++) {
        const uint32_t at = s * GEN3_SAVE_SECTOR_SIZE;
        if(!st->read(st->ctx, in, at, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = Gen3CartSdError;
        } else if(gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE) != c->crc[s]) {
            r = Gen3CartVerifyFailed;
        } else if(!st->write(st->ctx, out, at, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = cart_sd_fail(c, GEN3_SAVE_SECTOR_SIZE);
        }
        cart_step(c, Gen3CartStepPostBackup, s + 1u, CART_POST_STEPS);
    }
    st->close(st->ctx, in);
    if(!st->close(st->ctx, out) && r == Gen3CartOk) r = Gen3CartSdError;
    if(r != Gen3CartOk) return r;
    in = st->open(st->ctx, w->from ? w->from : GEN3_CART_WORK_NAME, Gen3StoreRead);
    if(!in) return Gen3CartSdError;
    out = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreUpdate);
    if(!out) {
        st->close(st->ctx, in);
        return Gen3CartSdError;
    }
    for(uint8_t k = 0; k < GEN3_SAVE_SLOT_SECTORS && r == Gen3CartOk; k++) {
        r = cart_image(c, w, in, k);
        if(r == Gen3CartOk && gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE) != c->exp_crc[k]) {
            r = Gen3CartVerifyFailed;
        }
        if(r == Gen3CartOk && !st->write(
                                  st->ctx,
                                  out,
                                  (uint32_t)w->plan.dst[k] * GEN3_SAVE_SECTOR_SIZE,
                                  c->buf,
                                  GEN3_SAVE_SECTOR_SIZE)) {
            r = cart_sd_fail(c, GEN3_SAVE_SECTOR_SIZE);
        }
    }
    st->close(st->ctx, in);
    if(!st->close(st->ctx, out) && r == Gen3CartOk) r = Gen3CartSdError;
    return r;
}

/* Step 10 on the cart: SECTOR_CRC32 of all 32 sectors, against what the new backup holds */
static Gen3CartResult cart_post_cart(Gen3Cart* c, const CartWrite* w) {
    for(uint8_t s = 0; s < GEN3_SAVE_SECTORS; s++) {
        uint32_t crc = 0;
        const Gen3CartResult r = cart_read(c, gen3_hcli_sector_crc32(c->cli, s, &crc));
        if(r != Gen3CartOk) return r;
        if(crc != cart_now_crc(c, w, s)) return Gen3CartVerifyFailed;
        cart_step(c, Gen3CartStepPostBackup, GEN3_SAVE_SECTORS + s + 1u, CART_POST_STEPS);
    }
    return Gen3CartOk;
}

/* Step 10 on the card: it gives the new backup back as composed. The new sectors' CRCs and infos
 * become the backup's; the rest are the same bytes. */
static Gen3CartResult cart_post_read_back(Gen3Cart* c, const CartWrite* w) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, GEN3_CART_BACKUP_TMP, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t s = 0; s < GEN3_SAVE_SECTORS && r == Gen3CartOk; s++) {
        if(!st->read(st->ctx, f, s * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = Gen3CartSdError;
            break;
        }
        const uint32_t crc = gen3_crc32(c->buf, GEN3_SAVE_SECTOR_SIZE);
        if(crc != cart_now_crc(c, w, s)) {
            r = Gen3CartVerifyFailed;
        } else if(cart_image_at(w, s) != CART_NO_IMAGE) {
            c->crc[s] = crc;
            gen3_save_sector_info(c->id.game, c->buf, &c->infos[s]);
        }
        cart_step(c, Gen3CartStepPostBackup, 2u * GEN3_SAVE_SECTORS + s + 1u, CART_POST_STEPS);
    }
    st->close(st->ctx, f);
    return r;
}

/* Step 10: the new backup, made as gen3_cart_backup makes one, from the old backup and
 * the images; it is checked on the cart and on the card, analyzed, kept as backups/<code>_<tid>_
 * <stamp>.sav, and the working copy is built from it. A lost link before it is kept leaves the
 * write to finish: its next run finds the slot written. Any other failure leaves the cart written
 * and no current backup. */
static Gen3CartResult cart_post_backup(Gen3Cart* c, const CartWrite* w, const char* stamp) {
    const Gen3Store* st = &c->store;
    Gen3CartResult r = cart_post_compose(c, w);
    if(r == Gen3CartOk) r = cart_post_cart(c, w);
    if(r == Gen3CartLinkLost || r == Gen3CartBusyTimeout) {
        st->remove(st->ctx, GEN3_CART_BACKUP_TMP);
        return r;
    }
    if(r == Gen3CartOk) r = cart_post_read_back(c, w);
    if(r == Gen3CartOk) {
        c->has_trainer = false;
        r = cart_backup_analyze(c);
    }
    /* Once kept, the .sav stays even if the working copy then fails: it is the cart as written,
     * checked */
    if(r == Gen3CartOk) r = cart_backup_keep(c, stamp);
    if(r == Gen3CartOk) r = cart_build_work(c);
    if(r == Gen3CartOk) {
        c->wslot = GEN3_SAVE_NO_SECTOR;
        /* Done, and the result says to power-cycle the GBA to play, which ends the helper: no job
         * talks to it again before a reconnect, so none fails on a link that went with it */
        c->linked = false;
        return Gen3CartOk;
    }
    st->remove(st->ctx, GEN3_CART_BACKUP_TMP);
    cart_drop_backup(c);
    /* The edits are on the cart now; a restore's working copy holds the save before it */
    c->dirty = 0;
    memset(c->changed, 0, sizeof(c->changed));
    if(w->from) c->work_ok = false;
    return Gen3CartWrittenNoBackup;
}

/* From here until it ends with a new backup, WrittenNoBackup, Changed or a discard, the write may
 * have changed its slot: wslot, and all that a reconnect or attach needs to settle it, the CRCs
 * its step 3 found included */
static void cart_pend(Gen3Cart* c, const CartWrite* w, const char* stamp) {
    c->wslot = w->plan.target;
    c->wcounter = w->plan.counter;
    c->wplan = w->plan;
    c->wmode = w->mode;
    snprintf(c->wfrom, sizeof(c->wfrom), "%s", w->from ? w->from : "");
    memcpy(c->wcrc, c->exp_crc, sizeof(c->wcrc));
    snprintf(c->wstamp, sizeof(c->wstamp), "%s", stamp);
}

/* Steps 3-10 of gen3_cart_write, for a write or a restore */
static Gen3CartResult cart_write_slot(Gen3Cart* c, const CartWrite* w, const char* stamp) {
    Gen3CartResult r = cart_prepare(c, w);
    if(r == Gen3CartOk) r = cart_footer_view(c);
    if(r != Gen3CartOk) return r;
    if(cart_view_fresh(c, w)) {
        r = cart_kept_same(c, w);
        if(r == Gen3CartOk) {
            cart_step(c, Gen3CartStepPrepare, CART_SLOT_STEPS, CART_SLOT_STEPS);
            cart_pend(c, w, stamp);
            r = cart_flash(c, w);
            if(r == Gen3CartOk) r = cart_verify(c, w);
            if(r != Gen3CartOk) {
                /* Slot S is untouched, so the backup stays current. The arming ends, if the link
                 * still lets it; its result is no matter. */
                if(c->linked) cart_read(c, gen3_hcli_disarm(c->cli));
                return r;
            }
        }
    } else if(cart_view_written(c, w)) {
        /* A run that a lost link interrupted after its last commit, which no reconnect has
         * settled yet: these are step 9's checks, the footers first. Nothing is written again.
         * The slot is this run's only once each sector there holds its image: until then a write
         * still pending keeps its own record, so that other images (a restore of another file)
         * whose check is cut short leave the next reconnect to settle that write by its own. */
        bool same = false;
        r = cart_dst_same(c, w, &same);
        if(r == Gen3CartOk && !same) r = Gen3CartChanged;
        if(r == Gen3CartOk) cart_pend(c, w, stamp);
    } else {
        r = Gen3CartChanged;
    }
    if(r == Gen3CartChanged) cart_drop_backup(c);
    if(r != Gen3CartOk) return r;
    return cart_post_backup(c, w, stamp);
}

/* A write or restore an earlier run left pending (wslot), once a connect found the cart as backed
 * up but for its slot (cart_still_backed_up: seen holds the footers). Only reads, until it is
 * found done:
 * - the game loads its slot with its counter, and each sector there holds the image it put there
 *   (SECTOR_CRC32 against wcrc): it got past its last commit. Its new backup is made
 *   (cart_post_backup) from the same images, which its source still gives (no Pokemon is put
 *   while it is pending, and a restore's file is never written), and it is finished;
 * - the game loads its slot with its counter, and other data there: the game's own next save,
 *   after a power cut. Changed: the backup and the edits go;
 * - else it is still pending: cut short before its last commit, or a game save there torn, neither
 *   of which the game loads. The next Write redoes it. */
static Gen3CartResult cart_resolve(Gen3Cart* c) {
    if(!gen3_cart_pending(c)) return Gen3CartOk;
    const CartWrite w = {
        .plan = c->wplan,
        .mode = c->wmode,
        .from = c->wfrom[0] ? c->wfrom : NULL,
    };
    const Gen3SaveSlots* v = &c->seen_slots;
    if(!cart_slots_load(v) || v->active != w.plan.target || v->save_counter != w.plan.counter) {
        return Gen3CartOk;
    }
    memcpy(c->exp_crc, c->wcrc, sizeof(c->exp_crc));
    bool same = false;
    Gen3CartResult r = v->strict_ok ? cart_dst_same(c, &w, &same) : Gen3CartOk;
    if(r != Gen3CartOk) return r;
    if(!same) {
        cart_drop_backup(c);
        cart_drop_edits(c);
        return Gen3CartChanged;
    }
    r = cart_post_backup(c, &w, c->wstamp);
    if(r == Gen3CartOk || r == Gen3CartWrittenNoBackup) {
        c->finished = w.from ? Gen3CartFinishedRestore : Gen3CartFinishedWrite;
    }
    return r;
}

Gen3CartResult gen3_cart_write(Gen3Cart* c, const char* stamp) {
    if(!gen3_cart_writable(c, NULL)) return Gen3CartRefused;
    if(c->dirty == 0) return Gen3CartNothing;
    if(!stamp || strlen(stamp) > GEN3_CART_STAMP_MAX) return Gen3CartBadArg;
    if(!c->linked) return Gen3CartLinkLost;
    const Gen3Store* st = &c->store;
    if(st->free_bytes(st->ctx) < CART_WRITE_SPACE) return Gen3CartSdFull;
    CartWrite w = {.mode = 0, .from = NULL};
    if(!gen3_save_plan_write(&c->slots, &w.plan)) return Gen3CartRefused;
    return cart_write_slot(c, &w, stamp);
}

/* The backup file's sectors 0..27 into seen, in this cart's layout, and their analysis: Ok if the
 * slot the game would load is strict */
static Gen3CartResult cart_restore_read(Gen3Cart* c, const char* name) {
    if(!name || strlen(name) >= GEN3_CART_NAME_MAX || strcmp(name, GEN3_CART_BACKUP_TMP) == 0) {
        return Gen3CartBadArg;
    }
    if(!c->id.can_browse) return Gen3CartRefused;
    const Gen3Store* st = &c->store;
    uint32_t size = 0;
    if(!st->size(st->ctx, name, &size)) return Gen3CartSdError;
    if(size != GEN3_SAVE_SIZE) return Gen3CartBadBackup;
    void* f = st->open(st->ctx, name, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    for(uint32_t s = 0; s < CART_INFOS && r == Gen3CartOk; s++) {
        if(!st->read(st->ctx, f, s * GEN3_SAVE_SECTOR_SIZE, c->buf, GEN3_SAVE_SECTOR_SIZE)) {
            r = Gen3CartSdError;
        } else {
            gen3_save_sector_info(c->id.game, c->buf, &c->seen[s]);
        }
        cart_step(c, Gen3CartStepRestoreCheck, s + 1u, CART_INFOS);
    }
    st->close(st->ctx, f);
    if(r != Gen3CartOk) return r;
    gen3_save_analyze(c->seen, &c->seen_slots);
    return c->seen_slots.strict_ok ? Gen3CartOk : Gen3CartBadBackup;
}

Gen3CartResult gen3_cart_restore_check(
    Gen3Cart* c,
    const char* name,
    Gen3SaveTrainer* trainer,
    bool* has_trainer) {
    *has_trainer = false;
    const Gen3CartResult r = cart_restore_read(c, name);
    if(r != Gen3CartOk) return r;
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, name, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    const bool ok = st->read(
        st->ctx,
        f,
        (uint32_t)c->seen_slots.phys[0] * GEN3_SAVE_SECTOR_SIZE,
        c->buf,
        GEN3_SAVE_SECTOR_SIZE);
    st->close(st->ctx, f);
    if(!ok) return Gen3CartSdError;
    gen3_save_trainer(c->buf, trainer);
    *has_trainer = true;
    return Gen3CartOk;
}

bool gen3_cart_restorable(const Gen3Cart* c) {
    if(!c->id.can_write || !c->backup_current || !c->analyzed) return false;
    return (c->slots.strict_ok && !c->slots.padded) || c->slots.result == Gen3SaveEmpty ||
           c->slots.result == Gen3SaveCorrupt;
}

Gen3CartResult gen3_cart_restore(Gen3Cart* c, const char* name, const char* stamp) {
    if(!gen3_cart_restorable(c)) return Gen3CartRefused;
    Gen3CartResult r = cart_restore_read(c, name);
    if(r == Gen3CartSdError) return r;
    if(r != Gen3CartOk) return Gen3CartRefused;
    if(!stamp || strlen(stamp) > GEN3_CART_STAMP_MAX) return Gen3CartBadArg;
    if(!c->linked) return Gen3CartLinkLost;
    const Gen3Store* st = &c->store;
    if(st->free_bytes(st->ctx) < CART_WRITE_SPACE) return Gen3CartSdFull;
    /* The file's save, taken now: the write's footer check reuses seen */
    const Gen3SaveSlots* b = &c->seen_slots;
    CartWrite w = {.from = name};
    if(c->slots.strict_ok && !c->slots.padded) {
        w.mode = 0;
        gen3_save_plan_write(&c->slots, &w.plan);
    } else if(c->slots.result == Gen3SaveEmpty || c->slots.result == Gen3SaveCorrupt) {
        w.mode = 1;
        gen3_save_plan_fresh(b->save_counter, b->first, &w.plan);
    } else {
        return Gen3CartRefused;
    }
    memcpy(w.plan.src, b->phys, sizeof(w.plan.src));
    return cart_write_slot(c, &w, stamp);
}

Gen3CartResult gen3_cart_reconnect(Gen3Cart* c) {
    const bool had = c->backup_current;
    c->finished = Gen3CartFinishedNone;
    Gen3CartResult r = gen3_cart_connect(c);
    if(r != Gen3CartOk || !had) return r;
    /* the connect found another header or flash id, and dropped the backup */
    if(!c->backup_current) return Gen3CartChanged;
    bool same = false;
    r = cart_still_backed_up(c, &same);
    if(r != Gen3CartOk) return r;
    if(same) return cart_resolve(c);
    cart_drop_backup(c);
    return Gen3CartChanged;
}

/* ---- ROM copy ---- */

uint32_t gen3_cart_rom_size(const Gen3Cart* c) {
    return c->info.rom_size;
}

void gen3_cart_rom_name(const Gen3Cart* c, char out[GEN3_CART_NAME_MAX]) {
    snprintf(out, GEN3_CART_NAME_MAX, CART_ROM_FORMAT, c->id.code, (unsigned)c->id.version);
}

/* The .part's name: the ROM's, and the CRC-32 of the cart's header bytes 0xA0..0xBF (title, code,
 * maker, version, complement) and its rom_size, as they were when the .part started. Only a cart
 * that reads the same in both resumes it: another cart with the same code and version (a hack, a
 * repro) starts its own copy instead of going on with this one. */
static void cart_part_name(const Gen3Cart* c, char out[GEN3_CART_NAME_MAX]) {
    const uint32_t rom = c->info.rom_size;
    const uint8_t size[4] = {
        (uint8_t)rom, (uint8_t)(rom >> 8), (uint8_t)(rom >> 16), (uint8_t)(rom >> 24)};
    uint32_t reg =
        gen3_crc32_update(0xFFFFFFFFu, c->info.header + CART_PART_ID_OFF, CART_PART_ID_LEN);
    reg = gen3_crc32_update(reg, size, sizeof(size));
    snprintf(
        out,
        GEN3_CART_NAME_MAX,
        CART_ROM_FORMAT ".%08lx.part",
        c->id.code,
        (unsigned)c->id.version,
        (unsigned long)~reg);
}

bool gen3_cart_rom_part(Gen3Cart* c, uint32_t* done_bytes) {
    char part[GEN3_CART_NAME_MAX];
    uint32_t size = 0;
    *done_bytes = 0;
    cart_part_name(c, part);
    if(c->info.rom_size == 0 || !c->store.size(c->store.ctx, part, &size)) return false;
    /* a .part longer than the ROM isn't this ROM's */
    if(size > c->info.rom_size) return false;
    *done_bytes = size - size % GEN3_CART_ROM_REGION;
    return *done_bytes > 0;
}

/* Cuts the .part back to len */
static bool cart_cut(Gen3Cart* c, const char* part, uint32_t len) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, part, Gen3StoreUpdate);
    if(!f) return false;
    const bool ok = st->truncate(st->ctx, f, len);
    return st->close(st->ctx, f) && ok;
}

/* The helper's ROM_CRC32 of [off, off + len), and the CRC of the same bytes of the .part as the
 * card gives them back */
static Gen3CartResult
    cart_rom_crcs(Gen3Cart* c, const char* part, uint32_t off, uint32_t len, uint32_t crc[2]) {
    cart_step(c, Gen3CartStepRomVerify, off + len, c->info.rom_size);
    const Gen3CartResult r =
        cart_read(c, gen3_hcli_rom_crc32(c->cli, CART_ROM_BASE + off, len, &crc[0]));
    if(r != Gen3CartOk) return r;
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, part, Gen3StoreRead);
    if(!f) return Gen3CartSdError;
    bool ok = true;
    uint32_t reg = 0xFFFFFFFFu;
    for(uint32_t pos = 0; pos < len && ok; pos += GEN3_CART_BUF_SIZE) {
        const uint32_t n = len - pos < GEN3_CART_BUF_SIZE ? len - pos : GEN3_CART_BUF_SIZE;
        ok = st->read(st->ctx, f, off + pos, c->buf, n);
        if(ok) reg = gen3_crc32_update(reg, c->buf, n);
    }
    st->close(st->ctx, f);
    crc[1] = ~reg;
    return ok ? Gen3CartOk : Gen3CartSdError;
}

/* One region: ROM_READs appended to the .part, then *same = its ROM_CRC32 and the card's copy
 * both equal the CRC of what came */
static Gen3CartResult
    cart_rom_region(Gen3Cart* c, const char* part, uint32_t off, uint32_t len, bool* same) {
    const Gen3Store* st = &c->store;
    void* f = st->open(st->ctx, part, Gen3StoreUpdate);
    if(!f) return Gen3CartSdError;
    Gen3CartResult r = Gen3CartOk;
    uint32_t reg = 0xFFFFFFFFu;
    for(uint32_t pos = 0; pos < len; pos += GEN3_CART_BUF_SIZE) {
        const uint32_t n = len - pos < GEN3_CART_BUF_SIZE ? len - pos : GEN3_CART_BUF_SIZE;
        if(cart_cancelled(c)) {
            r = Gen3CartCancelled;
            break;
        }
        r = cart_read(
            c, gen3_hcli_rom_read(c->cli, CART_ROM_BASE + off + pos, (uint16_t)(n / 2u), c->buf));
        if(r != Gen3CartOk) break;
        reg = gen3_crc32_update(reg, c->buf, n);
        if(!st->write(st->ctx, f, off + pos, c->buf, n)) {
            r = cart_sd_fail(c, n);
            break;
        }
        cart_step(c, Gen3CartStepRomRead, off + pos + n, c->info.rom_size);
    }
    if(!st->close(st->ctx, f) && r == Gen3CartOk) r = Gen3CartSdError;
    if(r != Gen3CartOk) return r;
    uint32_t crc[2];
    r = cart_rom_crcs(c, part, off, len, crc);
    *same = r == Gen3CartOk && crc[0] == ~reg && crc[1] == ~reg;
    return r;
}

/* The regions from start on, each read once more if it differs; the .part keeps only checked
 * ones */
static Gen3CartResult cart_rom_regions(Gen3Cart* c, const char* part, uint32_t start) {
    const uint32_t rom = c->info.rom_size;
    for(uint32_t off = start; off < rom; off += GEN3_CART_ROM_REGION) {
        const uint32_t len = rom - off < GEN3_CART_ROM_REGION ? rom - off : GEN3_CART_ROM_REGION;
        for(uint32_t attempt = 0;; attempt++) {
            bool same = false;
            const Gen3CartResult r = cart_rom_region(c, part, off, len, &same);
            if(r == Gen3CartOk && same) break;
            const bool cut = cart_cut(c, part, off);
            if(r != Gen3CartOk) return r;
            if(!cut) return Gen3CartSdError;
            if(attempt > 0) return Gen3CartVerifyFailed;
        }
    }
    return Gen3CartOk;
}

Gen3CartResult gen3_cart_rom_dump(Gen3Cart* c, bool resume) {
    const uint32_t rom = c->info.rom_size;
    if(rom == 0) return Gen3CartRefused;
    if(!c->linked) return Gen3CartLinkLost;
    const Gen3Store* st = &c->store;
    char part[GEN3_CART_NAME_MAX];
    cart_part_name(c, part);
    uint32_t start = 0;
    if(resume) {
        gen3_cart_rom_part(c, &start); /* 0 without a .part to resume */
    } else {
        /* From the start: nothing of an earlier copy is kept, and its room counts as free */
        st->remove(st->ctx, part);
    }
    if(st->free_bytes(st->ctx) < (uint64_t)(rom - start) + GEN3_CART_BUF_SIZE) {
        return Gen3CartSdFull;
    }
    void* f = st->open(st->ctx, part, start ? Gen3StoreUpdate : Gen3StoreCreate);
    if(!f) return Gen3CartSdError;
    bool ok = start == 0 || st->truncate(st->ctx, f, start);
    if(!st->close(st->ctx, f) || !ok) return Gen3CartSdError;
    if(start) {
        /* A power cut can leave the last region written but not checked: check it again */
        const uint32_t last = (start - 1u) / GEN3_CART_ROM_REGION * GEN3_CART_ROM_REGION;
        uint32_t crc[2];
        const Gen3CartResult r = cart_rom_crcs(c, part, last, start - last, crc);
        if(r != Gen3CartOk) return r;
        if(crc[0] != crc[1]) {
            if(!cart_cut(c, part, last)) return Gen3CartSdError;
            start = last;
        }
    }
    const Gen3CartResult r = cart_rom_regions(c, part, start);
    if(r != Gen3CartOk) return r;
    char name[GEN3_CART_NAME_MAX];
    gen3_cart_rom_name(c, name);
    st->remove(st->ctx, name);
    return st->rename(st->ctx, part, name) ? Gen3CartOk : Gen3CartSdError;
}
