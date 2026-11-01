#include <src/gen3/gen3_session_furi.h>

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#include <gblink/include/gblink.h>
#include <gblink/include/gblink_pinconf.h>

#include <src/gen3/gen3_link_log.h>

#define TAG "gen3_session"

#define GEN3_LOG_FILE_NAME   "gen3_link.log"
#define GEN3_LOG_FILE_HEADER "# gen3 link log v1\n"

#define GEN3_LOG_THREAD_NAME  "Gen3Log"
#define GEN3_LOG_THREAD_STACK 1024
#define GEN3_LOG_DRAIN_MS     100
#define GEN3_LOG_FLAG_STOP    (1u << 0)

/* The club's identity in each mode (D7: the Flipper stays FLIPPER / 22222 / SID 0 in both). */
static const Gen3ClubConfig gen3_session_test_cfg = {
    .name = GEN3_TEST_TRAINER_NAME,
    .tid = GEN3_TEST_TRAINER_TID,
    .sid = GEN3_TEST_TRAINER_SID,
    .gender = 0,
    .trading = false};
static const Gen3ClubConfig gen3_session_trade_cfg = {
    .name = GEN3_TEST_TRAINER_NAME,
    .tid = GEN3_TEST_TRAINER_TID,
    .sid = GEN3_TEST_TRAINER_SID,
    .gender = 0,
    .trading = true};

struct Gen3Session {
    MlinkHw* hw;
    bool running;
    bool trade_mode; /* the latest start was gen3_session_start_trade */

    /* Written by the frame callback (ISR) while running; by start before the driver runs. The
     * app reaches it only through the hand-off calls, inside a PRIMASK section. */
    Gen3Club club;
    Gen3LogRing ring;
    uint32_t log_frame; /* frames completed this run, the log's frame number */

    /* Status double buffer: the ISR fills status[(seq + 1) & 1], then bumps seq. Readers copy
     * status[seq & 1] and retry if seq moved meanwhile. seq is the "volatile index" widened to a
     * counter, so a reader also notices two publishes during one copy. */
    Gen3ClubStatus status[2];
    volatile uint32_t status_seq;

    /* Log file: opened and closed in app context; written only by the log thread in between. */
    Storage* storage;
    File* log_file;
    bool log_open;
    bool log_wrote; /* log thread only: this drain wrote at least one line */
    FuriThread* log_thread;
};

/* ---- Frame callback: TIM2 interrupt context. No allocation, logging or blocking. ---- */

static void gen3_session_frame(void* ctx, const MlinkRx* rx, MlinkTx* next) {
    Gen3Session* s = ctx;
    const MlinkTx sent = *next; /* next arrives holding the frame that rx answers */

    gen3_club_frame(&s->club, rx, next);

    const uint32_t seq = s->status_seq;
    Gen3ClubStatus* status = &s->status[(seq + 1u) & 1u];
    gen3_club_status(&s->club, status);

    gen3_log_frame(&s->ring, s->log_frame++, (uint8_t)status->phase, &sent, rx);

    __DMB(); /* the status must be complete before readers can pick it */
    s->status_seq = seq + 1u;
}

/* ---- Log thread ---- */

static bool gen3_session_log_write(void* ctx, const char* line, size_t len) {
    Gen3Session* s = ctx;
    if(!s->log_open) return false; /* no file: gen3_log_drain discards from now on */
    s->log_wrote = true;
    return storage_file_write(s->log_file, line, len) == len;
}

static int32_t gen3_session_log_thread(void* ctx) {
    Gen3Session* s = ctx;
    bool stop = false;

    while(!stop) {
        const uint32_t flags =
            furi_thread_flags_wait(GEN3_LOG_FLAG_STOP, FuriFlagWaitAny, GEN3_LOG_DRAIN_MS);
        stop = !(flags & FuriFlagError) && (flags & GEN3_LOG_FLAG_STOP);

        /* On stop the driver is already off, so this last drain empties the ring for good. */
        s->log_wrote = false;
        gen3_log_drain(&s->ring, gen3_session_log_write, s);
        /* Keep what is written so far if the app dies before stop closes the file. */
        if(s->log_wrote && !gen3_log_failed(&s->ring)) storage_file_sync(s->log_file);
    }

    FURI_LOG_D(
        TAG,
        "%s stack free %lu",
        GEN3_LOG_THREAD_NAME,
        (unsigned long)furi_thread_get_stack_space(furi_thread_get_current_id()));
    return 0;
}

/* App context, before the log thread starts. */
static void gen3_session_log_open(Gen3Session* s) {
    FuriString* path = furi_string_alloc_set(APP_DATA_PATH(GEN3_LOG_FILE_NAME));
    storage_common_resolve_path_and_ensure_app_directory(s->storage, path);

    s->log_open = storage_file_open(
        s->log_file, furi_string_get_cstr(path), FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(s->log_open) {
        const size_t len = strlen(GEN3_LOG_FILE_HEADER);
        s->log_open = storage_file_write(s->log_file, GEN3_LOG_FILE_HEADER, len) == len;
    }
    if(!s->log_open) {
        FURI_LOG_W(TAG, "No log: can't write %s", furi_string_get_cstr(path));
        storage_file_close(s->log_file); /* required even after a failed open */
    }
    furi_string_free(path);
}

/* App context, after the log thread has exited. */
static void gen3_session_log_close(Gen3Session* s) {
    if(!s->log_open) return;
    if(!gen3_log_failed(&s->ring)) {
        char line[40];
        const int len = snprintf(
            line, sizeof(line), "# dropped %lu\n", (unsigned long)gen3_log_dropped(&s->ring));
        if(len > 0 && (size_t)len < sizeof(line)) {
            storage_file_write(s->log_file, line, (size_t)len);
        }
    }
    storage_file_close(s->log_file);
    s->log_open = false;
}

/* ---- API ---- */

static void gen3_session_reset_status(Gen3Session* s) {
    memset(s->status, 0, sizeof(s->status));
    s->status[0].phase = Gen3ClubPhaseWaiting;
    s->status[1].phase = Gen3ClubPhaseWaiting;
}

Gen3Session* gen3_session_alloc(void* gblink_handle) {
    furi_check(gblink_handle);
    const MlinkPins pins = {
        .sc = gblink_pin_get_by_gpiopin(gblink_handle, PIN_CLK),
        .sd = gblink_pin_get_by_gpiopin(gblink_handle, PIN_SD),
        .so = gblink_pin_get_by_gpiopin(gblink_handle, PIN_SEROUT),
        .si = gblink_pin_get_by_gpiopin(gblink_handle, PIN_SERIN),
    };

    Gen3Session* s = malloc(sizeof(Gen3Session));
    memset(s, 0, sizeof(*s));
    s->hw = mlink_hw_alloc(&pins);
    s->storage = furi_record_open(RECORD_STORAGE);
    s->log_file = storage_file_alloc(s->storage);
    gen3_session_reset_status(s);
    return s;
}

void gen3_session_free(Gen3Session* s) {
    furi_check(s);
    gen3_session_stop(s);
    storage_file_free(s->log_file);
    furi_record_close(RECORD_STORAGE);
    mlink_hw_free(s->hw);
    free(s);
}

MlinkCable gen3_session_check_cable(Gen3Session* s) {
    furi_check(s);
    return mlink_hw_check_cable(s->hw);
}

static bool gen3_session_start_with(Gen3Session* s, const Gen3ClubConfig* cfg, bool national_dex) {
    furi_check(s);
    /* Already running: true only in the asked mode, so a Link Test run never passes for a trade
     * session, nor the other way round */
    if(s->running) return s->trade_mode == cfg->trading;

    MlinkTx first;

    /* The driver is off, so nothing else touches the club, the ring or the status yet. */
    gen3_club_init(&s->club, cfg);
    gen3_club_set_national_dex(&s->club, national_dex);
    gen3_log_init(&s->ring);
    s->log_frame = 0;
    gen3_session_reset_status(s);
    gen3_club_frame(&s->club, NULL, &first);

    if(!mlink_hw_start(s->hw, gen3_session_frame, s, &first)) {
        FURI_LOG_W(TAG, "Link not started: cable not OK or TIM2 busy");
        return false;
    }
    s->running = true;
    s->trade_mode = cfg->trading;

    gen3_session_log_open(s);
    s->log_thread = furi_thread_alloc_ex(
        GEN3_LOG_THREAD_NAME, GEN3_LOG_THREAD_STACK, gen3_session_log_thread, s);
    furi_thread_start(s->log_thread);
    return true;
}

bool gen3_session_start(Gen3Session* s) {
    return gen3_session_start_with(s, &gen3_session_test_cfg, false);
}

bool gen3_session_start_trade(Gen3Session* s, bool national_dex) {
    return gen3_session_start_with(s, &gen3_session_trade_cfg, national_dex);
}

bool gen3_session_running(Gen3Session* s) {
    return s && s->running;
}

bool gen3_session_trading(Gen3Session* s) {
    return s && s->running && s->trade_mode;
}

void gen3_session_stop(Gen3Session* s) {
    furi_check(s);
    if(!s->running) return;

    mlink_hw_stop(s->hw); /* no frame callback runs after this returns */

    furi_thread_flags_set(furi_thread_get_id(s->log_thread), GEN3_LOG_FLAG_STOP);
    furi_thread_join(s->log_thread);
    furi_thread_free(s->log_thread);
    s->log_thread = NULL;

    gen3_session_log_close(s);
    s->running = false;
}

void gen3_session_status(Gen3Session* s, Gen3ClubStatus* out, bool* log_ok, bool* cable_ok) {
    furi_check(s);
    furi_check(out);
    uint32_t seq;

    do {
        seq = s->status_seq;
        __DMB(); /* read seq before the buffer */
        *out = s->status[seq & 1u];
        __DMB(); /* finish the copy before re-reading seq */
    } while(seq != s->status_seq);

    if(log_ok) *log_ok = s->log_open && !gen3_log_failed(&s->ring);
    if(cable_ok) *cable_ok = mlink_hw_cable_ok(s->hw);
}

/* ---- App <-> club hand-off ----
 * Each PRIMASK section masks every interrupt, TIM2 included, around exactly one gen3_club_* call,
 * so the frame callback never sees a half-made change. Inside: at most 104 bytes copied (one
 * 100-byte Pokemon plus scalars), nothing that blocks, no memcmp, no decrypt: about 10-13 us at
 * worst with the firmware's byte-loop memcpy. That only delays the start of a transfer the TIM2
 * tick would begin meanwhile; the bit timing and the frame cadence are unaffected. */

bool gen3_session_set_offer_traits(
    Gen3Session* s,
    const uint8_t* offer,
    uint32_t trades_seen,
    uint8_t traits) {
    furi_check(s);
    if(!s->running) return false;

    /* The traits come in computed: the decrypt they need ran on the app thread, before this */
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    /* 100 B + has_offer + traits: 102 B */
    const bool ok = gen3_club_set_offer_traits(&s->club, offer, trades_seen, traits);
    __set_PRIMASK(primask);
    return ok;
}

void gen3_session_get_received(Gen3Session* s, uint8_t out[GEN3_PARTY_SIZE], uint32_t* trades_done) {
    furi_check(s);
    furi_check(out);

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    gen3_club_get_received(&s->club, out, trades_done); /* 100 B + 4 B */
    __set_PRIMASK(primask);
}

bool gen3_session_set_editing(Gen3Session* s, bool on, uint32_t trades_seen) {
    furi_check(s);
    if(!s->running) return true;

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const bool ok = gen3_club_set_editing(&s->club, on, trades_seen); /* 1 B */
    __set_PRIMASK(primask);
    return ok;
}

void gen3_session_set_national_dex(Gen3Session* s, bool on) {
    furi_check(s);

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    gen3_club_set_national_dex(&s->club, on); /* 1 B */
    __set_PRIMASK(primask);
}

void gen3_session_request_leave(Gen3Session* s) {
    furi_check(s);
    if(!s->running) return;

    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    gen3_club_request_leave(&s->club); /* 2 B */
    __set_PRIMASK(primask);
}
