#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_save.h>

/* What the Cartridge app's screens show and offer, decided from the session's status and the
 * cart: the progress screen, the hub's items, Cart info, the result after a job, the leave
 * warning, the party and box screens, and the restore and ROM copy questions. Pure C, no heap, no
 * file-scope state, so the host tests check every text fits the 128-pixel screen, which breaks
 * lines only at '\n': a line of at most 21 characters, a title or header of at most 16. */

#define GEN3_CART_SCREEN_LINE_LEN 21u
#define GEN3_CART_SCREEN_TITLE_LEN 16u
/* MbWait: how long the sender looks for the GBA before the screen says how to start it */
#define GEN3_CART_SCREEN_HINT_US 3000000u

/* What a result's Retry does */
typedef enum {
    Gen3CartRetryNone, /* no Retry: OK only */
    Gen3CartRetryConnect, /* back to the instructions, to send the helper again */
    Gen3CartRetryReconnect, /* the Reconnect job */
} Gen3CartRetry;

/* Back on the progress screen while job runs */
typedef enum {
    /* Back does nothing: Write, Restore and Discard run to their end, a cancel or not */
    Gen3CartBackIgnore,
    Gen3CartBackStop, /* gen3_cart_session_cancel, and wait for the job to end */
    /* "Stop copying?" first (Gen3CartRomStop), the ROM copy running on under it: an hours-long
     * job isn't stopped by one stray press */
    Gen3CartBackAsk,
} Gen3CartBack;
Gen3CartBack gen3_cart_screen_back(Gen3CartJob job);

/* The job ended with r and may have left the backup of another save than the one the editor's
 * trainer came from: a Connect, a Back up again or a Restore that is Ok. The editor then takes the
 * save's trainer again (gen3_cart_trainer), so a new Pokemon gets the OT of the save on the cart.
 * job: gen3_cart_screen_result_job's, so that a Reconnect that finished a restore counts. */
bool gen3_cart_screen_new_trainer(Gen3CartJob job, Gen3CartResult r);

/* The job whose result the screens show for job, which ended with r: a Connect or Reconnect that
 * found a write or restore an earlier run left cut short, done on the cart, finished it
 * (gen3_cart_finished, the status's finished): Ok or WrittenNoBackup, as that Write or Restore
 * ending so ("Written", "Power-cycle the GBA to play"). Any other: job itself. */
Gen3CartJob
    gen3_cart_screen_result_job(Gen3CartJob job, Gen3CartResult r, Gen3CartFinished finished);

/* Which status the progress screen shows */
typedef enum {
    /* the job was just handed to the session: its status may still hold the last job's step */
    Gen3CartScreenStarting,
    Gen3CartScreenRunning,
    Gen3CartScreenStopping, /* Back stopped the job, which hasn't ended yet */
} Gen3CartScreenState;

/* The progress screen's texts: static strings. line1 may be the cable warning, the one text
 * longer than 21 characters ("Cable: plug in/swap ends", as the Link Test shows it). */
typedef struct {
    const char* title;
    const char* line1;
    const char* line2;
    int8_t percent; /* -1: no bar */
    const char* hint; /* "Back: stop", "Do not power off" or "" */
} Gen3CartScreenProgress;

/* From the status of the job running (st->busy): its job, step, done and total, cable_ok and, in
 * MbWait, mb. It never reads link_up, which a Connect not yet taken by the worker leaves as the
 * last job left it. */
void gen3_cart_screen_progress(
    const Gen3CartSessionStatus* st,
    Gen3CartScreenState state,
    Gen3CartScreenProgress* out);

/* The power-on hint ("Hold START+SELECT / and power on the GBA"): Connect in MbWait with the cable
 * in, GEN3_CART_SCREEN_HINT_US without a GBA, and the sender has had no failure yet, so never
 * while it recovers from a glitch with the GBA on. */
bool gen3_cart_screen_power_hint(const Gen3CartSessionStatus* st);

/* The cart as the hub reads it, once no job runs */
typedef struct {
    bool busy; /* a job runs: the rest isn't read (the cart is the job's) */
    bool linked; /* gen3_cart_linked */
    bool backup_current;
    bool can_read_save;
    bool browsable; /* gen3_cart_browsable */
    bool writable; /* gen3_cart_writable */
    Gen3CartRefuse why; /* gen3_cart_writable's, when not writable */
    bool work_ok; /* gen3_cart_work_ok */
    bool restorable; /* gen3_cart_restorable */
    bool has_rom; /* gen3_cart_rom_size > 0 */
    uint16_t changes; /* gen3_cart_changes */
    bool pending; /* gen3_cart_pending */
} Gen3CartScreenHubIn;
/* The hub's input read from c, as the hub scene reads it: c NULL (a job runs) is busy */
void gen3_cart_screen_hub_in(const Gen3Cart* c, Gen3CartScreenHubIn* in);
/* The hub's items beyond Cart info, which is always there */
typedef struct {
    bool browse; /* "Party and boxes" */
    bool write; /* "Write changes (<n>)": linked and writable, with changes */
    /* "Writing off: <why>", which opens Cart info: not writable, for the cart's reason or the
     * save's. Not when the save isn't why: no current backup (Back up again shows), or edits with
     * no working copy (Discard rebuilds it), which why also reads as Save. An empty or corrupt
     * save, with no working copy and no edits, is why. */
    bool writing_off;
    /* "Discard changes": a current backup to build the working copy from, and changes, or a write
     * or restore pending while there is a list of Pokemon, which it keeps shut until it ends */
    bool discard;
    /* not linked: a command was lost, a connect (a Reconnect) didn't run to its end, or a write or
     * restore ended Ok, which says to power-cycle the GBA and so ends the helper */
    bool reconnect;
    bool backup; /* "Back up again": no current backup of a save the helper can read */
    /* "Restore backup" and "Copy ROM": linked, so that the cart's id and ROM size are the last
     * whole connect's and the helper still runs, and restorable or a ROM there; the hub offers
     * Reconnect otherwise */
    bool restore;
    bool rom;
} Gen3CartScreenHub;
/* While a job runs, none of them */
void gen3_cart_screen_hub(const Gen3CartScreenHubIn* in, Gen3CartScreenHub* out);

/* The result screen after job ended with r */
typedef struct {
    const char* header; /* at most 16 characters */
    const char* text; /* at most 3 lines of at most 21 characters */
    Gen3CartRetry retry;
} Gen3CartScreenResult;
/* step: the job's last step (the status's, once it ended). dropped: Connect's, the edits pending
 * from before were dropped. linked: gen3_cart_linked once it ended; a job other than Connect or
 * Reconnect that failed and left the cart unlinked offers Reconnect. A job Back stopped gets a
 * neutral header. A Write or Restore that lost the link once it may have committed (Commit, Verify
 * or PostBackup) says the cart may load either save: after the last commit the game loads the new
 * one. */
void gen3_cart_screen_result(
    Gen3CartJob job,
    Gen3CartStep step,
    Gen3CartResult r,
    bool dropped,
    bool linked,
    Gen3CartScreenResult* out);

/* Cart info: a header and 5 lines */
#define GEN3_CART_SCREEN_HEADER_SIZE (GEN3_CART_SCREEN_TITLE_LEN + 1u)
#define GEN3_CART_SCREEN_INFO_SIZE (5u * (GEN3_CART_SCREEN_LINE_LEN + 1u))
typedef struct {
    const Gen3CartId* id; /* after a connect that ran to its end */
    bool backup_current;
    const char* backup_name; /* gen3_cart_backup_name */
    const Gen3SaveSlots* slots; /* NULL: no analysis (no current backup, or not can_browse) */
    const Gen3SaveTrainer* trainer; /* NULL: no valid save */
    bool writable;
    Gen3CartRefuse why; /* when not writable (gen3_cart_writable) */
} Gen3CartScreenInfo;
/* header "<code> v<version>"; text: the flash chip, the save the game loads, the trainer, whether
 * writing is on (else why not), and the backup's stamp (MMDD-HHMMSS) */
void gen3_cart_screen_info(
    const Gen3CartScreenInfo* in,
    char header[GEN3_CART_SCREEN_HEADER_SIZE],
    char text[GEN3_CART_SCREEN_INFO_SIZE]);

/* The leave warning: "<n> changes not\nwritten." ("1 change" for one) */
#define GEN3_CART_SCREEN_LEAVE_SIZE 32u
void gen3_cart_screen_leave(uint16_t changes, char out[GEN3_CART_SCREEN_LEAVE_SIZE]);

/* ---- The party and box screens ---- */

/* The headers when a Pokemon isn't opened in the editor, or isn't put into a slot */
#define GEN3_CART_SCREEN_CANT_EDIT "Can't edit"
#define GEN3_CART_SCREEN_CANT_PUT "Can't put"
/* Why, in at most 3 lines; "" for Ok, Empty, and a value outside the enum. Pending's names only
 * the items of hub (the hub's, as it is now; NULL: none) that settle the write or end it. */
const char* gen3_cart_screen_mon_text(Gen3CartMon m, const Gen3CartScreenHub* hub);

/* A slot's row, numbered from 1 (slot 6 is "07"): "07 TREECKO Lv5", "07 EGG", "07 BAD EGG" or
 * "07 -- empty --", with '*' after it when changed. s NULL: the working copy couldn't give it
 * ("07 ?"). species: the Ok kind's name (gen3_species_name_get). */
#define GEN3_CART_SCREEN_ROW_SIZE 24u
void gen3_cart_screen_row(
    uint8_t slot,
    const Gen3MonSummary* s,
    const char* species,
    bool changed,
    char out[GEN3_CART_SCREEN_ROW_SIZE]);

/* The Put dialog: header "Put <SPECIES> Lv<n>", text "in <box>\nslot <k>?" (slot numbered from
 * 1). A header over 16 characters drops the level, which the text then starts with:
 * "Put CHARMANDER", "Lv100 in BOX 14\nslot 30?". box: at most 8 characters. */
#define GEN3_CART_SCREEN_PUT_SIZE 32u
void gen3_cart_screen_put(
    const char* species,
    uint8_t level,
    const char* box,
    uint8_t slot,
    char header[GEN3_CART_SCREEN_HEADER_SIZE],
    char text[GEN3_CART_SCREEN_PUT_SIZE]);

/* Under "Write changes?": "<n> changes. The save\nis backed up. Keep\nthe GBA on."; under
 * "Discard changes?": "<n> changes." ("1 change" for one) */
#define GEN3_CART_SCREEN_ASK_SIZE 96u
void gen3_cart_screen_write_ask(uint16_t changes, char out[GEN3_CART_SCREEN_ASK_SIZE]);
void gen3_cart_screen_discard_ask(uint16_t changes, char out[GEN3_CART_SCREEN_ASK_SIZE]);

/* gen3_check_text, its one long line broken to fit: Load failed's text */
const char* gen3_cart_screen_check_text(Gen3Check c);

/* Breaks text in place to lines of at most 21 characters, a space at a time ('\n' for ' '), after
 * dropping any leading '\n': "\nFixed: mail moves level ability" (gen3_load_changes) becomes
 * "Fixed: mail moves\nlevel ability". A word over 21 characters stays whole. */
void gen3_cart_screen_wrap(char* text);

/* What the slot router (Gen3CartSlot) holds in the editor, and what coming back from the editor
 * does with it: Existing goes back into its slot if it was edited (gen3_cart_mon_close); New
 * (made for this save) and Loaded (a .pk3) are put into the free slot once Put is confirmed
 * (gen3_cart_mon_place). None: the editor holds nothing that may go into a slot. */
typedef enum {
    Gen3CartEditNone,
    Gen3CartEditExisting,
    Gen3CartEditNew,
    Gen3CartEditLoaded,
} Gen3CartEditMode;

/* The slot router's scene state */
typedef enum {
    Gen3CartSlotOpen, /* just chosen in the list: decide */
    Gen3CartSlotMenu, /* a free slot: New Pokemon, Load .pk3 */
    Gen3CartSlotNotice, /* what loading the .pk3 fixed, before the editor */
    Gen3CartSlotEdit, /* the editor is over it: coming back closes or asks to put */
    Gen3CartSlotReedit, /* Back on Put: the editor again */
} Gen3CartSlotState;
typedef enum {
    Gen3CartSlotDoOpen, /* afresh: the mode back to None, then open the slot or offer it */
    Gen3CartSlotDoClose, /* gen3_cart_mon_close */
    Gen3CartSlotDoPut, /* Put? */
    Gen3CartSlotDoEdit, /* the editor again, on the Pokemon made or loaded */
} Gen3CartSlotDo;
/* What the router does on entering, from its state and the mode: Close, Put or Edit only for a
 * Pokemon the editor opened, made or loaded for this slot (mode not None), coming back from the
 * editor (Edit, Reedit), so that whatever the editor held before never reaches a slot */
Gen3CartSlotDo gen3_cart_screen_slot_entry(uint32_t state, Gen3CartEditMode mode);

/* ---- Restore and ROM copy ---- */

typedef enum {
    Gen3CartScreenNameOk,
    Gen3CartScreenNameOutside, /* not a file inside the folder */
    Gen3CartScreenNameTooLong, /* GEN3_CART_NAME_MAX characters or more */
} Gen3CartScreenName;
/* A file the browser picked, as the jobs name it: path past dir and its '/' ("backups/....sav").
 * Ok copies it into name; else name is left as it was. A name is never cut. */
Gen3CartScreenName
    gen3_cart_screen_restore_name(const char* path, const char* dir, char name[GEN3_CART_NAME_MAX]);
/* The text under "Can't restore" for a name that isn't Ok; "" for Ok */
const char* gen3_cart_screen_restore_name_text(Gen3CartScreenName n);
/* Under "Restore backup?": "Cart: <name> <tid>" ("Cart: no save" for NULL: none the game loads),
 * "File: <name> <tid>", "Hall of Fame is kept.", and with changes not written "<n> changes are
 * lost." ("1 change is lost."): a restore drops them */
void gen3_cart_screen_restore_ask(
    const Gen3SaveTrainer* cart,
    const Gen3SaveTrainer* file,
    uint16_t changes,
    char out[GEN3_CART_SCREEN_ASK_SIZE]);

/* A ROM copy's bytes per second (multiboot-and-save.md §6.6, stream mode): Copy ROM?'s estimate */
#define GEN3_CART_SCREEN_ROM_RATE 1850u
/* Under "Copy ROM?": "<MB> MB, about\n<h> h <m> min.\nKeep both powered.", the time at
 * GEN3_CART_SCREEN_ROM_RATE to the nearest 10 minutes ("<m> min." under an hour) */
void gen3_cart_screen_rom_ask(uint32_t rom_size, char out[GEN3_CART_SCREEN_ASK_SIZE]);
/* Under "Resume copy?": "<pct>% done" (done of rom_size, rounded down) */
void gen3_cart_screen_rom_resume(
    uint32_t done,
    uint32_t rom_size,
    char out[GEN3_CART_SCREEN_ASK_SIZE]);
/* Under "Restart copy?", which Resume copy?'s Restart asks before the .part is removed:
 * "<pct>% done is lost:\nthe copy starts\nagain from 0%." (pct as gen3_cart_screen_rom_resume) */
void gen3_cart_screen_rom_restart(
    uint32_t done,
    uint32_t rom_size,
    char out[GEN3_CART_SCREEN_ASK_SIZE]);
/* Under "ROM copied": "Verified.\n<name>" (gen3_cart_rom_name) */
void gen3_cart_screen_rom_done(const char* name, char out[GEN3_CART_SCREEN_ASK_SIZE]);
/* A ROM copy's time left, under its bar: the rate is measured from its first progress, done0
 * bytes elapsed_ms ago, to done now. "" until GEN3_CART_SCREEN_RATE_MS have passed with bytes
 * copied, and once done reaches total; else "<h> h <m> min left" ("<m> min left" under an hour),
 * rounded up to the minute, or "over 99 h left". */
#define GEN3_CART_SCREEN_RATE_MS 10000u
#define GEN3_CART_SCREEN_LEFT_SIZE 24u
void gen3_cart_screen_time_left(
    uint32_t done0,
    uint32_t done,
    uint32_t total,
    uint32_t elapsed_ms,
    char out[GEN3_CART_SCREEN_LEFT_SIZE]);
