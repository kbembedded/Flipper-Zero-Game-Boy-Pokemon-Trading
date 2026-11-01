#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_edit.h>
#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/gen3_pkm.h>
#include <src/gen3/gen3_save.h>

/* The cartridge jobs: connect, back up the save, browse and edit it in a working copy, write it
 * back, restore a backup, and copy the ROM, on top of the helper client
 * (gen3_hcli.h) and an SD store. Pure C, no heap, no file-scope state. Not ISR code: a job blocks
 * in the client's transport, so the jobs run on a worker thread with a 2 KB stack, of which the
 * store needs about 1 KB. So the big working arrays are Gen3Cart fields, and no job keeps more
 * than 256 B of locals, none over 128 B.
 *
 * The link: a job sends helper commands only after a whole gen3_cart_connect (or
 * gen3_cart_reconnect). Every LinkLost or BusyTimeout needs a new one first, since the helper may
 * still be busy with the lost command, or sending its response; until then a job answers LinkLost
 * with no link traffic. That connect keeps trying for the lost command's BUSY budget plus
 * GEN3_HCLI_SYNC_US. So does one after a write-class command answered with a response the client
 * can't take: what the helper did is then unknown, and the write's next run finds out.
 *
 * Files are on the store, named relative to its folder:
 * - backups/<code>_<tid | nosave>_<stamp>.sav: the whole 128 KB flash, all 32 sectors, as
 *   emulators and PKHeX load it. It is written as GEN3_CART_BACKUP_TMP and renamed only once the
 *   cart and the SD card both read it back the same; nothing writes or deletes a .sav after that.
 * - GEN3_CART_WORK_NAME: the working copy, the 14 sectors the game loads in id order, which the
 *   edits go into;
 * - roms/<code>_v<version>.gba, written as roms/<code>_v<version>.gba.<id>.part, where id ties the
 *   copy to the cart it started on (gen3_cart_rom_part). */

#define GEN3_CART_BUF_SIZE 4096u /* the buffer gen3_cart_init takes: one sector */
#define GEN3_CART_NAME_MAX 48u /* a store name with its terminator */
/* How long the first connect after a send keeps trying HELLO (gen3_cart_expect_boot): three
 * syncs, over twice the 3.5 s a GBA took to start the helper */
#define GEN3_CART_BOOT_US 9000000u
#define GEN3_CART_WORK_NAME "cart_work.bin"
#define GEN3_CART_BACKUP_DIR "backups"
#define GEN3_CART_ROM_DIR "roms"
/* The one temporary backup name: Create truncates it, and every failure removes it, so a power
 * cut leaves at most this file, which the next backup overwrites. */
#define GEN3_CART_BACKUP_TMP "backups/backup.tmp"
#define GEN3_CART_ROM_REGION 0x40000u /* a ROM copy is checked, and kept, in regions this long */
/* A backup's free-space check: the .sav, the working copy and a sector to spare */
#define GEN3_CART_BACKUP_SPACE (131072u + 57344u + 4096u)

typedef enum {
    Gen3StoreRead, /* an existing file, to read */
    Gen3StoreCreate, /* a new or truncated file, to read and write; its folder is made */
    Gen3StoreUpdate, /* an existing file, to read and write, kept as it is */
} Gen3StoreMode;

/* The SD card as the jobs need it. Initialise it with designated initializers. At most 2 files
 * are open at once, never one file twice. */
typedef struct {
    void* ctx;
    /* NULL on failure, or if a Read or Update file is absent */
    void* (*open)(void* ctx, const char* name, Gen3StoreMode mode);
    /* Exactly len bytes at off, all inside the file; false otherwise */
    bool (*read)(void* ctx, void* f, uint32_t off, void* buf, uint32_t len);
    /* len bytes at off (at most the file's size), the file growing as needed; false if they
     * weren't all written */
    bool (*write)(void* ctx, void* f, uint32_t off, const void* buf, uint32_t len);
    bool (*truncate)(void* ctx, void* f, uint32_t len); /* to len, at most its size */
    bool (*close)(void* ctx, void* f); /* false: the flush failed; f is closed either way */
    bool (*remove)(void* ctx, const char* name); /* false if absent */
    bool (*rename)(void* ctx, const char* from, const char* to); /* false if `to` exists */
    bool (*size)(void* ctx, const char* name, uint32_t* size); /* false if absent */
    uint64_t (*free_bytes)(void* ctx);
} Gen3Store;

typedef enum {
    Gen3CartStepNone,
    Gen3CartStepMbWait,
    Gen3CartStepMbSend,
    Gen3CartStepIdentify,
    Gen3CartStepBackupRead,
    Gen3CartStepBackupVerify,
    Gen3CartStepAnalyze,
    Gen3CartStepPrepare,
    Gen3CartStepErase,
    Gen3CartStepProgram,
    Gen3CartStepCommit,
    Gen3CartStepVerify,
    Gen3CartStepPostBackup,
    Gen3CartStepRestoreCheck,
    Gen3CartStepRomRead,
    Gen3CartStepRomVerify,
} Gen3CartStep;

/* How a job reports, from its own thread. Initialise it with designated initializers. cb may be
 * NULL; so may cancel, which a job reads between steps (each job says where). */
typedef struct {
    void (*cb)(void* ctx, Gen3CartStep step, uint32_t done, uint32_t total);
    void* ctx;
    volatile bool* cancel;
} Gen3CartProgress;

typedef enum {
    Gen3CartOk,
    Gen3CartWrittenNoBackup,
    Gen3CartNothing,
    Gen3CartCancelled,
    Gen3CartLinkLost,
    Gen3CartBusyTimeout,
    Gen3CartHelperError,
    Gen3CartRefused,
    Gen3CartChanged,
    Gen3CartSdFull,
    Gen3CartSdError,
    Gen3CartVerifyFailed,
    Gen3CartFlashError,
    Gen3CartBadBackup,
    Gen3CartBadArg,
    Gen3CartNoHelper,
    Gen3CartMultibootFailed,
    Gen3CartLinkBusy,
} Gen3CartResult;

/* At most 3 lines of at most 21 characters, broken with '\n'; "" for a value outside the enum */
const char* gen3_cart_result_text(Gen3CartResult r);

/* Pokemon slots of a save: the 6 of the party, then the 420 of the boxes */
#define GEN3_CART_MON_SLOTS (GEN3_SAVE_PARTY_MAX + GEN3_SAVE_BOXES * GEN3_SAVE_BOX_SLOTS)

/* A write or restore an earlier run left unfinished, which the last gen3_cart_attach or
 * gen3_cart_reconnect found done on the cart and finished: its new backup made (Ok), or tried
 * (WrittenNoBackup) */
typedef enum {
    Gen3CartFinishedNone,
    Gen3CartFinishedWrite,
    Gen3CartFinishedRestore,
} Gen3CartFinished;

/* The longest backup stamp a job takes (YYYYMMDD-HHMMSS is 15) */
#define GEN3_CART_STAMP_MAX 20u

typedef struct Gen3Cart Gen3Cart;

/* Fields are private to gen3_cart.c: read them through the functions below. About 2.5 KB: a field
 * of the session's state, or heap, never a stack local. */
struct Gen3Cart {
    Gen3Hcli* cli;
    Gen3Store store;
    uint8_t* buf; /* GEN3_CART_BUF_SIZE bytes */
    Gen3CartProgress progress;
    /* The link */
    bool linked; /* a connect ran to its end, and no command was lost since */
    uint32_t relink_us; /* how long the next connect keeps trying */
    Gen3HHello hello;
    /* The cart, as the last connect found it */
    Gen3HCartInfo info;
    Gen3CartId id; /* id.why: cart_why, or Save from the backup's analysis */
    Gen3CartRefuse cart_why; /* the identification's own */
    /* The backup */
    bool backup_current; /* the .sav and its analysis are the cart's save as it is */
    uint8_t header[0xC0]; /* the header of the cart it came from */
    uint16_t flash_id; /* and that cart's flash id */
    char backup_name[GEN3_CART_NAME_MAX];
    uint32_t crc[GEN3_SAVE_SECTORS]; /* each sector's CRC-32 (gen3_crc32) */
    Gen3SaveSectorInfo infos[GEN3_SAVE_SLOT_SECTORS * 2u]; /* sectors 0..27 in the cart's layout */
    bool analyzed; /* slots holds the analysis of infos (can_browse) */
    Gen3SaveSlots slots;
    bool has_trainer;
    Gen3SaveTrainer trainer;
    /* The working copy */
    bool work_ok; /* it holds the slot the game loads, plus exactly the edits recorded below */
    uint16_t dirty; /* bit k: section id k was edited */
    uint8_t changed[(GEN3_CART_MON_SLOTS + 7u) / 8u]; /* bit per Pokemon slot */
    /* A write or restore that armed (or found its slot already written) and hasn't ended with a
     * new backup or a discard: the slot it may have changed since the backup, GEN3_SAVE_NO_SECTOR
     * for none, and its counter. Reconnect and attach take a sector there that has the signature
     * only if it is the backup's or has that counter, and then find out whether it is done. Until
     * then no Pokemon is opened or put, so the edits it writes stay the ones it was given. */
    uint8_t wslot;
    uint32_t wcounter;
    /* ... its plan (wslot is plan.target, wcounter plan.counter), ARM_WRITE's mode, its source
     * ("" for the working copy, else a restore's backup file), the CRC-32 of each image it puts on
     * the cart (exp_crc as it armed: a later job's step 3 may change exp_crc, never these), and
     * its stamp, which the backup that finishes it is named with */
    Gen3SaveWritePlan wplan;
    uint8_t wmode;
    char wfrom[GEN3_CART_NAME_MAX];
    uint32_t wcrc[GEN3_SAVE_SLOT_SECTORS];
    char wstamp[GEN3_CART_STAMP_MAX + 1u];
    Gen3CartFinished finished; /* the last attach's or reconnect's */
    /* The jobs' scratch */
    Gen3HFooter footers[GEN3_SAVE_SECTORS];
    /* Sectors 0..27 as READ_FOOTERS shows them, or as a restore's backup file holds them, and
     * their analysis */
    Gen3SaveSectorInfo seen[GEN3_SAVE_SLOT_SECTORS * 2u];
    Gen3SaveSlots seen_slots;
    uint32_t exp_crc[GEN3_SAVE_SLOT_SECTORS]; /* the CRC-32 of each image a write puts on the cart */
};

/* No cart and no backup yet, and no link: gen3_cart_connect comes first. cli was just
 * gen3_hcli_init'ed; store and *p are copied (p may be NULL); buf is GEN3_CART_BUF_SIZE bytes,
 * kept. */
void gen3_cart_init(
    Gen3Cart* c,
    Gen3Hcli* cli,
    const Gen3Store* store,
    uint8_t* buf,
    const Gen3CartProgress* p);

/* HELLO (tried again for as long as a lost command may keep the helper busy), CART_INFO,
 * SAVE_PROBE, then gen3_cart_identify. A cart with another header or flash id than the backup's
 * makes the backup not current. Only a connect that ran to its end links the cart: after a cancel
 * or an error midway, no job runs until the next one. Progress Identify; cancel between
 * commands. */
Gen3CartResult gen3_cart_connect(Gen3Cart* c);
/* The helper was just sent: the next gen3_cart_connect keeps trying HELLO for at least
 * GEN3_CART_BOOT_US. After the last multiboot word a GBA takes a few seconds before it starts the
 * helper (3.5 s on the first one measured, 2026-10-31), longer than one sync. */
void gen3_cart_expect_boot(Gen3Cart* c);
/* After gen3_cart_connect: keeps the current backup and working copy, edits included, if this
 * cart's header and flash id (the connect checked them) and its footers (READ_FOOTERS) still match
 * them, except that the slot of a write or restore not yet backed up may also hold sectors with no
 * signature and sectors with that write's counter (its own, or a game save after a power cut);
 * otherwise runs gen3_cart_backup, and on Ok sets *dropped = (changes were pending). Any other
 * save in that slot (another cart's, say) gets a backup first.
 * Such a write is then settled before anything else runs, by reading only (gen3_cart_finished):
 * - the game loads its slot, with its counter, and each sector there holds the image the write
 *   put there (SECTOR_CRC32): it got past its last commit, and is finished: its new backup is made
 *   as gen3_cart_write's (Ok or WrittenNoBackup), named with its own stamp; the edits are on the
 *   cart;
 * - the game loads its slot with its counter and other data: the game saved there after a power
 *   cut. The backup and the edits are dropped, and a new backup is made, as above (*dropped);
 * - else (cut short before its last commit, or a game save there torn): it stays pending, with its
 *   edits, and the next gen3_cart_write (or restore) redoes it.
 * A cart whose save can't be read (not can_read_save) is Ok with no backup: it has its info and,
 * when rom_size > 0, its ROM copy, while browsing, writing and restoring all need can_read_save;
 * edits pending from the cart before are dropped. */
Gen3CartResult gen3_cart_attach(Gen3Cart* c, const char* stamp, bool* dropped);
/* Reads all 32 sectors to a new .sav, verified three ways: the packet CRC, the cart's
 * SECTOR_CRC32 of every sector, and an SD read-back. Then analyzes it and, if browsable, rebuilds
 * the working copy and reads that back too; the edits are gone. Refused unless can_read_save;
 * SdFull before any link traffic unless GEN3_CART_BACKUP_SPACE is free. Any failure leaves no
 * current backup, and removes every file it made but a .sav already kept: a working copy that
 * fails after it leaves that file, verified, in backups/. stamp: YYYYMMDD-HHMMSS (at most
 * GEN3_CART_STAMP_MAX characters).
 * Progress BackupRead per sector, then BackupVerify and Analyze; cancel after each sector. */
Gen3CartResult gen3_cart_backup(Gen3Cart* c, const char* stamp);
/* Rebuilds the working copy from the backup and reads it back: the edits are gone. So is the claim
 * of a write of them not yet backed up on its slot; the write itself isn't undone, and if it
 * finished the game loads it. So while linked, after such a write, the footers are read again
 * (READ_FOOTERS), with no slot skipped: if the cart isn't as backed up, the backup is dropped too,
 * and the edits with it (no working copy), and the result is Changed; the hub then backs it up
 * again. Not linked, the next attach or reconnect compares that slot instead. On another failure
 * there is no working copy, and the edits stay counted, until a discard works. BadArg without a
 * current backup of a save the game loads. */
Gen3CartResult gen3_cart_discard(Gen3Cart* c);

/* Writes the edits back (multiboot-and-save.md §5.8): the working copy's 14 sectors,
 * each with the counter N + 1 and an edited one with its checksum, go to the slot the game doesn't
 * load, edited ones uploaded and the rest copied on the cart; only the last step, a commit byte per
 * sector, makes that slot whole, and so the one the game loads. Then each of them is checked on the
 * cart, and a new backup is made of the cart as it now is (the .sav, with the 14 new sectors in
 * place of the old ones, checked against the cart and read back), from which the working copy is
 * built again.
 * - Before any link traffic: Refused unless gen3_cart_writable; Nothing without an edit; SdFull
 *   unless a backup and a sector fit on the card.
 * - Before arming, the cart must still hold what the backup says: the footers of the slot kept, the
 *   game's choice, and SECTOR_CRC32 of each sector kept; and the slot to write may hold only what
 *   the backup has there, sectors with no signature, or sectors with the write's counter N + 1;
 *   else Changed, which drops the backup. If the game loads that slot with N + 1 and each of its
 *   sectors holds this write's image (SECTOR_CRC32), a run a lost link or a power cut interrupted
 *   after its last commit is found written, and not redone; a whole save there with N + 1 and
 *   other data, as the game's own next save, is Changed. A torn one (some of its sectors with
 *   N + 1, which the game doesn't load) is written over, which is harmless: only a whole one is
 *   Changed.
 * - A failure from the arming to the check ends any arming (its result ignored). The backup stays
 *   current: the slot it holds was never written. Until the last commit the game still loads that
 *   slot; after it (a lost link in the check, or a check that failed with every commit done) it
 *   may load the new one, which the next reconnect or attach finds written, or else Changed.
 *   From the arming until the write ends Ok, WrittenNoBackup or Changed, or a discard, it is
 *   pending: no Pokemon is opened or put.
 * - Ok: written, checked and backed up; the edits are gone, now on the cart. The result says to
 *   power-cycle the GBA to play, which ends the helper, so the cart is no longer linked: a job
 *   that needs the helper waits for a reconnect.
 * - WrittenNoBackup: written and checked, but the new backup failed. The edits are gone, now on the
 *   cart, and there is no current backup until gen3_cart_backup.
 * - LinkLost or BusyTimeout, at any step: gen3_cart_reconnect (or, after the GBA lost power,
 *   gen3_cart_connect and gen3_cart_attach) finishes the job if it got past its last commit, and
 *   else this again does.
 * Never cancelled. Progress Prepare, Erase, Program, Commit, Verify, PostBackup. */
Gen3CartResult gen3_cart_write(Gen3Cart* c, const char* stamp);
/* Reads a backup file and checks it: 131072 bytes, strict with this cart's layout. Fills the
 * backup's trainer, from the sector the game would load as id 0. BadArg for a name that can't be
 * a backup (too long, or the temporary one); Refused for a cart with no known layout (not
 * can_browse); SdError if the file can't be read; BadBackup otherwise. Bytes past a section
 * needn't be zero: a restore clears them. Progress RestoreCheck per sector. */
Gen3CartResult gen3_cart_restore_check(
    Gen3Cart* c,
    const char* name,
    Gen3SaveTrainer* trainer,
    bool* has_trainer);
/* Puts the backup's save on the cart by gen3_cart_write's path, every sector uploaded
 * from the file, each with the bytes past its section zeroed and the plan's counter; the file
 * itself is never written. A cart whose save is strict and not padded keeps it as the fallback:
 * the backup's goes to the slot the game doesn't load, with the counter N + 1. A cart with no save
 * the game loads (Empty or Corrupt) gets the backup's own counter and rotation. Refused unless
 * gen3_cart_restorable, and then unless gen3_cart_restore_check is Ok (an SdError stays one); then
 * as gen3_cart_write. Sectors 28..31 are never written. Ok drops the edits not written: the
 * working copy is built again from the new backup. */
Gen3CartResult gen3_cart_restore(Gen3Cart* c, const char* name, const char* stamp);
/* gen3_cart_restore's own condition, before the file: can_write (not writable: a save the Flipper
 * won't write over may still be restored over), a current backup, and on it either a save to keep
 * (strict, not padded) or none the game loads (Empty or Corrupt). A save the game loads that is
 * padded or not strict is Refused: the slot it is in is the one a restore keeps, and the helper's
 * full-length check fails a padded kept slot; and a slot that isn't strict has no counter the game
 * is sure to beat. */
bool gen3_cart_restorable(const Gen3Cart* c);
/* After LinkLost or BusyTimeout: gen3_cart_connect (HELLO tried for the lost command's budget,
 * CART_INFO, SAVE_PROBE); then, if there was a current backup, the cart must still be its cart
 * (header and flash id) with the same footers (READ_FOOTERS), the slot of a write or restore not
 * yet backed up taken as gen3_cart_attach takes it; else Changed, which drops the backup, and the
 * edits stay counted. Such a write is then settled as gen3_cart_attach settles it, except that a
 * game save in its slot is Changed, with the backup and the edits dropped, and no new backup.
 * Progress Identify; cancel as gen3_cart_connect. */
Gen3CartResult gen3_cart_reconnect(Gen3Cart* c);
/* The write or restore the last gen3_cart_attach or gen3_cart_reconnect finished, if any */
Gen3CartFinished gen3_cart_finished(const Gen3Cart* c);
/* A write or restore armed and hasn't ended (wslot): no Pokemon is opened or put until the next
 * reconnect, attach or write settles it, or a discard ends it */
bool gen3_cart_pending(const Gen3Cart* c);

/* A connect ran to its end, and no command was lost since (nor answered with a response the
 * client couldn't take), nor did a write or restore end Ok since (the GBA is then to be
 * power-cycled): jobs may talk to the helper. False: a reconnect first. */
bool gen3_cart_linked(const Gen3Cart* c);
/* The cart as the last connect found it. Read it, gen3_cart_rom_size and gen3_cart_rom_name only
 * after a connect that ran to its end: one cut short may have read another cart's CART_INFO, and
 * not yet its id. */
const Gen3CartId* gen3_cart_id(const Gen3Cart* c);
/* The backup's analysis, while gen3_cart_backup_current and the cart can_browse */
const Gen3SaveSlots* gen3_cart_slots(const Gen3Cart* c);
bool gen3_cart_trainer(const Gen3Cart* c, Gen3SaveTrainer* out); /* false: no valid save */
const char* gen3_cart_backup_name(const Gen3Cart* c); /* "" without a current backup */
bool gen3_cart_backup_current(const Gen3Cart* c);
/* can_browse, a current backup whose save has a slot the game loads (all 14 ids), and a working
 * copy holding it */
bool gen3_cart_browsable(const Gen3Cart* c);
/* can_write, and a current backup whose save is strict_ok and not padded, with its working copy.
 * When false, *why (why may be NULL) is the identification's reason, else Gen3CartRefuseSave: the
 * save as backed up isn't one to write, or there is no current backup of it, or no working copy
 * (gen3_cart_work_ok). */
bool gen3_cart_writable(const Gen3Cart* c, Gen3CartRefuse* why);
/* The working copy holds the backup's loaded slot plus exactly the edits counted. False after a
 * store failure while editing (until gen3_cart_discard), or with no save to browse. */
bool gen3_cart_work_ok(const Gen3Cart* c);

/* The working copy, while browsable; box GEN3_SAVE_PARTY is the party. Each call reads or writes
 * the store. */
uint8_t gen3_cart_party_count(Gen3Cart* c); /* at most 6; 0 if not browsable */
/* A box's name, decoded; false (out "") if not browsable or box > 13 */
bool gen3_cart_box_name(Gen3Cart* c, uint8_t box, char out[9]);
/* The slot's game-form bytes, *len 100 (party) or 80 (box). BadArg if not browsable or out of
 * range. */
Gen3CartResult gen3_cart_mon_get(
    Gen3Cart* c,
    uint8_t box,
    uint8_t slot,
    uint8_t out[GEN3_PARTY_SIZE],
    size_t* len);
/* Encrypts p into the slot (gen3_save_pkm_to_mon) and marks its sections dirty and the slot
 * changed. A party slot equal to the count appends: the count goes up by one. BadArg if not
 * browsable, out of range, or a party slot over the count; Refused while a write is pending
 * (gen3_cart_pending), with nothing written or marked. The caller checks p
 * (gen3_trade_offer_check) first. A store failure leaves the working copy untrusted: not browsable
 * until gen3_cart_discard. The slot counts as changed either way, so that Discard and the warning
 * on leaving show. */
Gen3CartResult gen3_cart_mon_put(Gen3Cart* c, uint8_t box, uint8_t slot, const Gen3Pkm* p);
bool gen3_cart_mon_changed(const Gen3Cart* c, uint8_t box, uint8_t slot);
uint16_t gen3_cart_changes(const Gen3Cart* c); /* Pokemon slots changed */

/* Opening a slot's Pokemon in the editor, and putting the editor's Pokemon into a slot: the only
 * ways the Cartridge app changes a Pokemon in the save. Nothing goes in that the game would not
 * take back as it was, or that the Flipper can't write back whole: the offer rules of a trade to
 * Ruby (gen3_trade_offer_check) hold for every Pokemon put, as gen3_pkm_check does.
 * gen3_cart_mon_open, _close and _place are for the app's thread only, while no job runs: they
 * read and write the working copy, and the editor is the app's. */
typedef enum {
    Gen3CartMonOk,
    Gen3CartMonEmpty, /* open: no Pokemon there; a new one may be put */
    Gen3CartMonBadArg, /* not browsable, or no such slot */
    Gen3CartMonSdError, /* the working copy couldn't be read or written */
    Gen3CartMonTaken, /* put: the slot isn't free */
    /* The offer rules, and gen3_pkm_check: */
    Gen3CartMonMail, /* a Mail item or a mail index: its letter is a record of its own */
    Gen3CartMonItem, /* an item Ruby has no entry for (the Enigma Berry, or one past its table) */
    Gen3CartMonEgg,
    Gen3CartMonBadEgg, /* as the game shows it: a checksum that fails, the flag, no species */
    Gen3CartMonBadData, /* a species, move or item out of range */
    Gen3CartMonNoData, /* the editor needed the bulk file (gen3_data.h), which can't be read */
    /* a write or restore is pending (gen3_cart_pending): it is settled first, so that the edits it
     * writes are never mixed with new ones */
    Gen3CartMonPending,
} Gen3CartMon;

/* Opens slot (box, slot) of the working copy in the editor, to edit it in place. In order: a
 * slot that isn't one (BadArg); a write pending (Pending), for an empty slot too, as nothing may
 * be put there either; a party slot past the last one is Empty if it is the next one, else
 * BadArg; the slot's bytes (SdError); none there -> Empty; a Bad Egg, an egg, then the offer rules
 * and gen3_pkm_check ->
 * their reason; then gen3_edit_load, which normalises it (party stats recalculated, moves packed)
 * -> NoData if the bulk file can't be read, else BadData. Ok: the editor holds it, and orig its
 * normalised bytes as the slot keeps them (*orig_len 100 for the party, 80 for a box), which
 * gen3_cart_mon_close compares with: a Pokemon opened and closed unchanged is never put. Anything
 * else leaves the editor as it was, and orig undefined. scratch: the caller's (not a stack local). */
Gen3CartMon gen3_cart_mon_open(
    Gen3Cart* c,
    Gen3Editor* ed,
    uint8_t box,
    uint8_t slot,
    uint8_t orig[GEN3_PARTY_SIZE],
    size_t* orig_len,
    Gen3Pkm* scratch);
/* Puts the editor's Pokemon back into the slot gen3_cart_mon_open opened, if it was edited
 * (Pending if a write is pending then, which no job leaves while the editor is open): its
 * orig_len bytes as the slot keeps them (into mon, the caller's) differ from orig. A species
 * changed in the editor made a new Pokemon there, met in Ruby (gen3_edit_set_species): if its
 * species or its MetGame isn't orig's, its MetGame becomes this cart's game, as a new one's does.
 * Only one that gen3_pkm_check accepts and the offer rules allow is put (gen3_cart_mon_put); else
 * their reason, and nothing is put. *put: it was put. mon and scratch: the caller's (not stack
 * locals). */
Gen3CartMon gen3_cart_mon_close(
    Gen3Cart* c,
    Gen3Editor* ed,
    uint8_t box,
    uint8_t slot,
    const uint8_t* orig,
    size_t orig_len,
    uint8_t mon[GEN3_PARTY_SIZE],
    Gen3Pkm* scratch,
    bool* put);
/* Puts the editor's Pokemon into a free slot: an empty box slot, or the party slot after the last,
 * which adds it to the party; another slot is Taken, so no Pokemon is ever written over; while a
 * write is pending (gen3_cart_pending), Pending first. new_mon: it was made for this save
 * (gen3_cart_new_mon), so its MetGame is this cart's game again, which a species change sets to
 * Ruby; a loaded .pk3 keeps its own. Then gen3_pkm_check and the offer rules, as
 * gen3_cart_mon_close. mon: the caller's scratch. */
Gen3CartMon gen3_cart_mon_place(
    Gen3Cart* c,
    Gen3Editor* ed,
    uint8_t box,
    uint8_t slot,
    bool new_mon,
    uint8_t mon[GEN3_PARTY_SIZE]);

/* CART_INFO's rom_size; 0: no ROM copy. After a whole connect only, as gen3_cart_id. */
uint32_t gen3_cart_rom_size(const Gen3Cart* c);
void gen3_cart_rom_name(const Gen3Cart* c, char out[GEN3_CART_NAME_MAX]); /* "roms/AXVE_v1.gba" */
/* A .part of this ROM holds at least one region: *done_bytes, its length rounded down to a region,
 * is where a resumed copy starts. It is this cart's only if the cart's header bytes 0xA0..0xBF and
 * rom_size are the ones it started with: the .part's name holds their CRC-32, so a .part another
 * cart left (the same code and version, another header or size) isn't found, and a copy starts
 * from 0. */
bool gen3_cart_rom_part(Gen3Cart* c, uint32_t* done_bytes);
/* Copies the ROM in GEN3_CART_ROM_REGION regions: ROM_READs of 2048 halfwords
 * appended to the .part, then the region's ROM_CRC32 and an SD read-back must both equal the CRC
 * of what came; a region that differs is read once more, then VerifyFailed. The .part keeps only
 * checked regions: any failure, cancel included, cuts it back to the last one. resume starts at
 * gen3_cart_rom_part's length, after checking the last region it keeps again; a copy that doesn't
 * resume removes this cart's .part first, so its room counts as free. SdFull before any ROM_READ
 * unless rom_size - that length + 4096 is free. At the end the .part is renamed to
 * gen3_cart_rom_name's,
 * replacing an older copy. Refused when rom_size is 0, LinkLost unless linked, both before any
 * file is touched. Progress RomRead and RomVerify in bytes; cancel between ROM_READs. */
Gen3CartResult gen3_cart_rom_dump(Gen3Cart* c, bool resume);

/* ---- The jobs as the Cartridge session runs them ---- */

/* The session (gen3_cart_furi.h) runs these on its worker, and the screens (gen3_cart_screen.h)
 * show them: here, so that both see them without a furi header. */
typedef enum {
    Gen3CartJobConnect, /* send the helper (again), then gen3_cart_connect and gen3_cart_attach */
    Gen3CartJobBackup,
    Gen3CartJobWrite,
    Gen3CartJobDiscard,
    Gen3CartJobRestoreCheck,
    Gen3CartJobRestore,
    Gen3CartJobRom,
    Gen3CartJobReconnect,
} Gen3CartJob;

/* About 100 B: never a stack local in a scene; copy it into a static buffer. */
typedef struct {
    bool busy; /* from gen3_cart_session_run until the job's result is in */
    Gen3CartJob job;
    Gen3CartResult result; /* the last job's, valid when !busy */
    Gen3CartStep step;
    uint32_t done, total;
    /* false while Connect waits for the cable (no cable, or the wrong end in the Flipper); while
     * the driver runs, its cable guard's latest decision */
    bool cable_ok;
    Gen3MbStatus mb; /* Connect: the multiboot sender's, during MbWait and MbSend */
    bool link_up; /* the helper answered the last command */
    bool dropped; /* Connect: edits that were pending were dropped (gen3_cart_attach) */
    /* Connect and Reconnect: a write or restore cut short that it found done, and finished
     * (gen3_cart_finished) */
    Gen3CartFinished finished;
    Gen3SaveTrainer restore_trainer; /* RestoreCheck: the backup's trainer */
    bool restore_has_trainer;
} Gen3CartSessionStatus;
