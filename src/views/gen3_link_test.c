#include <src/views/gen3_link_test.h>

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <gui/canvas.h>
#include <gui/view.h>

#include <src/gen3/gen3_text.h>

#define GEN3_LINK_TEST_TICK_MS 250

typedef struct {
    View* view;
    FuriTimer* timer;
    Gen3LinkTestTickCallback tick_callback;
    void* tick_context;
} Gen3LinkTest;

typedef struct {
    bool cable_ok;
    bool log_ok;
    Gen3ClubPhase phase;
    Gen3ClubError last_error;
    bool partner_valid;
    char partner_name[12]; /* decoded Gen3LinkPlayer.name[11] + NUL */
    uint16_t partner_tid;
    uint32_t frames, checksum_errors, timeouts;
} Gen3LinkTestModel;

static const char* gen3_link_test_phase_text(Gen3ClubPhase phase, Gen3ClubError error) {
    switch(phase) {
    case Gen3ClubPhaseWaiting:
        return "waiting for GBA";
    case Gen3ClubPhaseLinked:
        return "linked";
    case Gen3ClubPhasePlayers:
        return "players exchanged";
    case Gen3ClubPhaseCards:
        return "trainer cards";
    case Gen3ClubPhaseRoom:
        return "in Trade Center";
    case Gen3ClubPhaseClosing:
        return "closing";
    case Gen3ClubPhaseExchange:
        return "trade menu (loading)";
    case Gen3ClubPhaseMenu:
        return "trade menu";
    case Gen3ClubPhaseTrade:
        return "trading";
    case Gen3ClubPhaseSave:
        return "GBA saving";
    case Gen3ClubPhaseError: /* a checksum error never enters it: it only counts in Err */
        switch(error) {
        case Gen3ClubErrorBadPlayerBlock:
            return "error: bad data";
        case Gen3ClubErrorOtherRoom:
            return "GBA chose other room";
        case Gen3ClubErrorUnexpected:
            return "error: unexpected";
        default:
            return "error";
        }
    }
    return "?";
}

static void gen3_link_test_draw_callback(Canvas* canvas, void* view_model) {
    furi_assert(view_model);
    const Gen3LinkTestModel* model = view_model;
    char line[48];

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Gen III Link Test");

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 21, model->cable_ok ? "Cable: OK" : "Cable: plug in/swap ends");

    snprintf(
        line,
        sizeof(line),
        "Link: %s",
        gen3_link_test_phase_text(model->phase, model->last_error));
    canvas_draw_str(canvas, 0, 31, line);

    if(model->partner_valid) {
        snprintf(
            line, sizeof(line), "%s TID %05u", model->partner_name, (unsigned)model->partner_tid);
        canvas_draw_str(canvas, 0, 41, line);
    }

    snprintf(
        line,
        sizeof(line),
        "Frm %lu Err %lu T/O %lu",
        (unsigned long)model->frames,
        (unsigned long)model->checksum_errors,
        (unsigned long)model->timeouts);
    canvas_draw_str(canvas, 0, 51, line);

    canvas_draw_str(canvas, 0, 61, model->log_ok ? "Log: on" : "Log: off");
}

/* Timer thread, every GEN3_LINK_TEST_TICK_MS while the view is shown. */
static void gen3_link_test_timer_callback(void* context) {
    furi_assert(context);
    Gen3LinkTest* link_test = context;

    if(link_test->tick_callback) link_test->tick_callback(link_test->tick_context);
}

static void gen3_link_test_enter_callback(void* context) {
    furi_assert(context);
    Gen3LinkTest* link_test = context;

    link_test->timer =
        furi_timer_alloc(gen3_link_test_timer_callback, FuriTimerTypePeriodic, link_test);
    furi_timer_start(link_test->timer, furi_ms_to_ticks(GEN3_LINK_TEST_TICK_MS));
}

static void gen3_link_test_exit_callback(void* context) {
    furi_assert(context);
    Gen3LinkTest* link_test = context;

    if(link_test->timer) {
        furi_timer_stop(link_test->timer);
        furi_timer_free(link_test->timer);
        link_test->timer = NULL;
    }
}

void* gen3_link_test_alloc(ViewDispatcher* view_dispatcher, uint32_t view_id) {
    Gen3LinkTest* link_test = malloc(sizeof(Gen3LinkTest));
    memset(link_test, 0, sizeof(*link_test));

    link_test->view = view_alloc();
    view_set_context(link_test->view, link_test);
    view_allocate_model(link_test->view, ViewModelTypeLocking, sizeof(Gen3LinkTestModel));
    with_view_model(
        link_test->view,
        Gen3LinkTestModel * model,
        {
            memset(model, 0, sizeof(*model));
            model->phase = Gen3ClubPhaseWaiting;
        },
        false);
    view_set_draw_callback(link_test->view, gen3_link_test_draw_callback);
    view_set_enter_callback(link_test->view, gen3_link_test_enter_callback);
    view_set_exit_callback(link_test->view, gen3_link_test_exit_callback);

    view_dispatcher_add_view(view_dispatcher, view_id, link_test->view);
    return link_test;
}

void gen3_link_test_free(ViewDispatcher* view_dispatcher, uint32_t view_id, void* link_test_ctx) {
    furi_assert(link_test_ctx);
    Gen3LinkTest* link_test = link_test_ctx;

    view_dispatcher_remove_view(view_dispatcher, view_id);
    gen3_link_test_exit_callback(link_test); /* frees the timer if the view was still shown */
    view_free(link_test->view);
    free(link_test);
}

void gen3_link_test_set_tick_callback(
    void* link_test_ctx,
    Gen3LinkTestTickCallback callback,
    void* context) {
    furi_assert(link_test_ctx);
    Gen3LinkTest* link_test = link_test_ctx;

    link_test->tick_callback = callback;
    link_test->tick_context = context;
}

void gen3_link_test_update(
    void* link_test_ctx,
    bool cable_ok,
    const Gen3ClubStatus* status,
    bool log_ok) {
    furi_assert(link_test_ctx);
    furi_assert(status);
    Gen3LinkTest* link_test = link_test_ctx;

    with_view_model(
        link_test->view,
        Gen3LinkTestModel * model,
        {
            model->cable_ok = cable_ok;
            model->log_ok = log_ok;
            model->phase = status->phase;
            model->last_error = status->last_error;
            model->partner_valid = status->partner_valid;
            gen3_text_decode(
                status->partner.name,
                sizeof(status->partner.name),
                model->partner_name,
                sizeof(model->partner_name));
            model->partner_tid = (uint16_t)(status->partner.trainer_id & 0xFFFFu);
            model->frames = status->frames;
            model->checksum_errors = status->link.checksum_errors;
            model->timeouts = status->link.timeouts;
        },
        true);
}
