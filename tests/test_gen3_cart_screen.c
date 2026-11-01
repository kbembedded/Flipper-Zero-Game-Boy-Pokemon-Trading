#include "test.h"
#include <stdbool.h>
#include <stdio.h>
#include <src/gen3/gen3_cart.h>
#include <src/gen3/gen3_cart_id.h>
#include <src/gen3/gen3_cart_screen.h>
#include <src/gen3/gen3_mb.h>
#include <src/gen3/gen3_pk3.h>
#include <src/gen3/gen3_save.h>

/* The Cartridge app's screen texts and choices: what the progress screen shows for each job and
 * step, the power-on hint, the hub's items, the results and their Retry, Cart info, the leave
 * warning, and the party and box screens; and that every text fits the screen. */

#define ASSERT_FALSE(x) ASSERT_TRUE(!(x))
#define CABLE_TEXT "Cable: plug in/swap ends"

static const Gen3CartJob all_jobs[] = {
    Gen3CartJobConnect,
    Gen3CartJobBackup,
    Gen3CartJobWrite,
    Gen3CartJobDiscard,
    Gen3CartJobRestoreCheck,
    Gen3CartJobRestore,
    Gen3CartJobRom,
    Gen3CartJobReconnect,
};

/* The status is about 100 B: static, as the app keeps it */
static Gen3CartSessionStatus st;

static void status(Gen3CartJob job, Gen3CartStep step, uint32_t done, uint32_t total) {
    memset(&st, 0, sizeof(st));
    st.busy = true;
    st.job = job;
    st.step = step;
    st.done = done;
    st.total = total;
    st.cable_ok = true;
}

/* Lines broken with '\n': at most max_lines, each at most max_len characters, none empty unless
 * the whole text is */
static bool fits(const char* t, unsigned max_lines, size_t max_len) {
    if(!t) return false;
    unsigned lines = 1;
    size_t len = 0;
    for(const char* p = t; *p; p++) {
        if(*p == '\n') {
            if(len == 0) return false;
            lines++;
            len = 0;
        } else if(++len > max_len) {
            return false;
        }
    }
    return lines <= max_lines && (t[0] == '\0' || len > 0);
}

static unsigned line_count(const char* t) {
    unsigned n = 1;
    for(; *t; t++) n += *t == '\n';
    return n;
}

/* Line k (0-based) of t into out */
static void line_of(const char* t, unsigned k, char out[64]) {
    for(; k && *t; t++) {
        if(*t == '\n') k--;
    }
    size_t n = 0;
    while(t[n] && t[n] != '\n' && n < 63) n++;
    memcpy(out, t, n);
    out[n] = '\0';
}

TEST(test_back) {
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobWrite), Gen3CartBackIgnore);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobRestore), Gen3CartBackIgnore);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobDiscard), Gen3CartBackIgnore);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobConnect), Gen3CartBackStop);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobBackup), Gen3CartBackStop);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobReconnect), Gen3CartBackStop);
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobRestoreCheck), Gen3CartBackStop);
    /* A ROM copy asks first, and runs on under the question */
    ASSERT_EQ(gen3_cart_screen_back(Gen3CartJobRom), Gen3CartBackAsk);
}

/* The editor's trainer is the save's: taken again after each job that may leave another save's
 * backup, so a new Pokemon gets the OT of the save now on the cart */
TEST(test_new_trainer) {
    for(size_t j = 0; j < sizeof(all_jobs) / sizeof(all_jobs[0]); j++) {
        const Gen3CartJob job = all_jobs[j];
        const bool takes = job == Gen3CartJobConnect || job == Gen3CartJobBackup ||
                           job == Gen3CartJobRestore;
        ASSERT_EQ(gen3_cart_screen_new_trainer(job, Gen3CartOk), takes);
        /* Anything else leaves no new backup, or none at all: the trainer stays */
        for(int r = Gen3CartOk + 1; r <= Gen3CartLinkBusy; r++) {
            ASSERT_FALSE(gen3_cart_screen_new_trainer(job, (Gen3CartResult)r));
        }
    }
}

TEST(test_progress_connect_steps) {
    Gen3CartScreenProgress p;

    status(Gen3CartJobConnect, Gen3CartStepMbWait, 0, 8000);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Waiting for GBA");
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
    ASSERT_EQ(p.percent, -1);
    ASSERT_STR_EQ(p.hint, "Back: stop");

    status(Gen3CartJobConnect, Gen3CartStepMbSend, 4000, 8000);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Sending helper");
    ASSERT_EQ(p.percent, 50);
    ASSERT_STR_EQ(p.hint, "Back: stop");

    status(Gen3CartJobConnect, Gen3CartStepIdentify, 1, 3);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Reading cart");
    ASSERT_EQ(p.percent, 33);

    status(Gen3CartJobConnect, Gen3CartStepBackupRead, 8, 32);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Backing up save");
    ASSERT_STR_EQ(p.line2, "about 70 s");
    ASSERT_EQ(p.percent, 25);
    ASSERT_STR_EQ(p.hint, "Back: stop");

    status(Gen3CartJobConnect, Gen3CartStepBackupVerify, 64, 64);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Checking backup");
    ASSERT_EQ(p.percent, 100);

    status(Gen3CartJobConnect, Gen3CartStepAnalyze, 0, 1);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Reading save");

    /* No total yet: no bar */
    status(Gen3CartJobConnect, Gen3CartStepMbSend, 0, 0);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_EQ(p.percent, -1);
}

TEST(test_progress_cable) {
    Gen3CartScreenProgress p;
    status(Gen3CartJobConnect, Gen3CartStepMbWait, 0, 0);
    st.cable_ok = false;
    st.mb.waiting_us = 10000000u; /* even with the hint's wait over */
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Waiting for GBA");
    ASSERT_STR_EQ(p.line1, CABLE_TEXT);
    ASSERT_STR_EQ(p.line2, "");
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));

    /* While the driver runs, its cable guard: any step */
    status(Gen3CartJobBackup, Gen3CartStepBackupRead, 3, 32);
    st.cable_ok = false;
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.line1, CABLE_TEXT);
    ASSERT_STR_EQ(p.line2, "about 70 s");

    /* Starting: the cable flag may be the last job's */
    gen3_cart_screen_progress(&st, Gen3CartScreenStarting, &p);
    ASSERT_STR_EQ(p.line1, "");
}

TEST(test_power_hint) {
    Gen3CartScreenProgress p;

    status(Gen3CartJobConnect, Gen3CartStepMbWait, 0, 8000);
    st.mb.phase = Gen3MbPhaseDetect;
    st.mb.waiting_us = GEN3_CART_SCREEN_HINT_US - 1u;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.line1, "");

    st.mb.waiting_us = GEN3_CART_SCREEN_HINT_US;
    ASSERT_TRUE(gen3_cart_screen_power_hint(&st));
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Waiting for GBA");
    ASSERT_STR_EQ(p.line1, "Hold START+SELECT");
    ASSERT_STR_EQ(p.line2, "and power on the GBA");
    ASSERT_EQ(p.percent, -1);

    /* After any failure the GBA is on: never tell it to power on */
    st.mb.restarts = 1;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
    st.mb.restarts = 0;
    st.mb.last_fail = Gen3MbFailPalette; /* an uncounted one */
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    st.mb.last_fail = Gen3MbFailNone;
    ASSERT_TRUE(gen3_cart_screen_power_hint(&st));

    /* Only while waiting, and only for Connect */
    st.step = Gen3CartStepMbSend;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    st.step = Gen3CartStepMbWait;
    st.job = Gen3CartJobReconnect;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    st.job = Gen3CartJobConnect;
    st.busy = false;
    ASSERT_FALSE(gen3_cart_screen_power_hint(&st));
    st.busy = true;

    /* Starting: a Connect not yet taken shows nothing of the sender */
    gen3_cart_screen_progress(&st, Gen3CartScreenStarting, &p);
    ASSERT_STR_EQ(p.title, "Starting");
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
}

TEST(test_progress_ignores_link_up) {
    for(size_t j = 0; j < sizeof(all_jobs) / sizeof(all_jobs[0]); j++) {
        for(int step = Gen3CartStepNone; step <= Gen3CartStepRomVerify; step++) {
            for(int state = Gen3CartScreenStarting; state <= Gen3CartScreenStopping; state++) {
                Gen3CartScreenProgress a, b;
                status(all_jobs[j], (Gen3CartStep)step, 5, 10);
                st.mb.waiting_us = 5000000u;
                st.link_up = false;
                gen3_cart_screen_progress(&st, (Gen3CartScreenState)state, &a);
                st.link_up = true;
                gen3_cart_screen_progress(&st, (Gen3CartScreenState)state, &b);
                ASSERT_TRUE(a.title == b.title && a.line1 == b.line1 && a.line2 == b.line2);
                ASSERT_TRUE(a.hint == b.hint && a.percent == b.percent);
            }
        }
    }
}

TEST(test_progress_write) {
    Gen3CartScreenProgress p;
    const Gen3CartJob jobs[] = {Gen3CartJobWrite, Gen3CartJobRestore};
    for(size_t j = 0; j < 2; j++) {
        status(jobs[j], Gen3CartStepPrepare, 15, 15);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Preparing");
        ASSERT_STR_EQ(p.hint, "");

        /* One bar for the three passes */
        status(jobs[j], Gen3CartStepErase, 7, 14);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Writing");
        ASSERT_STR_EQ(p.hint, "Do not power off");
        ASSERT_EQ(p.percent, 16);
        status(jobs[j], Gen3CartStepProgram, 0, 14);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Writing");
        ASSERT_STR_EQ(p.hint, "Do not power off");
        ASSERT_EQ(p.percent, 33);
        status(jobs[j], Gen3CartStepCommit, 14, 14);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Writing");
        ASSERT_STR_EQ(p.hint, "Do not power off");
        ASSERT_EQ(p.percent, 100);

        status(jobs[j], Gen3CartStepVerify, 3, 15);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Verifying");
        ASSERT_STR_EQ(p.hint, "Do not power off");
        ASSERT_EQ(p.percent, 20);

        status(jobs[j], Gen3CartStepPostBackup, 0, 96);
        gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
        ASSERT_STR_EQ(p.title, "Saving backup");
        ASSERT_STR_EQ(p.hint, "");
    }

    status(Gen3CartJobRestoreCheck, Gen3CartStepRestoreCheck, 14, 28);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Checking file");
    ASSERT_STR_EQ(p.hint, "Back: stop");
    ASSERT_EQ(p.percent, 50);

    status(Gen3CartJobRom, Gen3CartStepRomRead, 0x200000u, 0x1000000u);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Copying ROM");
    ASSERT_EQ(p.percent, 12);
    /* 32 MiB in bytes: no overflow */
    status(Gen3CartJobRom, Gen3CartStepRomVerify, 0x2000000u, 0x2000000u);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Checking ROM");
    ASSERT_EQ(p.percent, 100);

    status(Gen3CartJobDiscard, Gen3CartStepNone, 0, 0);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.hint, "");
    status(Gen3CartJobReconnect, Gen3CartStepIdentify, 0, 3);
    gen3_cart_screen_progress(&st, Gen3CartScreenRunning, &p);
    ASSERT_STR_EQ(p.title, "Reading cart");
    ASSERT_STR_EQ(p.hint, "Back: stop");
}

TEST(test_progress_starting_stopping) {
    Gen3CartScreenProgress p;
    /* The step is the last job's until the worker takes the new one */
    status(Gen3CartJobBackup, Gen3CartStepIdentify, 3, 3);
    gen3_cart_screen_progress(&st, Gen3CartScreenStarting, &p);
    ASSERT_STR_EQ(p.title, "Starting");
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
    ASSERT_EQ(p.percent, -1);
    ASSERT_STR_EQ(p.hint, "Back: stop");
    status(Gen3CartJobWrite, Gen3CartStepIdentify, 3, 3);
    gen3_cart_screen_progress(&st, Gen3CartScreenStarting, &p);
    ASSERT_STR_EQ(p.hint, "");

    status(Gen3CartJobConnect, Gen3CartStepBackupRead, 8, 32);
    gen3_cart_screen_progress(&st, Gen3CartScreenStopping, &p);
    ASSERT_STR_EQ(p.title, "Stopping");
    ASSERT_STR_EQ(p.line1, "");
    ASSERT_STR_EQ(p.line2, "");
    ASSERT_EQ(p.percent, -1);
    ASSERT_STR_EQ(p.hint, "");
}

TEST(test_progress_texts_fit) {
    for(size_t j = 0; j < sizeof(all_jobs) / sizeof(all_jobs[0]); j++) {
        for(int step = Gen3CartStepNone; step <= Gen3CartStepRomVerify; step++) {
            for(int state = Gen3CartScreenStarting; state <= Gen3CartScreenStopping; state++) {
                for(int k = 0; k < 4; k++) {
                    Gen3CartScreenProgress p;
                    status(all_jobs[j], (Gen3CartStep)step, 10, 10);
                    st.cable_ok = k & 1;
                    st.mb.waiting_us = (k & 2) ? GEN3_CART_SCREEN_HINT_US : 0;
                    gen3_cart_screen_progress(&st, (Gen3CartScreenState)state, &p);
                    ASSERT_TRUE(p.title[0] != '\0');
                    ASSERT_TRUE(fits(p.title, 1, GEN3_CART_SCREEN_TITLE_LEN));
                    ASSERT_TRUE(
                        strcmp(p.line1, CABLE_TEXT) == 0 ||
                        fits(p.line1, 1, GEN3_CART_SCREEN_LINE_LEN));
                    ASSERT_TRUE(fits(p.line2, 1, GEN3_CART_SCREEN_LINE_LEN));
                    ASSERT_TRUE(fits(p.hint, 1, GEN3_CART_SCREEN_LINE_LEN));
                    ASSERT_TRUE(p.percent >= -1 && p.percent <= 100);
                }
            }
        }
    }
}

/* A linked cart, backed up, browsable and writable, with no change */
static Gen3CartScreenHubIn hub_in(void) {
    return (Gen3CartScreenHubIn){
        .busy = false,
        .linked = true,
        .backup_current = true,
        .can_read_save = true,
        .browsable = true,
        .writable = true,
        .why = Gen3CartRefuseNone,
        .work_ok = true,
        .restorable = true,
        .has_rom = true,
        .changes = 0,
        .pending = false,
    };
}

static void hub(const Gen3CartScreenHubIn* in, Gen3CartScreenHub* h) {
    memset(h, 0xA5, sizeof(*h));
    gen3_cart_screen_hub(in, h);
}

TEST(test_hub) {
    Gen3CartScreenHub h;
    Gen3CartScreenHubIn in = hub_in();

    hub(&in, &h);
    ASSERT_TRUE(h.browse);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.writing_off);
    ASSERT_FALSE(h.discard);
    ASSERT_FALSE(h.reconnect);
    ASSERT_FALSE(h.backup);
    ASSERT_TRUE(h.restore);
    ASSERT_TRUE(h.rom);

    /* Changes: write them, or discard them */
    in.changes = 2;
    hub(&in, &h);
    ASSERT_TRUE(h.write);
    ASSERT_TRUE(h.discard);
    ASSERT_FALSE(h.writing_off);

    /* Not writable: why not, instead of Write, and Discard still */
    in.writable = false;
    in.why = Gen3CartRefuseSave;
    hub(&in, &h);
    ASSERT_FALSE(h.write);
    ASSERT_TRUE(h.writing_off);
    ASSERT_TRUE(h.discard);
    in.changes = 0;
    hub(&in, &h);
    ASSERT_TRUE(h.writing_off);
    ASSERT_FALSE(h.discard);

    /* Not browsable (no working copy, or no layout): no list */
    in = hub_in();
    in.browsable = false;
    hub(&in, &h);
    ASSERT_FALSE(h.browse);

    /* Not linked (a command lost, or a Reconnect stopped before its end): Reconnect. Write changes
     * waits for it, as a write needs the helper; Discard changes, on the SD card, doesn't. */
    in = hub_in();
    in.linked = false;
    hub(&in, &h);
    ASSERT_TRUE(h.reconnect);
    ASSERT_TRUE(h.browse);
    in.changes = 2;
    hub(&in, &h);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.writing_off);
    ASSERT_TRUE(h.discard);
    ASSERT_TRUE(h.reconnect);

    /* No current backup of a save the helper can read: Back up again, and "Writing off: Save not
     * recognised" doesn't show, since the save isn't why. The edits can't be discarded without a
     * backup to rebuild from; backing up drops them. */
    in = hub_in();
    in.backup_current = false;
    in.browsable = false;
    in.writable = false;
    in.why = Gen3CartRefuseSave;
    in.restorable = false;
    in.changes = 2;
    hub(&in, &h);
    ASSERT_TRUE(h.backup);
    ASSERT_FALSE(h.reconnect);
    ASSERT_FALSE(h.writing_off);
    ASSERT_FALSE(h.discard);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.restore);
    /* A cart's own reason shows all the same */
    in.why = Gen3CartRefuseEmerald;
    hub(&in, &h);
    ASSERT_TRUE(h.writing_off);
    /* A cart whose save can't be read reaches the hub with no backup: nothing to back up */
    in.can_read_save = false;
    in.why = Gen3CartRefuseSram;
    in.changes = 0;
    hub(&in, &h);
    ASSERT_FALSE(h.backup);
    ASSERT_TRUE(h.writing_off);
    ASSERT_FALSE(h.restore);
    ASSERT_TRUE(h.rom);

    /* A working copy that went (a failed put): not the save's fault; Discard rebuilds it */
    in = hub_in();
    in.browsable = false;
    in.writable = false;
    in.why = Gen3CartRefuseSave;
    in.work_ok = false;
    in.changes = 1;
    hub(&in, &h);
    ASSERT_FALSE(h.writing_off);
    ASSERT_TRUE(h.discard);
    ASSERT_FALSE(h.backup);
    /* The save backed up is the reason: not strict, padded, empty or corrupt */
    in.work_ok = true;
    hub(&in, &h);
    ASSERT_TRUE(h.writing_off);

    /* Restore and Copy ROM: only when the job layer allows them, and linked, since the cart as
     * the hub knows it is the last whole connect's */
    in = hub_in();
    in.restorable = false;
    in.has_rom = false;
    hub(&in, &h);
    ASSERT_FALSE(h.restore);
    ASSERT_FALSE(h.rom);
    in = hub_in();
    in.linked = false;
    hub(&in, &h);
    ASSERT_FALSE(h.restore);
    ASSERT_FALSE(h.rom);
    ASSERT_TRUE(h.reconnect);
    /* Restore needs only can_write and a current backup, not a save to write: an empty or corrupt
     * one, which the game doesn't load, so it has no working copy and no edits. The save is why
     * writing is off, and the hub says so. */
    in = hub_in();
    in.browsable = false;
    in.writable = false;
    in.why = Gen3CartRefuseSave;
    in.work_ok = false;
    hub(&in, &h);
    ASSERT_TRUE(h.restore);
    ASSERT_TRUE(h.writing_off);
    ASSERT_FALSE(h.discard);
    ASSERT_FALSE(h.backup);
    ASSERT_FALSE(h.browse);

    /* While a job runs, the cart isn't read: none of them */
    in = hub_in();
    in.busy = true;
    in.linked = false;
    in.backup_current = false;
    in.changes = 3;
    hub(&in, &h);
    ASSERT_FALSE(h.browse);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.writing_off);
    ASSERT_FALSE(h.discard);
    ASSERT_FALSE(h.reconnect);
    ASSERT_FALSE(h.backup);
    ASSERT_FALSE(h.restore);
    ASSERT_FALSE(h.rom);
}

TEST(test_result_retry) {
    Gen3CartScreenResult r;

    /* Connect: every failure goes back to the instructions */
    for(int res = Gen3CartLinkLost; res <= Gen3CartLinkBusy; res++) {
        gen3_cart_screen_result(
            Gen3CartJobConnect, Gen3CartStepMbWait, (Gen3CartResult)res, false, true, &r);
        ASSERT_EQ(r.retry, Gen3CartRetryConnect);
        ASSERT_STR_EQ(r.header, "Connect failed");
        ASSERT_STR_EQ(r.text, gen3_cart_result_text((Gen3CartResult)res));
    }
    gen3_cart_screen_result(Gen3CartJobConnect, Gen3CartStepAnalyze, Gen3CartOk, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);

    /* A lost link: Reconnect, and a Reconnect that fails sends the helper again */
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartLinkLost, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    ASSERT_STR_EQ(r.header, "Backup failed");
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepErase, Gen3CartBusyTimeout, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    ASSERT_STR_EQ(r.header, "Write failed");
    gen3_cart_screen_result(
        Gen3CartJobRom, Gen3CartStepRomRead, Gen3CartLinkLost, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartLinkLost, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);
    ASSERT_STR_EQ(r.header, "Reconnect failed");
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartBusyTimeout, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartHelperError, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);

    /* A Reconnect that ran to its end and found another save, or was stopped: no Retry */
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartChanged, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartCancelled, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartOk, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    ASSERT_STR_EQ(r.header, "Reconnected");
    ASSERT_STR_EQ(r.text, "Done.");

    /* Any other result: OK only */
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepVerify, Gen3CartVerifyFailed, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartSdFull, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartCancelled, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    ASSERT_STR_EQ(r.text, "Stopped.");
    gen3_cart_screen_result(Gen3CartJobBackup, Gen3CartStepAnalyze, Gen3CartOk, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    ASSERT_STR_EQ(r.header, "Backed up");
}

TEST(test_result_after_commit) {
    Gen3CartScreenResult r;
    const Gen3CartJob jobs[] = {Gen3CartJobWrite, Gen3CartJobRestore};
    const Gen3CartStep late[] = {Gen3CartStepCommit, Gen3CartStepVerify, Gen3CartStepPostBackup};
    const Gen3CartStep early[] = {Gen3CartStepPrepare, Gen3CartStepErase, Gen3CartStepProgram};
    for(size_t j = 0; j < 2; j++) {
        /* Before any commit the game still loads the old slot: the job layer's own text */
        for(size_t k = 0; k < 3; k++) {
            gen3_cart_screen_result(jobs[j], early[k], Gen3CartLinkLost, false, true, &r);
            ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartLinkLost));
            gen3_cart_screen_result(jobs[j], early[k], Gen3CartBusyTimeout, false, true, &r);
            ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartBusyTimeout));
        }
        /* After a commit it may load the new one: never claim the old one */
        for(size_t k = 0; k < 3; k++) {
            gen3_cart_screen_result(jobs[j], late[k], Gen3CartLinkLost, false, true, &r);
            ASSERT_STR_EQ(
                r.text, "Link lost. The cart\nmay load either save.\nRetry, then redo it.");
            ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
            ASSERT_TRUE(strstr(r.text, "still") == NULL);
            gen3_cart_screen_result(jobs[j], late[k], Gen3CartBusyTimeout, false, true, &r);
            ASSERT_STR_EQ(
                r.text, "No answer. The cart\nmay load either save.\nRetry, then redo it.");
            ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
            /* Other results keep their own text */
            gen3_cart_screen_result(jobs[j], late[k], Gen3CartVerifyFailed, false, true, &r);
            ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartVerifyFailed));
        }
    }
    /* Only a write can have committed */
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepVerify, Gen3CartLinkLost, false, true, &r);
    ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartLinkLost));
}

TEST(test_result_dropped) {
    Gen3CartScreenResult r;
    gen3_cart_screen_result(Gen3CartJobConnect, Gen3CartStepAnalyze, Gen3CartOk, true, true, &r);
    ASSERT_STR_EQ(r.header, "Edits dropped");
    ASSERT_STR_EQ(r.text, "The save on the cart\nchanged.");
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    /* dropped is Connect's only */
    gen3_cart_screen_result(Gen3CartJobBackup, Gen3CartStepAnalyze, Gen3CartOk, true, true, &r);
    ASSERT_STR_EQ(r.header, "Backed up");
}

/* A Connect or Reconnect that finished a write or restore cut short shows that one's result: the
 * header and text of a Write or Restore that ended so, no Retry; and a Reconnect that finished a
 * restore takes the new save's trainer, as a Restore does */
TEST(test_result_finished) {
    static const struct {
        Gen3CartFinished finished;
        Gen3CartJob job;
    } as[] = {
        {Gen3CartFinishedWrite, Gen3CartJobWrite},
        {Gen3CartFinishedRestore, Gen3CartJobRestore},
    };
    static const Gen3CartJob by[] = {Gen3CartJobConnect, Gen3CartJobReconnect};
    for(size_t i = 0; i < 2; i++) {
        for(size_t b = 0; b < 2; b++) {
            ASSERT_EQ(gen3_cart_screen_result_job(by[b], Gen3CartOk, as[i].finished), as[i].job);
            ASSERT_EQ(
                gen3_cart_screen_result_job(by[b], Gen3CartWrittenNoBackup, as[i].finished),
                as[i].job);
            /* nothing finished, or it didn't end so: the job's own */
            ASSERT_EQ(gen3_cart_screen_result_job(by[b], Gen3CartOk, Gen3CartFinishedNone), by[b]);
            ASSERT_EQ(gen3_cart_screen_result_job(by[b], Gen3CartLinkLost, as[i].finished), by[b]);
            ASSERT_EQ(gen3_cart_screen_result_job(by[b], Gen3CartChanged, as[i].finished), by[b]);
        }
        /* only a Connect or a Reconnect settles one */
        for(size_t j = 0; j < sizeof(all_jobs) / sizeof(all_jobs[0]); j++) {
            const Gen3CartJob job = all_jobs[j];
            if(job == Gen3CartJobConnect || job == Gen3CartJobReconnect) continue;
            ASSERT_EQ(gen3_cart_screen_result_job(job, Gen3CartOk, as[i].finished), job);
        }
    }
    Gen3CartScreenResult r;
    const Gen3CartJob w =
        gen3_cart_screen_result_job(Gen3CartJobReconnect, Gen3CartOk, Gen3CartFinishedWrite);
    gen3_cart_screen_result(w, Gen3CartStepPostBackup, Gen3CartOk, false, false, &r);
    ASSERT_STR_EQ(r.header, "Written");
    ASSERT_STR_EQ(r.text, "Written and verified.\nPower-cycle the GBA\nto play.");
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    const Gen3CartJob rs =
        gen3_cart_screen_result_job(Gen3CartJobConnect, Gen3CartOk, Gen3CartFinishedRestore);
    gen3_cart_screen_result(rs, Gen3CartStepPostBackup, Gen3CartOk, false, false, &r);
    ASSERT_STR_EQ(r.header, "Restored");
    ASSERT_STR_EQ(r.text, "Restored and\nverified. Power-cycle\nthe GBA to play.");
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    ASSERT_TRUE(gen3_cart_screen_new_trainer(rs, Gen3CartOk));
    gen3_cart_screen_result(
        gen3_cart_screen_result_job(
            Gen3CartJobConnect, Gen3CartWrittenNoBackup, Gen3CartFinishedWrite),
        Gen3CartStepPostBackup,
        Gen3CartWrittenNoBackup,
        false,
        true,
        &r);
    ASSERT_STR_EQ(r.header, "Written");
    ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartWrittenNoBackup));
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
}

TEST(test_result_texts_fit) {
    for(size_t j = 0; j < sizeof(all_jobs) / sizeof(all_jobs[0]); j++) {
        for(int res = Gen3CartOk; res <= Gen3CartLinkBusy; res++) {
            for(int step = Gen3CartStepNone; step <= Gen3CartStepRomVerify; step++) {
                for(int flags = 0; flags < 4; flags++) {
                    Gen3CartScreenResult r;
                    gen3_cart_screen_result(
                        all_jobs[j],
                        (Gen3CartStep)step,
                        (Gen3CartResult)res,
                        flags & 1,
                        flags & 2,
                        &r);
                    ASSERT_TRUE(r.header && r.header[0] != '\0');
                    ASSERT_TRUE(fits(r.header, 1, GEN3_CART_SCREEN_TITLE_LEN));
                    ASSERT_TRUE(r.text && r.text[0] != '\0');
                    ASSERT_TRUE(fits(r.text, 3, GEN3_CART_SCREEN_LINE_LEN));
                }
            }
        }
    }
}

/* A job that left the cart unlinked (a write-class response the client couldn't take, say) offers
 * Reconnect, whatever its result; one stopped by Back offers nothing, and the hub has Reconnect */
TEST(test_result_retry_unlinked) {
    Gen3CartScreenResult r;
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepProgram, Gen3CartHelperError, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryReconnect);
    ASSERT_STR_EQ(r.header, "Write failed");
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepProgram, Gen3CartHelperError, false, true, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobBackup, Gen3CartStepBackupRead, Gen3CartCancelled, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(Gen3CartJobDiscard, Gen3CartStepNone, Gen3CartOk, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    /* Connect and Reconnect keep their own */
    gen3_cart_screen_result(
        Gen3CartJobReconnect, Gen3CartStepIdentify, Gen3CartChanged, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobConnect, Gen3CartStepMbWait, Gen3CartMultibootFailed, false, false, &r);
    ASSERT_EQ(r.retry, Gen3CartRetryConnect);
}

/* Back stopped it: a neutral header, not "failed" */
TEST(test_result_stopped) {
    static const struct {
        Gen3CartJob job;
        const char* header;
    } want[] = {
        {Gen3CartJobConnect, "Connect stopped"},
        {Gen3CartJobBackup, "Backup stopped"},
        {Gen3CartJobRestoreCheck, "Check stopped"},
        {Gen3CartJobRom, "ROM copy stopped"},
        {Gen3CartJobReconnect, "Reconnect halted"},
    };
    Gen3CartScreenResult r;
    for(size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        gen3_cart_screen_result(
            want[k].job, Gen3CartStepIdentify, Gen3CartCancelled, false, true, &r);
        ASSERT_STR_EQ(r.header, want[k].header);
        /* a ROM copy keeps what it checked */
        ASSERT_STR_EQ(
            r.text, want[k].job == Gen3CartJobRom ? "Copy ROM again\nto resume." : "Stopped.");
        ASSERT_TRUE(strstr(r.header, "failed") == NULL);
        ASSERT_EQ(r.retry, Gen3CartRetryNone);
    }
}

/* Restore and ROM copy results */
TEST(test_result_restore_rom) {
    Gen3CartScreenResult r;
    gen3_cart_screen_result(
        Gen3CartJobRestore, Gen3CartStepPostBackup, Gen3CartOk, false, true, &r);
    ASSERT_STR_EQ(r.header, "Restored");
    ASSERT_STR_EQ(r.text, "Restored and\nverified. Power-cycle\nthe GBA to play.");
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    /* restored, but no new backup: the hub then offers one */
    gen3_cart_screen_result(
        Gen3CartJobRestore, Gen3CartStepPostBackup, Gen3CartWrittenNoBackup, false, true, &r);
    ASSERT_STR_EQ(r.header, "Restored");
    ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartWrittenNoBackup));
    gen3_cart_screen_result(
        Gen3CartJobRestore, Gen3CartStepPrepare, Gen3CartBadBackup, false, true, &r);
    ASSERT_STR_EQ(r.header, "Restore failed");
    /* the file's check, when it isn't Ok */
    gen3_cart_screen_result(
        Gen3CartJobRestoreCheck, Gen3CartStepRestoreCheck, Gen3CartBadBackup, false, true, &r);
    ASSERT_STR_EQ(r.header, "Can't restore");
    ASSERT_STR_EQ(r.text, "Not a valid backup\nfor this game.");
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    /* the ROM: its name goes under it (gen3_cart_screen_rom_done) */
    gen3_cart_screen_result(Gen3CartJobRom, Gen3CartStepRomVerify, Gen3CartOk, false, true, &r);
    ASSERT_STR_EQ(r.header, "ROM copied");
    ASSERT_STR_EQ(r.text, "Verified.");
    gen3_cart_screen_result(Gen3CartJobRom, Gen3CartStepRomRead, Gen3CartSdFull, false, true, &r);
    ASSERT_STR_EQ(r.header, "ROM copy failed");
    ASSERT_STR_EQ(r.text, "SD card full.");
}

TEST(test_result_written) {
    Gen3CartScreenResult r;
    gen3_cart_screen_result(Gen3CartJobWrite, Gen3CartStepPostBackup, Gen3CartOk, false, true, &r);
    ASSERT_STR_EQ(r.header, "Written");
    ASSERT_STR_EQ(r.text, "Written and verified.\nPower-cycle the GBA\nto play.");
    ASSERT_EQ(r.retry, Gen3CartRetryNone);
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepPostBackup, Gen3CartWrittenNoBackup, false, true, &r);
    ASSERT_STR_EQ(r.header, "Written");
    ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartWrittenNoBackup));
    gen3_cart_screen_result(
        Gen3CartJobWrite, Gen3CartStepPrepare, Gen3CartChanged, false, true, &r);
    ASSERT_STR_EQ(r.header, "Write failed");
    ASSERT_STR_EQ(r.text, "The cart changed\nsince the backup.\nBack up again.");
    /* A discard that found the cart changed dropped the edits all the same */
    gen3_cart_screen_result(Gen3CartJobDiscard, Gen3CartStepNone, Gen3CartOk, false, true, &r);
    ASSERT_STR_EQ(r.header, "Discarded");
    gen3_cart_screen_result(
        Gen3CartJobDiscard, Gen3CartStepNone, Gen3CartChanged, false, true, &r);
    ASSERT_STR_EQ(r.header, "Discarded");
    ASSERT_STR_EQ(r.text, gen3_cart_result_text(Gen3CartChanged));
    gen3_cart_screen_result(
        Gen3CartJobDiscard, Gen3CartStepNone, Gen3CartSdError, false, true, &r);
    ASSERT_STR_EQ(r.header, "Discard failed");
}

/* ---- The party and box screens ---- */

/* Why a Pokemon isn't opened or put, under "Can't edit" or "Can't put" */
TEST(test_mon_texts) {
    static const struct {
        Gen3CartMon m;
        const char* text;
    } want[] = {
        {Gen3CartMonMail, "Take the Mail off\nin the game first."},
        {Gen3CartMonItem, "It holds an item\nthe Flipper can't\nwrite back."},
        {Gen3CartMonBadData, "Unusual data:\nleft as is."},
        {Gen3CartMonEgg, "Eggs can't be\nedited."},
        {Gen3CartMonBadEgg, "Bad Egg: left as is."},
        {Gen3CartMonTaken, "That slot isn't\nfree."},
        {Gen3CartMonSdError, "SD card error."},
        {Gen3CartMonBadArg, "Invalid request."},
        {Gen3CartMonNoData, "Gen III data\nunreadable."},
    };
    Gen3CartScreenHub h;
    Gen3CartScreenHubIn in = hub_in();
    hub(&in, &h);
    for(size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        ASSERT_STR_EQ(gen3_cart_screen_mon_text(want[k].m, &h), want[k].text);
        ASSERT_STR_EQ(gen3_cart_screen_mon_text(want[k].m, NULL), want[k].text);
        ASSERT_TRUE(fits(want[k].text, 3, GEN3_CART_SCREEN_LINE_LEN));
    }
    ASSERT_STR_EQ(gen3_cart_screen_mon_text(Gen3CartMonOk, &h), "");
    ASSERT_STR_EQ(gen3_cart_screen_mon_text(Gen3CartMonEmpty, &h), "");
    ASSERT_STR_EQ(gen3_cart_screen_mon_text((Gen3CartMon)(Gen3CartMonPending + 1), &h), "");
    ASSERT_TRUE(fits(GEN3_CART_SCREEN_CANT_EDIT, 1, GEN3_CART_SCREEN_TITLE_LEN));
    ASSERT_TRUE(fits(GEN3_CART_SCREEN_CANT_PUT, 1, GEN3_CART_SCREEN_TITLE_LEN));
}

static const char* pending_text(const Gen3CartScreenHub* h) {
    return gen3_cart_screen_mon_text(Gen3CartMonPending, h);
}

/* t names an item of the hub only if h shows it: Reconnect, Write changes, Discard changes,
 * Restore backup, wherever a line breaks */
static bool names_only_shown(const char* t, const Gen3CartScreenHub* h) {
    char flat[96];
    snprintf(flat, sizeof(flat), "%s", t);
    for(char* p = flat; *p; p++) {
        if(*p == '\n') *p = ' ';
    }
    return (!strstr(flat, "Reconnect") || h->reconnect) &&
           (!strstr(flat, "Write changes") || h->write) &&
           (!strstr(flat, "Discard changes") || h->discard) &&
           (!strstr(flat, "Restore backup") || h->restore);
}

/* A write or restore pending (cut short, and not settled since): no Pokemon is opened or put
 * until it is. Discard changes ends it, even with no edits; and the text under "Can't edit" or
 * "Can't put" names only the hub's items that settle or end it. */
TEST(test_pending) {
    Gen3CartScreenHub h;
    Gen3CartScreenHubIn in = hub_in();

    /* A restore cut short, with no edits, reconnected: no Write changes, but Restore backup again,
     * or Discard changes */
    in.pending = true;
    hub(&in, &h);
    ASSERT_TRUE(h.browse);
    ASSERT_FALSE(h.write);
    ASSERT_TRUE(h.discard);
    ASSERT_TRUE(h.restore);
    ASSERT_FALSE(h.reconnect);
    ASSERT_STR_EQ(
        pending_text(&h), "Finish the restore\nfirst: Restore backup\nor Discard changes.");
    /* ... before the reconnect: Reconnect, and Discard changes still */
    in.linked = false;
    hub(&in, &h);
    ASSERT_TRUE(h.reconnect);
    ASSERT_TRUE(h.discard);
    ASSERT_FALSE(h.write);
    ASSERT_FALSE(h.restore);
    ASSERT_STR_EQ(pending_text(&h), "Finish the write\nfirst: Reconnect or\nDiscard changes.");

    /* A write cut short, its edits kept: Reconnect, and once linked Write changes */
    in = hub_in();
    in.pending = true;
    in.changes = 1;
    in.linked = false;
    hub(&in, &h);
    ASSERT_TRUE(h.reconnect);
    ASSERT_FALSE(h.write);
    ASSERT_TRUE(h.discard);
    ASSERT_STR_EQ(pending_text(&h), "Finish the write\nfirst: Reconnect or\nDiscard changes.");
    in.linked = true;
    hub(&in, &h);
    ASSERT_FALSE(h.reconnect);
    ASSERT_TRUE(h.write);
    ASSERT_TRUE(h.discard);
    ASSERT_STR_EQ(pending_text(&h), "Finish the write\nfirst: Write changes\nor Discard changes.");

    /* A restore cut short on a cart whose game loads no save (empty or corrupt): nothing to
     * browse waits on it, and a discard needs a save the game loads: no Discard */
    in = hub_in();
    in.pending = true;
    in.browsable = false;
    in.writable = false;
    in.why = Gen3CartRefuseSave;
    in.work_ok = false;
    hub(&in, &h);
    ASSERT_FALSE(h.browse);
    ASSERT_FALSE(h.discard);
    ASSERT_TRUE(h.restore);

    /* No hub (none read), or one without Discard: no item named */
    ASSERT_STR_EQ(pending_text(NULL), "Finish the write\nfirst.");
    in.busy = true;
    hub(&in, &h);
    ASSERT_STR_EQ(pending_text(&h), "Finish the write\nfirst.");

    /* Every hub with something pending: Discard changes whenever the list (and so the reason)
     * can show, and a text that fits and names only items shown */
    for(uint32_t bits = 0; bits < (1u << 11); bits++) {
        in = (Gen3CartScreenHubIn){
            .busy = (bits >> 0) & 1u,
            .linked = (bits >> 1) & 1u,
            .backup_current = (bits >> 2) & 1u,
            .can_read_save = (bits >> 3) & 1u,
            .browsable = (bits >> 4) & 1u,
            .writable = (bits >> 5) & 1u,
            .why = ((bits >> 6) & 1u) ? Gen3CartRefuseSave : Gen3CartRefuseNone,
            .work_ok = (bits >> 7) & 1u,
            .restorable = (bits >> 8) & 1u,
            .has_rom = (bits >> 9) & 1u,
            .changes = ((bits >> 10) & 1u) ? 2u : 0u,
            .pending = true,
        };
        hub(&in, &h);
        const char* t = pending_text(&h);
        ASSERT_TRUE(t[0] != '\0');
        ASSERT_TRUE(fits(t, 3, GEN3_CART_SCREEN_LINE_LEN));
        ASSERT_TRUE(names_only_shown(t, &h));
        if(!in.busy && in.browsable && in.backup_current) ASSERT_TRUE(h.discard);
        if(!in.linked) ASSERT_FALSE(h.write);
    }
}

static Gen3MonSummary summary(Gen3MonKind kind, uint16_t national, uint8_t level) {
    Gen3MonSummary s;
    memset(&s, 0, sizeof(s));
    s.kind = kind;
    s.national = national;
    s.level = level;
    return s;
}

TEST(test_rows) {
    char row[GEN3_CART_SCREEN_ROW_SIZE];
    Gen3MonSummary s = summary(Gen3MonOk, 252, 5);
    gen3_cart_screen_row(0, &s, "TREECKO", false, row);
    ASSERT_STR_EQ(row, "01 TREECKO Lv5");
    gen3_cart_screen_row(29, &s, "TREECKO", true, row);
    ASSERT_STR_EQ(row, "30 TREECKO Lv5*");
    s = summary(Gen3MonOk, 6, 100);
    gen3_cart_screen_row(9, &s, "CHARMANDER", true, row);
    ASSERT_STR_EQ(row, "10 CHARMANDER Lv100*");
    s = summary(Gen3MonEmpty, 0, 0);
    gen3_cart_screen_row(3, &s, "", false, row);
    ASSERT_STR_EQ(row, "04 -- empty --");
    s = summary(Gen3MonEgg, 252, 5);
    gen3_cart_screen_row(4, &s, "TREECKO", false, row);
    ASSERT_STR_EQ(row, "05 EGG");
    s = summary(Gen3MonBad, 0, 0);
    gen3_cart_screen_row(5, &s, "", false, row);
    ASSERT_STR_EQ(row, "06 BAD EGG");
    /* a slot the working copy couldn't give */
    gen3_cart_screen_row(6, NULL, "", false, row);
    ASSERT_STR_EQ(row, "07 ?");
}

/* Put <SPECIES> Lv<n> / in <box>\nslot <k>?, the level in the text when the header would be over
 * 16 characters */
TEST(test_put_text) {
    char h[GEN3_CART_SCREEN_HEADER_SIZE];
    char t[GEN3_CART_SCREEN_PUT_SIZE];
    gen3_cart_screen_put("TREECKO", 10, "BOX 2", 0, h, t);
    ASSERT_STR_EQ(h, "Put TREECKO Lv10");
    ASSERT_STR_EQ(t, "in BOX 2\nslot 1?");
    gen3_cart_screen_put("CHARMANDER", 100, "BOX 14", 29, h, t);
    ASSERT_STR_EQ(h, "Put CHARMANDER");
    ASSERT_STR_EQ(t, "Lv100 in BOX 14\nslot 30?");
    gen3_cart_screen_put("MEW", 5, "Party", 3, h, t);
    ASSERT_STR_EQ(h, "Put MEW Lv5");
    ASSERT_STR_EQ(t, "in Party\nslot 4?");
    /* every length fits */
    static const char* const names[] = {"A", "ABCDEFG", "ABCDEFGH", "ABCDEFGHI", "ABCDEFGHIJ"};
    static const char* const boxes[] = {"", "B", "ABCDEFGH"};
    static const uint8_t levels[] = {1, 9, 10, 99, 100};
    for(size_t a = 0; a < 5; a++) {
        for(size_t b = 0; b < 3; b++) {
            for(size_t c = 0; c < 5; c++) {
                for(uint8_t slot = 0; slot < 30; slot += 9) {
                    gen3_cart_screen_put(names[a], levels[c], boxes[b], slot, h, t);
                    ASSERT_TRUE(fits(h, 1, GEN3_CART_SCREEN_TITLE_LEN));
                    ASSERT_TRUE(fits(t, 2, GEN3_CART_SCREEN_LINE_LEN));
                    ASSERT_TRUE(strstr(h, names[a]) != NULL);
                }
            }
        }
    }
}

TEST(test_ask_texts) {
    char t[GEN3_CART_SCREEN_ASK_SIZE];
    gen3_cart_screen_write_ask(1, t);
    ASSERT_STR_EQ(t, "1 change. The save\nis backed up. Keep\nthe GBA on.");
    gen3_cart_screen_write_ask(3, t);
    ASSERT_STR_EQ(t, "3 changes. The save\nis backed up. Keep\nthe GBA on.");
    gen3_cart_screen_write_ask(GEN3_CART_MON_SLOTS, t);
    ASSERT_TRUE(fits(t, 4, GEN3_CART_SCREEN_LINE_LEN));
    gen3_cart_screen_discard_ask(1, t);
    ASSERT_STR_EQ(t, "1 change.");
    gen3_cart_screen_discard_ask(GEN3_CART_MON_SLOTS, t);
    ASSERT_STR_EQ(t, "426 changes.");
    ASSERT_TRUE(fits(t, 4, GEN3_CART_SCREEN_LINE_LEN));
    ASSERT_TRUE(fits("Write changes?", 1, GEN3_CART_SCREEN_TITLE_LEN));
    ASSERT_TRUE(fits("Discard changes?", 1, GEN3_CART_SCREEN_TITLE_LEN));
}

/* A .pk3's check, as Load failed shows it */
TEST(test_check_texts) {
    for(int c = Gen3CheckOk; c <= Gen3CheckNoData; c++) {
        const char* t = gen3_cart_screen_check_text((Gen3Check)c);
        ASSERT_TRUE(t[0] != '\0');
        ASSERT_TRUE(fits(t, 3, GEN3_CART_SCREEN_LINE_LEN));
    }
    ASSERT_STR_EQ(gen3_cart_screen_check_text(Gen3CheckNoData), "Gen III data\nunreadable");
    ASSERT_STR_EQ(
        gen3_cart_screen_check_text(Gen3CheckBadSize), gen3_check_text(Gen3CheckBadSize));
}

/* What a loaded .pk3 had fixed, broken into lines that fit */
TEST(test_wrap) {
    char t[40];
    snprintf(t, sizeof(t), "%s", "\nFixed: mail moves level ability");
    gen3_cart_screen_wrap(t);
    ASSERT_STR_EQ(t, "Fixed: mail moves\nlevel ability");
    snprintf(t, sizeof(t), "%s", "\nFixed: level");
    gen3_cart_screen_wrap(t);
    ASSERT_STR_EQ(t, "Fixed: level");
    snprintf(t, sizeof(t), "%s", "");
    gen3_cart_screen_wrap(t);
    ASSERT_STR_EQ(t, "");
    snprintf(t, sizeof(t), "%s", "aaaaaaaaaa bbbbbbbbbb cccc dd");
    gen3_cart_screen_wrap(t);
    ASSERT_STR_EQ(t, "aaaaaaaaaa bbbbbbbbbb\ncccc dd");
    ASSERT_TRUE(fits(t, 2, GEN3_CART_SCREEN_LINE_LEN));
}

/* ---- Restore and ROM copy ---- */

#define SHARED "/ext/apps_data/pokemon/gen3"

TEST(test_restore_name) {
    char name[GEN3_CART_NAME_MAX];
    memset(name, 'x', sizeof(name));
    ASSERT_EQ(
        gen3_cart_screen_restore_name(
            SHARED "/backups/AXVE_12345_20261025-143005.sav", SHARED, name),
        Gen3CartScreenNameOk);
    ASSERT_STR_EQ(name, "backups/AXVE_12345_20261025-143005.sav");
    /* the longest name the job takes: 47 characters */
    char path[160];
    char want[GEN3_CART_NAME_MAX];
    memset(want, 'a', GEN3_CART_NAME_MAX - 1u);
    want[GEN3_CART_NAME_MAX - 1u] = '\0';
    snprintf(path, sizeof(path), "%s/%s", SHARED, want);
    ASSERT_EQ(gen3_cart_screen_restore_name(path, SHARED, name), Gen3CartScreenNameOk);
    ASSERT_STR_EQ(name, want);
    /* one more is too long, and never cut */
    snprintf(path, sizeof(path), "%s/%sb", SHARED, want);
    memset(name, 0, sizeof(name));
    ASSERT_EQ(gen3_cart_screen_restore_name(path, SHARED, name), Gen3CartScreenNameTooLong);
    ASSERT_STR_EQ(name, "");
    /* outside the folder, or the folder itself */
    static const char* const outside[] = {
        "/ext/apps_data/pokemon/gen3x/backups/a.sav",
        "/ext/apps_data/pokemon/a.sav",
        "/ext/apps_data/pokemon/gen3",
        "/ext/apps_data/pokemon/gen3/",
        "backups/a.sav",
        "",
    };
    for(size_t k = 0; k < sizeof(outside) / sizeof(outside[0]); k++) {
        ASSERT_EQ(
            gen3_cart_screen_restore_name(outside[k], SHARED, name), Gen3CartScreenNameOutside);
    }
    ASSERT_STR_EQ(
        gen3_cart_screen_restore_name_text(Gen3CartScreenNameTooLong), "File name too long.");
    ASSERT_STR_EQ(
        gen3_cart_screen_restore_name_text(Gen3CartScreenNameOutside),
        "Pick a file in\ngen3/backups.");
    ASSERT_STR_EQ(gen3_cart_screen_restore_name_text(Gen3CartScreenNameOk), "");
    for(int n = Gen3CartScreenNameOutside; n <= Gen3CartScreenNameTooLong; n++) {
        ASSERT_TRUE(fits(
            gen3_cart_screen_restore_name_text((Gen3CartScreenName)n),
            3,
            GEN3_CART_SCREEN_LINE_LEN));
    }
    ASSERT_TRUE(fits("Can't restore", 1, GEN3_CART_SCREEN_TITLE_LEN));
}

static Gen3SaveTrainer trainer_named(const char* name, uint16_t tid) {
    Gen3SaveTrainer t;
    memset(&t, 0, sizeof(t));
    snprintf(t.name, sizeof(t.name), "%s", name);
    t.tid = tid;
    return t;
}

TEST(test_restore_ask) {
    char t[GEN3_CART_SCREEN_ASK_SIZE];
    const Gen3SaveTrainer cart = trainer_named("BRENDAN", 12345);
    const Gen3SaveTrainer file = trainer_named("MAY", 7);
    gen3_cart_screen_restore_ask(&cart, &file, 0, t);
    ASSERT_STR_EQ(t, "Cart: BRENDAN 12345\nFile: MAY 00007\nHall of Fame is kept.");
    /* no save the game loads on the cart */
    gen3_cart_screen_restore_ask(NULL, &file, 0, t);
    ASSERT_STR_EQ(t, "Cart: no save\nFile: MAY 00007\nHall of Fame is kept.");
    /* edits not written: the restore drops them, and says so first */
    gen3_cart_screen_restore_ask(&cart, &cart, 1, t);
    ASSERT_STR_EQ(
        t, "Cart: BRENDAN 12345\nFile: BRENDAN 12345\nHall of Fame is kept.\n1 change is lost.");
    gen3_cart_screen_restore_ask(&cart, &file, GEN3_CART_MON_SLOTS, t);
    ASSERT_STR_EQ(
        t, "Cart: BRENDAN 12345\nFile: MAY 00007\nHall of Fame is kept.\n426 changes are lost.");
    ASSERT_TRUE(fits(t, 4, GEN3_CART_SCREEN_LINE_LEN));
    const Gen3SaveTrainer longest = trainer_named("ABCDEFG", 65535);
    gen3_cart_screen_restore_ask(&longest, &longest, GEN3_CART_MON_SLOTS, t);
    ASSERT_TRUE(fits(t, 4, GEN3_CART_SCREEN_LINE_LEN));
    ASSERT_TRUE(fits("Restore backup?", 1, GEN3_CART_SCREEN_TITLE_LEN));
}

TEST(test_rom_texts) {
    char t[GEN3_CART_SCREEN_ASK_SIZE];
    /* the five games: 16 MiB at 1850 bytes a second, 2 h 31 min, to the nearest 10 minutes */
    gen3_cart_screen_rom_ask(16u * 1048576u, t);
    ASSERT_STR_EQ(t, "16 MB, about\n2 h 30 min.\nKeep both powered.");
    gen3_cart_screen_rom_ask(32u * 1048576u, t);
    ASSERT_STR_EQ(t, "32 MB, about\n5 h 0 min.\nKeep both powered.");
    gen3_cart_screen_rom_ask(8u * 1048576u, t);
    ASSERT_STR_EQ(t, "8 MB, about\n1 h 20 min.\nKeep both powered.");
    /* under an hour */
    gen3_cart_screen_rom_ask(1048576u, t);
    ASSERT_STR_EQ(t, "1 MB, about\n10 min.\nKeep both powered.");
    for(uint32_t mib = 1; mib <= 32; mib <<= 1) {
        gen3_cart_screen_rom_ask(mib * 1048576u, t);
        ASSERT_TRUE(fits(t, 4, GEN3_CART_SCREEN_LINE_LEN));
    }
    gen3_cart_screen_rom_resume(4u * 1048576u, 16u * 1048576u, t);
    ASSERT_STR_EQ(t, "25% done");
    gen3_cart_screen_rom_resume(0x40000u, 16u * 1048576u, t);
    ASSERT_STR_EQ(t, "1% done");
    gen3_cart_screen_rom_resume(16u * 1048576u - 0x40000u, 16u * 1048576u, t);
    ASSERT_STR_EQ(t, "98% done");
    gen3_cart_screen_rom_resume(5, 0, t);
    ASSERT_STR_EQ(t, "0% done");
    /* Resume copy?'s Restart asks again: what it removes */
    gen3_cart_screen_rom_restart(4u * 1048576u, 16u * 1048576u, t);
    ASSERT_STR_EQ(t, "25% done is lost:\nthe copy starts\nagain from 0%.");
    gen3_cart_screen_rom_restart(16u * 1048576u - 0x40000u, 16u * 1048576u, t);
    ASSERT_STR_EQ(t, "98% done is lost:\nthe copy starts\nagain from 0%.");
    gen3_cart_screen_rom_restart(5, 0, t);
    ASSERT_STR_EQ(t, "0% done is lost:\nthe copy starts\nagain from 0%.");
    gen3_cart_screen_rom_restart(100, 100, t);
    ASSERT_TRUE(fits(t, 4, GEN3_CART_SCREEN_LINE_LEN));
    gen3_cart_screen_rom_done("roms/AXVE_v1.gba", t);
    ASSERT_STR_EQ(t, "Verified.\nroms/AXVE_v1.gba");
    gen3_cart_screen_rom_done("roms/BPRE_v255.gba", t);
    ASSERT_TRUE(fits(t, 3, GEN3_CART_SCREEN_LINE_LEN));
    static const char* const headers[] = {
        "Copy ROM?", "Resume copy?", "Restart copy?", "Stop copying?"};
    for(size_t k = 0; k < 4; k++) ASSERT_TRUE(fits(headers[k], 1, GEN3_CART_SCREEN_TITLE_LEN));
    ASSERT_TRUE(fits("You can resume\nlater.", 4, GEN3_CART_SCREEN_LINE_LEN));
}

/* The time a ROM copy has left, from the rate measured since its first progress */
TEST(test_time_left) {
    char t[GEN3_CART_SCREEN_LEFT_SIZE];
    const uint32_t rom = 16u * 1048576u;
    /* nothing measured yet */
    gen3_cart_screen_time_left(0, 0, rom, 60000, t);
    ASSERT_STR_EQ(t, "");
    gen3_cart_screen_time_left(0, 18500, rom, GEN3_CART_SCREEN_RATE_MS - 1u, t);
    ASSERT_STR_EQ(t, "");
    /* 1850 bytes a second from the start: 9058.8 s left, rounded up to the minute */
    gen3_cart_screen_time_left(0, 18500, rom, 10000, t);
    ASSERT_STR_EQ(t, "2 h 31 min left");
    /* resumed at a quarter, at 3700 bytes a second: 3390.6 s */
    gen3_cart_screen_time_left(rom / 4u, rom / 4u + 37000u, rom, 10000, t);
    ASSERT_STR_EQ(t, "57 min left");
    /* the last bytes */
    gen3_cart_screen_time_left(0, rom - 1u, rom, 10000, t);
    ASSERT_STR_EQ(t, "1 min left");
    gen3_cart_screen_time_left(0, rom, rom, 10000, t);
    ASSERT_STR_EQ(t, "");
    /* very slow: capped, and every text fits */
    gen3_cart_screen_time_left(0, 1, 32u * 1048576u, UINT32_MAX, t);
    ASSERT_STR_EQ(t, "over 99 h left");
    gen3_cart_screen_time_left(0, 1000, rom, 10000, t);
    ASSERT_TRUE(fits(t, 1, GEN3_CART_SCREEN_LINE_LEN));
    gen3_cart_screen_time_left(0, 5, rom, 10000, t);
    ASSERT_STR_EQ(t, "over 99 h left");
    /* done back before done0 (a resume that cut a region back): nothing measured */
    gen3_cart_screen_time_left(0x80000u, 0x40000u, rom, 60000, t);
    ASSERT_STR_EQ(t, "");
}

/* The slot router on entering: only a Pokemon the editor opened, made or loaded for this slot
 * (mode not None), coming back from the editor, is ever closed or put */
TEST(test_slot_entry) {
    static const Gen3CartEditMode modes[] = {
        Gen3CartEditNone, Gen3CartEditExisting, Gen3CartEditNew, Gen3CartEditLoaded};
    for(uint32_t state = Gen3CartSlotOpen; state <= Gen3CartSlotReedit + 1u; state++) {
        for(size_t m = 0; m < 4; m++) {
            const bool made = modes[m] == Gen3CartEditNew || modes[m] == Gen3CartEditLoaded;
            Gen3CartSlotDo want = Gen3CartSlotDoOpen;
            if(state == Gen3CartSlotEdit && modes[m] == Gen3CartEditExisting) {
                want = Gen3CartSlotDoClose;
            } else if(state == Gen3CartSlotEdit && made) {
                want = Gen3CartSlotDoPut;
            } else if(state == Gen3CartSlotReedit && made) {
                want = Gen3CartSlotDoEdit;
            }
            ASSERT_EQ(gen3_cart_screen_slot_entry(state, modes[m]), want);
        }
    }
    ASSERT_EQ(
        gen3_cart_screen_slot_entry(Gen3CartSlotEdit, (Gen3CartEditMode)(Gen3CartEditLoaded + 1)),
        Gen3CartSlotDoOpen);
}

/* Cart info */

static Gen3CartId cid;
static Gen3SaveSlots slots;
static Gen3SaveTrainer trainer;
static Gen3CartScreenInfo in;
static char header[GEN3_CART_SCREEN_HEADER_SIZE];
static char text[GEN3_CART_SCREEN_INFO_SIZE];

/* A readable, writable Ruby with an Ok save in slot B and a current backup */
static void ruby(void) {
    memset(&cid, 0, sizeof(cid));
    memcpy(cid.code, "AXVE", 5);
    memcpy(cid.title, "POKEMON RUBY", 13);
    cid.version = 1;
    cid.game = Gen3LinkGameRS;
    cid.met_game = 2;
    cid.flash_id = 0x09C2u;
    cid.can_read_save = true;
    cid.can_browse = true;
    cid.can_write = true;
    cid.why = Gen3CartRefuseNone;
    memset(&slots, 0, sizeof(slots));
    slots.slot[0] = Gen3SlotOk;
    slots.slot[1] = Gen3SlotOk;
    slots.counter[0] = 40;
    slots.counter[1] = 41;
    slots.result = Gen3SaveOk;
    slots.save_counter = 41;
    slots.active = 1;
    slots.strict_ok = true;
    memset(&trainer, 0, sizeof(trainer));
    memcpy(trainer.name, "BRENDAN", 8);
    trainer.tid = 42;
    in.id = &cid;
    in.backup_current = true;
    in.backup_name = "backups/AXVE_00042_20261027-093015.sav";
    in.slots = &slots;
    in.trainer = &trainer;
    in.writable = true;
    in.why = Gen3CartRefuseNone;
}

static void info(void) {
    memset(header, 0xA5, sizeof(header));
    memset(text, 0xA5, sizeof(text));
    gen3_cart_screen_info(&in, header, text);
}

static bool info_line(unsigned k, const char* want) {
    char got[64];
    line_of(text, k, got);
    if(strcmp(got, want) == 0) return true;
    printf("  line %u: \"%s\" != \"%s\"\n", k, got, want);
    return false;
}

static bool info_fits(void) {
    return fits(header, 1, GEN3_CART_SCREEN_TITLE_LEN) && line_count(text) == 5 &&
           fits(text, 5, GEN3_CART_SCREEN_LINE_LEN);
}

TEST(test_info_ruby) {
    ruby();
    info();
    ASSERT_STR_EQ(header, "AXVE v1");
    ASSERT_TRUE(info_line(0, "Flash 09C2 Macronix"));
    ASSERT_TRUE(info_line(1, "Save: B #41"));
    ASSERT_TRUE(info_line(2, "BRENDAN 00042"));
    ASSERT_TRUE(info_line(3, "Writing on"));
    ASSERT_TRUE(info_line(4, "Backup 1027-093015"));
    ASSERT_TRUE(info_fits());
}

TEST(test_info_saves) {
    ruby();
    cid.flash_id = 0x1362u;
    slots.active = 0;
    slots.save_counter = 4294967295u;
    trainer.tid = 65535;
    info();
    ASSERT_TRUE(info_line(0, "Flash 1362 Sanyo"));
    ASSERT_TRUE(info_line(1, "Save: A #4294967295"));
    ASSERT_TRUE(info_line(2, "BRENDAN 65535"));
    ASSERT_TRUE(info_fits());

    /* One slot damaged: the game loads the other */
    slots.result = Gen3SaveError;
    slots.active = 1;
    slots.save_counter = 7;
    info();
    ASSERT_TRUE(info_line(1, "Save: B #7"));

    ruby();
    slots.result = Gen3SaveEmpty;
    in.trainer = NULL;
    in.writable = false;
    in.why = Gen3CartRefuseSave;
    info();
    ASSERT_TRUE(info_line(1, "Save: empty"));
    ASSERT_TRUE(info_line(2, "No trainer"));
    ASSERT_TRUE(info_line(3, "Save not recognised"));
    ASSERT_TRUE(info_fits());

    slots.result = Gen3SaveCorrupt;
    info();
    ASSERT_TRUE(info_line(1, "Save: corrupt"));
    ASSERT_TRUE(info_fits());
}

TEST(test_info_no_backup) {
    /* Japanese: backed up, but its layout isn't known */
    ruby();
    memcpy(cid.code, "AXVJ", 5);
    cid.can_browse = false;
    cid.can_write = false;
    cid.why = Gen3CartRefuseJapanese;
    in.slots = NULL;
    in.trainer = NULL;
    in.writable = false;
    in.why = Gen3CartRefuseJapanese;
    info();
    ASSERT_STR_EQ(header, "AXVJ v1");
    ASSERT_TRUE(info_line(1, "Save: layout unknown"));
    ASSERT_TRUE(info_line(3, "Japanese: backup only"));
    ASSERT_TRUE(info_line(4, "Backup 1027-093015"));
    ASSERT_TRUE(info_fits());

    /* The backup went (a cart that changed): no save to show */
    ruby();
    in.backup_current = false;
    in.backup_name = "";
    in.slots = NULL;
    in.trainer = NULL;
    in.writable = false;
    in.why = Gen3CartRefuseSave;
    info();
    ASSERT_TRUE(info_line(1, "Save: no backup"));
    ASSERT_TRUE(info_line(4, "No backup"));
    ASSERT_TRUE(info_fits());

    /* A name without a stamp in it */
    ruby();
    in.backup_name = "backups/x.sav";
    info();
    ASSERT_TRUE(info_line(4, "Backup made"));
    ASSERT_TRUE(info_fits());
}

TEST(test_info_unreadable) {
    /* A save the helper can't read still reaches the hub: why, and no save */
    static const Gen3CartRefuse whys[] = {
        Gen3CartRefuseHeader, Gen3CartRefuseNotPokemon, Gen3CartRefuseSram, Gen3CartRefuseChip};
    for(size_t k = 0; k < sizeof(whys) / sizeof(whys[0]); k++) {
        ruby();
        memcpy(cid.code, "BPEE", 5);
        cid.version = 255;
        cid.flash_id = 0xD4BFu;
        cid.can_read_save = false;
        cid.can_browse = false;
        cid.can_write = false;
        cid.why = whys[k];
        in.backup_current = false;
        in.backup_name = "";
        in.slots = NULL;
        in.trainer = NULL;
        in.writable = false;
        in.why = whys[k];
        info();
        ASSERT_STR_EQ(header, "BPEE v255");
        ASSERT_TRUE(info_line(0, "Flash D4BF ?"));
        ASSERT_TRUE(info_line(1, "Save: not read"));
        ASSERT_TRUE(info_line(2, "No trainer"));
        ASSERT_TRUE(info_line(3, gen3_cart_refuse_text(whys[k])));
        ASSERT_TRUE(info_line(4, "No backup"));
        ASSERT_TRUE(info_fits());
    }
}

TEST(test_leave_text) {
    char t[GEN3_CART_SCREEN_LEAVE_SIZE];
    gen3_cart_screen_leave(1, t);
    ASSERT_STR_EQ(t, "1 change not\nwritten.");
    gen3_cart_screen_leave(2, t);
    ASSERT_STR_EQ(t, "2 changes not\nwritten.");
    gen3_cart_screen_leave(GEN3_CART_MON_SLOTS, t);
    ASSERT_STR_EQ(t, "426 changes not\nwritten.");
    ASSERT_TRUE(fits(t, 2, GEN3_CART_SCREEN_LINE_LEN));
    gen3_cart_screen_leave(65535, t);
    ASSERT_TRUE(fits(t, 2, GEN3_CART_SCREEN_LINE_LEN));
}

int main(void) {
    RUN(test_back);
    RUN(test_new_trainer);
    RUN(test_progress_connect_steps);
    RUN(test_progress_cable);
    RUN(test_power_hint);
    RUN(test_progress_ignores_link_up);
    RUN(test_progress_write);
    RUN(test_progress_starting_stopping);
    RUN(test_progress_texts_fit);
    RUN(test_hub);
    RUN(test_result_retry);
    RUN(test_result_after_commit);
    RUN(test_result_dropped);
    RUN(test_result_finished);
    RUN(test_result_texts_fit);
    RUN(test_result_retry_unlinked);
    RUN(test_result_stopped);
    RUN(test_result_written);
    RUN(test_result_restore_rom);
    RUN(test_mon_texts);
    RUN(test_pending);
    RUN(test_rows);
    RUN(test_put_text);
    RUN(test_ask_texts);
    RUN(test_check_texts);
    RUN(test_wrap);
    RUN(test_restore_name);
    RUN(test_restore_ask);
    RUN(test_rom_texts);
    RUN(test_time_left);
    RUN(test_slot_entry);
    RUN(test_info_ruby);
    RUN(test_info_saves);
    RUN(test_info_no_backup);
    RUN(test_info_unreadable);
    RUN(test_leave_text);
    TEST_EXIT();
}
