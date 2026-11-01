#include <src/gen3/gen3_cart_furi.h>

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#include <gblink/include/gblink.h>
#include <gblink/include/gblink_pinconf.h>

#include <src/gen3/gen3_hcli.h>
#include <src/gen3/gen3_hlink.h>
#include <src/gen3/gen3_store_furi.h>
#include <src/gen3/mlink_hw_furi.h>

#define TAG "gen3_cart"

#define GEN3_CART_LOG_PATH EXT_PATH("apps_data/pokemon/gen3_cart.log")
#define GEN3_CART_LOG_HEADER "# gen3 cart log v1\n"
/* The longest line is cart_log_mb's, at most 162 characters with its newline: 11 for the tick
 * ("%08lu " of 4294967295), 10 "mb restart", 9 " nr 65535", 10 " last ffff", 19
 * " want ffff got ffff", 13 " restarts 255", 10 " all 65535", 12 " last_fail 9", 6 " cc ff",
 * 6 " rr ff", 19 " palette_busy 65535", 20 " final_us 4294967295", 16 " sent 4294967295" and 1 for
 * the newline. With its NUL it fits, so no line is ever cut. */
#define GEN3_CART_LOG_LINE 168u

#define GEN3_CART_THREAD_NAME "Gen3Cart"
#define GEN3_CART_THREAD_STACK 2048
#define GEN3_CART_FLAG_JOB (1u << 0)
#define GEN3_CART_FLAG_EXIT (1u << 1)

/* Connect: how often the worker reads the multiboot sender's progress */
#define GEN3_CART_MB_POLL_MS 20u
/* Connect: polls in a row that must find SI high, while the sender waits for the GBA, before the
 * MultiPlay wire gives way to the Normal wire: a jiggled GBA cable doesn't switch */
#define GEN3_CART_WIRE_POLLS 2u
/* Connect: an `mb wait` log line this often while the sender waits for the GBA */
#define GEN3_CART_MB_WAIT_LOG_MS 5000u

/* ---- Tunables for the hardware test (docs/gen3/hardware-test-3.md "If it fails") ---- */

/* Idle ticks sent after each frame that transferred, before the core gets that frame's result,
 * whose elapsed time then includes theirs. 0, the default, sends frame after frame. A knob for the
 * hardware check of stream mode beside the Flipper's own radio and USB, whose interrupts wait
 * while a transfer masks them (multiboot-and-save.md M18). */
#define GEN3_CART_GAP_TICKS 0u
_Static_assert(GEN3_CART_GAP_TICKS <= UINT8_MAX, "gap_left counts the idle ticks");

/* Normal wire, the BIOS phase (multiboot): 0, the default, clocks every slot ungated, with no wait
 * for the GBA's ready (SI low), as the probe of 2026-10-30 did: GBATEK's "slave not in mode yet"
 * reply suggests the BIOS takes Normal mode only once transfers come (multiboot-and-save.md §7.3,
 * M27). 1 gates it too, as the helper's phase always is. */
#define GEN3_CART_MB_GATED 0
/* 1: every Connect starts on the Normal wire, whatever SI reads (a GBC cable whose SI reads low,
 * so that Connect stays on `wire multi`). 0, the default: the cable decides. */
#define GEN3_CART_FORCE_NORMAL 0

typedef enum {
    Gen3CartCoreMb, /* the multiboot sender */
    Gen3CartCoreHlink, /* the helper's packet pump */
} Gen3CartCore;

struct Gen3CartSession {
    MlinkHw* hw;
    Gen3StoreFuri* store;
    Storage* storage;
    FuriThread* thread;
    FuriMutex* lock;

    /* Under lock. The app thread writes the request only while no job runs or waits for the
     * worker; the worker takes it and is the only writer of the status. */
    bool req_pending;
    Gen3CartJob req_job;
    /* A longer name is cut to GEN3_CART_NAME_MAX characters, a length gen3_cart refuses */
    char req_arg[GEN3_CART_NAME_MAX + 1u];
    bool req_has_arg;
    Gen3CartSessionStatus status;
    bool driver_on; /* the worker started the driver and hasn't stopped it */

    volatile bool cancel; /* the running job's: set by the app, cleared by the next run */

    /* The worker's (and the GUI thread's, through gen3_cart_session_cart, while no job runs) */
    bool pump_on; /* the driver runs the pump: the transport may use it */
    uint8_t* buf; /* the helper image, then the jobs' sector buffer */
    uint32_t buf_len;
    char stamp[16]; /* YYYYMMDD-HHMMSS */
    Gen3Hcli cli;
    Gen3Cart cart;
    bool cart_ready; /* buf, cli and cart are set up */
    File* log;
    bool log_open;
    char line[GEN3_CART_LOG_LINE];

    /* The frame callback's. core changes only while the driver is stopped, or to switch to the
     * pump once the sender is Done (gen3_hlink_init, a barrier, then core). */
    volatile uint8_t core;
    MlinkWire wire; /* the wire this Connect runs on; the worker's */
    Gen3Mb mb;
    Gen3Hlink hlink;
    uint8_t gap_left; /* idle ticks still to send before the core gets gap_rx */
    uint32_t gap_us;
    MlinkRx gap_rx;
};

/* ---- Frame callback: TIM2 interrupt context. It calls the two cores, both ISR-safe
 * (gen3_mb.h, gen3_hlink.h), directly and nothing else: no furi call, allocation or logging. */

static void gen3_cart_frame(void* ctx, const MlinkRx* rx, MlinkTx* next) {
    Gen3CartSession* s = ctx;
    /* rx->ticks is the frame's time on either wire. A frame the MultiPlay cable guard skipped
     * took one tick: with the cable out, the cores' timeouts can only end early, which never
     * risks the save (gen3_hlink.h) */
    uint32_t elapsed_us = (uint32_t)rx->ticks * MLINK_TICK_US;

    if(GEN3_CART_GAP_TICKS) {
        if(s->gap_left) {
            s->gap_us += elapsed_us;
            if(--s->gap_left) {
                next->count = 0;
                return;
            }
            rx = &s->gap_rx;
            elapsed_us = s->gap_us;
        } else if(rx->count) {
            s->gap_rx = *rx;
            s->gap_us = elapsed_us;
            s->gap_left = (uint8_t)GEN3_CART_GAP_TICKS;
            next->count = 0;
            return;
        }
    }

    if(s->core == Gen3CartCoreHlink) {
        gen3_hlink_frame(&s->hlink, rx, next, elapsed_us);
    } else {
        gen3_mb_frame(&s->mb, rx, next, elapsed_us);
    }
}

/* ---- Status ---- */

static void cart_lock(Gen3CartSession* s) {
    furi_check(furi_mutex_acquire(s->lock, FuriWaitForever) == FuriStatusOk);
}

static void cart_unlock(Gen3CartSession* s) {
    furi_check(furi_mutex_release(s->lock) == FuriStatusOk);
}

static void cart_progress(void* ctx, Gen3CartStep step, uint32_t done, uint32_t total) {
    Gen3CartSession* s = ctx;
    cart_lock(s);
    s->status.step = step;
    s->status.done = done;
    s->status.total = total;
    cart_unlock(s);
}

/* The worker is the status's only writer, so it may read it without the lock */
static void cart_set_link_up(Gen3CartSession* s, bool up) {
    if(s->status.link_up == up) return;
    cart_lock(s);
    s->status.link_up = up;
    cart_unlock(s);
}

static void cart_set_cable(Gen3CartSession* s, bool ok) {
    if(s->status.cable_ok == ok) return;
    cart_lock(s);
    s->status.cable_ok = ok;
    cart_unlock(s);
}

/* ---- Log: the worker's only, one line per event ---- */

static const char* cart_job_name(Gen3CartJob job) {
    static const char* const names[] = {
        [Gen3CartJobConnect] = "connect",
        [Gen3CartJobBackup] = "backup",
        [Gen3CartJobWrite] = "write",
        [Gen3CartJobDiscard] = "discard",
        [Gen3CartJobRestoreCheck] = "restore_check",
        [Gen3CartJobRestore] = "restore",
        [Gen3CartJobRom] = "rom",
        [Gen3CartJobReconnect] = "reconnect",
    };
    return (size_t)job < COUNT_OF(names) ? names[job] : "?";
}

static const char* cart_result_name(Gen3CartResult r) {
    static const char* const names[] = {
        [Gen3CartOk] = "Ok",
        [Gen3CartWrittenNoBackup] = "WrittenNoBackup",
        [Gen3CartNothing] = "Nothing",
        [Gen3CartCancelled] = "Cancelled",
        [Gen3CartLinkLost] = "LinkLost",
        [Gen3CartBusyTimeout] = "BusyTimeout",
        [Gen3CartHelperError] = "HelperError",
        [Gen3CartRefused] = "Refused",
        [Gen3CartChanged] = "Changed",
        [Gen3CartSdFull] = "SdFull",
        [Gen3CartSdError] = "SdError",
        [Gen3CartVerifyFailed] = "VerifyFailed",
        [Gen3CartFlashError] = "FlashError",
        [Gen3CartBadBackup] = "BadBackup",
        [Gen3CartBadArg] = "BadArg",
        [Gen3CartNoHelper] = "NoHelper",
        [Gen3CartMultibootFailed] = "MultibootFailed",
        [Gen3CartLinkBusy] = "LinkBusy",
    };
    return (size_t)r < COUNT_OF(names) ? names[r] : "?";
}

/* App thread, before the worker starts. No SD card: no log, and the jobs run all the same. */
static void cart_log_open(Gen3CartSession* s) {
    s->log_open = storage_file_open(s->log, GEN3_CART_LOG_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(s->log_open) {
        const size_t len = strlen(GEN3_CART_LOG_HEADER);
        s->log_open = storage_file_write(s->log, GEN3_CART_LOG_HEADER, len) == len;
    }
    if(!s->log_open) {
        FURI_LOG_W(TAG, "No log: can't write %s", GEN3_CART_LOG_PATH);
        storage_file_close(s->log); /* required even after a failed open */
    }
}

/* Writes s->line, len bytes as snprintf counted them; a line too long is cut, keeping its newline.
 * Each line goes to the card at once: the log is for the runs that go wrong. After a failed write
 * the log stops. */
static void cart_log_put(Gen3CartSession* s, int len) {
    if(!s->log_open || len <= 0) return;
    size_t n = (size_t)len;
    if(n >= sizeof(s->line)) {
        n = sizeof(s->line) - 1u;
        s->line[n - 1u] = '\n';
    }
    if(storage_file_write(s->log, s->line, n) != n || !storage_file_sync(s->log)) {
        storage_file_close(s->log);
        s->log_open = false;
    }
}

/* One log line: "%08lu " with the tick, the text, a newline. At least one argument after fmt. */
#define CART_LOG(s, fmt, ...)               \
    cart_log_put(                           \
        (s),                                \
        snprintf(                           \
            (s)->line,                      \
            sizeof((s)->line),              \
            "%08lu " fmt "\n",              \
            (unsigned long)furi_get_tick(), \
            __VA_ARGS__))

/* After a connect that ran to its end: the cart as the helper found it */
static void cart_log_cart(Gen3CartSession* s) {
    const Gen3CartId* id = gen3_cart_id(&s->cart);
    CART_LOG(
        s,
        "cart %s v%u game %u flash %04x read %u browse %u write %u why %u rom %lu",
        id->code,
        (unsigned)id->version,
        (unsigned)id->game,
        (unsigned)id->flash_id,
        (unsigned)id->can_read_save,
        (unsigned)id->can_browse,
        (unsigned)id->can_write,
        (unsigned)id->why,
        (unsigned long)gen3_cart_rom_size(&s->cart));
}

/* The backup's slot analysis (multiboot-and-save.md §5.5) */
static void cart_log_slots(Gen3CartSession* s) {
    const Gen3Cart* c = &s->cart;
    if(!gen3_cart_backup_current(c)) {
        CART_LOG(s, "backup %s", "none");
        return;
    }
    CART_LOG(s, "backup %s", gen3_cart_backup_name(c));
    if(!gen3_cart_id(c)->can_browse) return;
    const Gen3SaveSlots* sl = gen3_cart_slots(c);
    CART_LOG(
        s,
        "save %u slots %u/%u counters %lu/%lu active %u counter %lu strict %u padded %u",
        (unsigned)sl->result,
        (unsigned)sl->slot[0],
        (unsigned)sl->slot[1],
        (unsigned long)sl->counter[0],
        (unsigned long)sl->counter[1],
        (unsigned)sl->active,
        (unsigned long)sl->save_counter,
        (unsigned)sl->strict_ok,
        (unsigned)sl->padded);
}

/* ---- Transport (gen3_hcli.h): the worker hands one request or sync at a time to the pump ---- */

/* Waits for the pump to end what it was given. Every wait it runs ends by its own budget
 * (gen3_hlink.h), and frames keep coming while the driver runs, cable or none. The ISR sends no
 * signal: a 1 ms poll costs at most about a tick of the link per exchange. */
static Gen3HlinkState cart_pump_wait(Gen3CartSession* s) {
    Gen3HlinkState st;
    while((st = gen3_hlink_state(&s->hlink)) != Gen3HlinkDone && st != Gen3HlinkFailed) {
        furi_delay_ms(1);
    }
    return st;
}

static uint16_t cart_le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static Gen3HlinkFail
    cart_exchange(void* ctx, const Gen3HlinkReq* req, uint8_t* status, uint16_t* resp_words) {
    Gen3CartSession* s = ctx;
    /* Without the pump (no helper sent, or the driver stopped), as a helper that never answers */
    Gen3HlinkFail fail = Gen3HlinkFailNoReady;
    uint16_t notready = 0;
    *status = 0;
    *resp_words = 0;
    if(s->pump_on) {
        gen3_hlink_submit(&s->hlink, req);
        cart_pump_wait(s);
        gen3_hlink_result(&s->hlink, status, resp_words, &fail);
        notready = gen3_hlink_notready(&s->hlink);
    }
    cart_set_link_up(s, fail == Gen3HlinkFailNone);
    if(fail != Gen3HlinkFailNone) {
        /* The request, why it failed (Gen3HlinkFail) and its not-ready words (Normal wire) */
        CART_LOG(
            s,
            "cmd %02x fail %u nr %u",
            (unsigned)req->code,
            (unsigned)fail,
            (unsigned)notready);
    }
    /* A sound response has at least the 2 words resp_max_words always leaves room for. A status
     * other than OK carries the fail code and offset (multiboot-and-save.md §6.4); HELLO's answer
     * starts with the protocol and the build, and its 4th word with the BIOS's boot mode, 2 for
     * Normal, 3 for MultiPlay (multiboot-and-save.md §6.3, §7.2) */
    if(fail != Gen3HlinkFailNone || *resp_words < 2u || !req->resp) return fail;
    if(*status != GEN3_HST_OK) {
        CART_LOG(
            s,
            "cmd %02x last_status %02x fail_code %u fail_off %u",
            (unsigned)req->code,
            (unsigned)*status,
            (unsigned)cart_le16(req->resp),
            (unsigned)cart_le16(req->resp + 2));
    } else if(req->code == GEN3_HCMD_HELLO) {
        CART_LOG(
            s,
            "hello proto %u build %u boot %u",
            (unsigned)cart_le16(req->resp),
            (unsigned)cart_le16(req->resp + 2),
            *resp_words >= 4u ? (unsigned)req->resp[6] : 0u);
    }
    return fail;
}

static bool cart_sync(void* ctx, uint8_t need, uint32_t timeout_us) {
    Gen3CartSession* s = ctx;
    if(!s->pump_on) return false;
    gen3_hlink_sync(&s->hlink, need, timeout_us);
    const bool ok = cart_pump_wait(s) == Gen3HlinkDone;
    /* No READYs: a connect fails before any exchange, so this is the helper's only trace in the
     * log; nr high on the Normal wire means its SO never went low (M24, M25) */
    if(!ok) CART_LOG(s, "sync fail nr %u", (unsigned)gen3_hlink_notready(&s->hlink));
    return ok;
}

/* ---- Connect ---- */

static void cart_driver_stop(Gen3CartSession* s) {
    if(!s->driver_on) return;
    mlink_hw_stop(s->hw); /* no frame callback runs after this returns */
    s->pump_on = false;
    cart_lock(s);
    s->driver_on = false;
    s->status.cable_ok = mlink_hw_cable_ok(s->hw);
    s->status.link_up = false; /* nothing answers without the driver */
    cart_unlock(s);
}

/* The first Connect: one buffer, first for the helper image and then the jobs' sectors, as long as
 * the image but at least a sector, and no longer than the sender sends; then the client and the
 * jobs' state, which later Connects keep */
static void cart_setup(Gen3CartSession* s, uint64_t image_len) {
    uint32_t n = GEN3_CART_BUF_SIZE;
    if(image_len > n) n = image_len > GEN3_MB_MAX_LEN ? GEN3_MB_MAX_LEN : (uint32_t)image_len;
    s->buf = malloc(n);
    s->buf_len = n;
    const Gen3HTransport t = {
        .ctx = s,
        .exchange = cart_exchange,
        .sync = cart_sync,
    };
    const Gen3CartProgress p = {
        .cb = cart_progress,
        .ctx = s,
        .cancel = &s->cancel,
    };
    gen3_hcli_init(&s->cli, &t);
    gen3_cart_init(&s->cart, &s->cli, gen3_store_furi_api(s->store), s->buf, &p);
    s->cart_ready = true;
}

/* The helper image into the buffer, checked; false if it is missing or bad. The asset folder is
 * this app's: threads an app starts share its appid. */
static bool cart_helper_load(Gen3CartSession* s, uint32_t* len) {
    File* f = storage_file_alloc(s->storage);
    bool ok =
        storage_file_open(f, APP_ASSETS_PATH(GEN3_HELPER_ASSET), FSAM_READ, FSOM_OPEN_EXISTING);
    const uint64_t size = ok ? storage_file_size(f) : 0;
    if(ok && !s->cart_ready) cart_setup(s, size);
    ok = ok && size <= s->buf_len && storage_file_read(f, s->buf, (size_t)size) == size;
    storage_file_close(f); /* required even after a failed open */
    storage_file_free(f);
    *len = ok ? (uint32_t)size : 0;
    const Gen3MbImageCheck check = ok ? gen3_mb_image_check(s->buf, *len, GEN3_HELPER_PROTO) :
                                        Gen3MbImageBadLen;
    CART_LOG(
        s, "helper len %lu read %u check %u", (unsigned long)size, (unsigned)ok, (unsigned)check);
    return check == Gen3MbImageOk;
}

/* The wire as the log names it: the Normal wire's BIOS phase is ungated unless
 * GEN3_CART_MB_GATED */
static const char* cart_wire_name(MlinkWire wire) {
    if(wire != MlinkWireNormal) return "multi";
    return GEN3_CART_MB_GATED ? "normal gated" : "normal ungated";
}

/* Inits the sender for wire and starts the driver in stream mode with its first frame. False if
 * the start fails: TIM2 is held, or, on the MultiPlay wire, SI reads high. On the Normal wire the
 * BIOS phase runs ungated unless GEN3_CART_MB_GATED (multiboot-and-save.md §7.3). */
static bool cart_mb_start(Gen3CartSession* s, MlinkWire wire, uint32_t len) {
    MlinkTx first = {0};
    if(wire == MlinkWireNormal) mlink_hw_set_normal_gate(s->hw, GEN3_CART_MB_GATED);
    gen3_mb_init(&s->mb, wire, s->buf, len, furi_hal_random_get());
    gen3_mb_frame(&s->mb, NULL, &first, 0);
    s->wire = wire;
    s->core = Gen3CartCoreMb;
    s->gap_left = 0;
    s->gap_us = 0;
    return mlink_hw_start_wire(s->hw, MlinkModeStream, wire, gen3_cart_frame, s, &first);
}

/* Starts the driver on the wire the cable calls for: MultiPlay while SI reads low,
 * Normal otherwise (always with GEN3_CART_FORCE_NORMAL), with the sender's first frame */
static Gen3CartResult cart_driver_start(Gen3CartSession* s, uint32_t len) {
    cart_progress(s, Gen3CartStepMbWait, 0, 0);
    cart_set_cable(s, true); /* the Normal wire has no cable to check */
    if(s->cancel) return Gen3CartCancelled;
    MlinkWire wire = GEN3_CART_FORCE_NORMAL ?
                         MlinkWireNormal :
                         gen3_mb_wire_next(
                             MlinkWireMulti,
                             mlink_hw_check_cable(s->hw) == MlinkCableOk,
                             Gen3MbPhaseDetect);
    if(!cart_mb_start(s, wire, len)) {
        /* A MultiPlay start reads SI again: one now high goes Normal; else another link holds
         * TIM2 */
        if(wire != MlinkWireMulti || mlink_hw_check_cable(s->hw) == MlinkCableOk) {
            return Gen3CartLinkBusy;
        }
        wire = MlinkWireNormal;
        if(!cart_mb_start(s, wire, len)) return Gen3CartLinkBusy;
    }
    CART_LOG(s, "wire %s", cart_wire_name(wire));
    cart_lock(s);
    s->driver_on = true;
    s->status.cable_ok = true;
    cart_unlock(s);
    return Gen3CartOk;
}

/* The sender's status, for the hardware check of its budgets and of the Normal wire: the
 * not-ready slots and the latest reply (the GBA's last word; FFFF with nothing at the other end),
 * the words of the latest word failure, restarts counted and all, the slave's bytes, the latest
 * palette wait's busy replies (GEN3_MB_PALETTE_US), and the time from the first 0x0065 to the
 * 0x0075 (GEN3_MB_FINAL_US). The short fields come first. */
static void cart_log_mb(Gen3CartSession* s, const char* what, const Gen3MbStatus* st) {
    /* The expected and received words belong to the latest failure of these kinds only */
    char want[24] = "";
    switch(st->last_fail) {
    case Gen3MbFailHeader:
    case Gen3MbFailData:
    case Gen3MbFailHandshake:
    case Gen3MbFailLength:
    case Gen3MbFailCrc:
        snprintf(
            want, sizeof(want), " want %04x got %04x", (unsigned)st->fail_want, (unsigned)st->fail_got);
        break;
    default:
        break;
    }
    CART_LOG(
        s,
        "mb %s nr %u last %04x%s restarts %u all %u last_fail %u cc %02x rr %02x palette_busy %u "
        "final_us %lu sent %lu",
        what,
        (unsigned)st->notready,
        (unsigned)st->last_reply,
        want,
        (unsigned)st->restarts,
        (unsigned)st->all_restarts,
        (unsigned)st->last_fail,
        (unsigned)st->cc,
        (unsigned)st->rr,
        (unsigned)st->palette_busy,
        (unsigned long)st->final_us,
        (unsigned long)st->sent);
}

/* Runs the sender to Done or Failed, MbWait while it looks for the GBA and MbSend after. Each
 * restart is logged, counted or not; those between two polls in one line. While it waits for the
 * GBA, an `mb wait` line every GEN3_CART_MB_WAIT_LOG_MS; a cancel logs `mb cancel`. */
static Gen3CartResult cart_multiboot(Gen3CartSession* s, uint32_t len) {
    Gen3MbStatus st;
    uint16_t all_restarts = 0;
    uint8_t si_high = 0; /* polls in a row that called for the Normal wire */
    uint32_t wait_log = furi_get_tick();
    for(;;) {
        gen3_mb_status(&s->mb, &st);
        const bool waiting = st.phase == Gen3MbPhaseWait || st.phase == Gen3MbPhaseDetect;
        cart_lock(s);
        s->status.mb = st;
        s->status.step = waiting ? Gen3CartStepMbWait : Gen3CartStepMbSend;
        s->status.done = st.sent;
        s->status.total = st.total;
        cart_unlock(s);
        if(st.phase == Gen3MbPhaseDone) {
            cart_log_mb(s, "done", &st);
            return Gen3CartOk;
        }
        if(st.phase == Gen3MbPhaseFailed) {
            cart_log_mb(s, "failed", &st);
            return Gen3CartMultibootFailed;
        }
        if(st.all_restarts != all_restarts) {
            all_restarts = st.all_restarts;
            cart_log_mb(s, "restart", &st);
        }
        if(s->cancel) {
            cart_log_mb(s, "cancel", &st);
            return Gen3CartCancelled;
        }
        if(waiting && furi_get_tick() - wait_log >= furi_ms_to_ticks(GEN3_CART_MB_WAIT_LOG_MS)) {
            wait_log = furi_get_tick();
            cart_log_mb(s, "wait", &st);
        }
        /* SI high while the GBA hasn't answered, GEN3_CART_WIRE_POLLS polls in a row: not a GBA
         * cable. Only on the MultiPlay wire does the cable check mean anything. */
        if(gen3_mb_wire_next(s->wire, mlink_hw_cable_ok(s->hw), st.phase) == s->wire) {
            si_high = 0;
        } else if(++si_high >= GEN3_CART_WIRE_POLLS) {
            cart_driver_stop(s);
            if(!cart_mb_start(s, MlinkWireNormal, len)) return Gen3CartLinkBusy;
            cart_lock(s);
            s->driver_on = true;
            s->status.cable_ok = true;
            cart_unlock(s);
            all_restarts = 0;
            si_high = 0;
            CART_LOG(s, "wire %s (SI high)", cart_wire_name(MlinkWireNormal));
        }
        furi_delay_ms(GEN3_CART_MB_POLL_MS);
    }
}

/* Sends the helper (again), then connects to it and attaches the cart */
static Gen3CartResult cart_job_connect(Gen3CartSession* s) {
    cart_driver_stop(s);
    uint32_t len = 0;
    if(!cart_helper_load(s, &len)) return Gen3CartNoHelper;
    Gen3CartResult r = cart_driver_start(s, len);
    if(r == Gen3CartOk) r = cart_multiboot(s, len);
    if(r != Gen3CartOk) {
        cart_driver_stop(s);
        return r;
    }

    /* The pump takes the link over; the buffer is the jobs' sector buffer from here on. On the
     * Normal wire the ready gate applies from now: the helper says when it is ready. */
    if(s->wire == MlinkWireNormal) {
        mlink_hw_set_normal_gate(s->hw, true);
        if(!GEN3_CART_MB_GATED) CART_LOG(s, "wire %s", "normal gated (helper)");
    }
    gen3_hlink_init(&s->hlink);
    __DMB();
    s->core = Gen3CartCoreHlink;
    s->pump_on = true;

    /* The GBA starts the helper a few seconds after the last multiboot word */
    gen3_cart_expect_boot(&s->cart);
    r = gen3_cart_connect(&s->cart);
    if(r != Gen3CartOk) return r;
    cart_log_cart(s);
    bool dropped = false;
    r = gen3_cart_attach(&s->cart, s->stamp, &dropped);
    cart_lock(s);
    s->status.dropped = dropped;
    s->status.finished = gen3_cart_finished(&s->cart);
    cart_unlock(s);
    return r;
}

/* ---- The other jobs ---- */

static Gen3CartResult cart_job_restore_check(Gen3CartSession* s, const char* name) {
    Gen3SaveTrainer t;
    bool has = false;
    memset(&t, 0, sizeof(t));
    const Gen3CartResult r = gen3_cart_restore_check(&s->cart, name, &t, &has);
    cart_lock(s);
    s->status.restore_trainer = t;
    s->status.restore_has_trainer = has;
    cart_unlock(s);
    return r;
}

static Gen3CartResult cart_job(Gen3CartSession* s, Gen3CartJob job) {
    Gen3Cart* c = &s->cart;
    const char* arg = s->req_has_arg ? s->req_arg : NULL;
    if(job == Gen3CartJobConnect) return cart_job_connect(s);
    if(!s->cart_ready) return Gen3CartBadArg; /* no Connect has loaded the helper yet */
    switch(job) {
    case Gen3CartJobBackup:
        return gen3_cart_backup(c, s->stamp);
    case Gen3CartJobWrite:
        return gen3_cart_write(c, s->stamp);
    case Gen3CartJobDiscard:
        return gen3_cart_discard(c);
    case Gen3CartJobRestoreCheck:
        return cart_job_restore_check(s, arg);
    case Gen3CartJobRestore:
        return gen3_cart_restore(c, arg, s->stamp);
    case Gen3CartJobRom:
        return gen3_cart_rom_dump(c, arg && strcmp(arg, "resume") == 0);
    case Gen3CartJobReconnect: {
        const Gen3CartResult r = gen3_cart_reconnect(c);
        cart_lock(s);
        s->status.finished = gen3_cart_finished(c);
        cart_unlock(s);
        /* Ok or Changed: its connect ran to its end */
        if(r == Gen3CartOk || r == Gen3CartChanged) cart_log_cart(s);
        return r;
    }
    case Gen3CartJobConnect:
        break;
    }
    return Gen3CartBadArg;
}

/* The backup stamp, YYYYMMDD-HHMMSS, from the RTC */
static void cart_stamp(Gen3CartSession* s) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    snprintf(
        s->stamp,
        sizeof(s->stamp),
        "%04u%02u%02u-%02u%02u%02u",
        (unsigned)(dt.year % 10000u),
        (unsigned)(dt.month % 100u),
        (unsigned)(dt.day % 100u),
        (unsigned)(dt.hour % 100u),
        (unsigned)(dt.minute % 100u),
        (unsigned)(dt.second % 100u));
}

/* ---- Worker ---- */

static void cart_run_one(Gen3CartSession* s, Gen3CartJob job) {
    cart_stamp(s);
    const uint32_t resends = s->cli.resends;
    CART_LOG(s, "job %s start", cart_job_name(job));
    const Gen3CartResult r = cart_job(s, job);
    CART_LOG(
        s,
        "job %s end %s resends %lu finished %u",
        cart_job_name(job),
        cart_result_name(r),
        (unsigned long)(s->cli.resends - resends),
        (unsigned)s->status.finished);
    if(s->cart_ready && job != Gen3CartJobDiscard && job != Gen3CartJobRestoreCheck &&
       job != Gen3CartJobRom) {
        cart_log_slots(s);
    }

    cart_lock(s);
    s->status.result = r;
    s->status.busy = false;
    cart_unlock(s);
}

static int32_t cart_worker(void* ctx) {
    Gen3CartSession* s = ctx;
    for(;;) {
        const uint32_t flags = furi_thread_flags_wait(
            GEN3_CART_FLAG_JOB | GEN3_CART_FLAG_EXIT, FuriFlagWaitAny, FuriWaitForever);
        if(flags & FuriFlagError) continue;
        if(flags & GEN3_CART_FLAG_EXIT) break;

        cart_lock(s);
        const bool pending = s->req_pending;
        const Gen3CartJob job = s->req_job;
        s->req_pending = false;
        if(pending) {
            s->status.busy = true;
            s->status.job = job;
            s->status.step = Gen3CartStepNone;
            s->status.done = 0;
            s->status.total = 0;
            s->status.dropped = false;
            s->status.finished = Gen3CartFinishedNone;
            s->status.restore_has_trainer = false;
            /* A Connect's multiboot starts afresh: never show the last run's */
            if(job == Gen3CartJobConnect) memset(&s->status.mb, 0, sizeof(s->status.mb));
        }
        cart_unlock(s);
        if(pending) cart_run_one(s, job);
    }

    const uint32_t free_stack = furi_thread_get_stack_space(furi_thread_get_current_id());
    CART_LOG(s, "worker stack free %lu", (unsigned long)free_stack);
    FURI_LOG_D(TAG, "%s stack free %lu", GEN3_CART_THREAD_NAME, (unsigned long)free_stack);
    return 0;
}

/* ---- API ---- */

Gen3CartSession* gen3_cart_session_alloc(void* gblink_handle) {
    furi_check(gblink_handle);
    const MlinkPins pins = {
        .sc = gblink_pin_get_by_gpiopin(gblink_handle, PIN_CLK),
        .sd = gblink_pin_get_by_gpiopin(gblink_handle, PIN_SD),
        .so = gblink_pin_get_by_gpiopin(gblink_handle, PIN_SEROUT),
        .si = gblink_pin_get_by_gpiopin(gblink_handle, PIN_SERIN),
    };

    Gen3CartSession* s = malloc(sizeof(Gen3CartSession));
    memset(s, 0, sizeof(*s));
    s->hw = mlink_hw_alloc(&pins);
    s->lock = furi_mutex_alloc(FuriMutexTypeNormal);
    s->store = gen3_store_furi_alloc(); /* makes apps_data/pokemon, the log's folder, too */
    s->storage = furi_record_open(RECORD_STORAGE);
    s->log = storage_file_alloc(s->storage);
    cart_log_open(s);
    s->thread =
        furi_thread_alloc_ex(GEN3_CART_THREAD_NAME, GEN3_CART_THREAD_STACK, cart_worker, s);
    furi_thread_start(s->thread);
    return s;
}

void gen3_cart_session_free(Gen3CartSession* s) {
    furi_check(s);
    s->cancel = true;
    furi_thread_flags_set(furi_thread_get_id(s->thread), GEN3_CART_FLAG_EXIT);
    furi_thread_join(s->thread);
    furi_thread_free(s->thread);

    mlink_hw_free(s->hw); /* stops the driver: no frame callback runs after this */
    if(s->log_open) storage_file_close(s->log);
    storage_file_free(s->log);
    furi_record_close(RECORD_STORAGE);
    gen3_store_furi_free(s->store);
    free(s->buf);
    furi_mutex_free(s->lock);
    free(s);
}

bool gen3_cart_session_run(Gen3CartSession* s, Gen3CartJob job, const char* arg) {
    furi_check(s);
    if((size_t)job > (size_t)Gen3CartJobReconnect) return false;
    cart_lock(s);
    const bool idle = !s->status.busy && !s->req_pending;
    if(idle) {
        s->req_pending = true;
        s->req_job = job;
        s->req_has_arg = arg != NULL;
        if(arg) {
            size_t n = strlen(arg);
            if(n > GEN3_CART_NAME_MAX) n = GEN3_CART_NAME_MAX;
            memcpy(s->req_arg, arg, n);
            s->req_arg[n] = '\0';
        }
        s->cancel = false;
    }
    cart_unlock(s);
    if(idle) furi_thread_flags_set(furi_thread_get_id(s->thread), GEN3_CART_FLAG_JOB);
    return idle;
}

void gen3_cart_session_cancel(Gen3CartSession* s) {
    furi_check(s);
    s->cancel = true;
}

void gen3_cart_session_status(Gen3CartSession* s, Gen3CartSessionStatus* out) {
    furi_check(s);
    furi_check(out);
    cart_lock(s);
    *out = s->status;
    if(s->req_pending) {
        out->busy = true;
        out->job = s->req_job;
        /* As the worker will once it takes the Connect */
        if(s->req_job == Gen3CartJobConnect) memset(&out->mb, 0, sizeof(out->mb));
    }
    if(s->driver_on) out->cable_ok = mlink_hw_cable_ok(s->hw);
    cart_unlock(s);
}

Gen3Cart* gen3_cart_session_cart(Gen3CartSession* s) {
    furi_check(s);
    return &s->cart;
}
