#include <src/views/gen3_cart_progress.h>

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view.h>

#define GEN3_CART_PROGRESS_TICK_MS 250

typedef struct {
    View* view;
    FuriTimer* timer;
    Gen3CartProgressCallback back_cb;
    void* back_ctx;
    Gen3CartProgressCallback tick_cb;
    void* tick_ctx;
} Gen3CartProgressView;

typedef struct {
    char title[22];
    char line1[26];
    char line2[26];
    char hint[26];
    int8_t percent;
} Gen3CartProgressModel;

static void gen3_cart_progress_draw_callback(Canvas* canvas, void* view_model) {
    furi_assert(view_model);
    const Gen3CartProgressModel* model = view_model;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, model->title);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 26, AlignCenter, AlignBottom, model->line1);
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignBottom, model->line2);
    if(model->percent >= 0) {
        elements_progress_bar(canvas, 4, 46, 120, (float)model->percent / 100.0f);
    }
    canvas_draw_str_aligned(canvas, 64, 62, AlignCenter, AlignBottom, model->hint);
}

/* GUI thread */
static bool gen3_cart_progress_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    Gen3CartProgressView* pv = context;

    if(event->key != InputKeyBack || !pv->back_cb) return false;
    if(event->type == InputTypeShort) pv->back_cb(pv->back_ctx);
    return true;
}

/* Timer thread, every GEN3_CART_PROGRESS_TICK_MS while the view is shown */
static void gen3_cart_progress_timer_callback(void* context) {
    furi_assert(context);
    Gen3CartProgressView* pv = context;

    if(pv->tick_cb) pv->tick_cb(pv->tick_ctx);
}

static void gen3_cart_progress_enter_callback(void* context) {
    furi_assert(context);
    Gen3CartProgressView* pv = context;

    pv->timer = furi_timer_alloc(gen3_cart_progress_timer_callback, FuriTimerTypePeriodic, pv);
    furi_timer_start(pv->timer, furi_ms_to_ticks(GEN3_CART_PROGRESS_TICK_MS));
}

static void gen3_cart_progress_exit_callback(void* context) {
    furi_assert(context);
    Gen3CartProgressView* pv = context;

    if(pv->timer) {
        furi_timer_stop(pv->timer);
        furi_timer_free(pv->timer);
        pv->timer = NULL;
    }
}

void* gen3_cart_progress_alloc(ViewDispatcher* vd, uint32_t view_id) {
    Gen3CartProgressView* pv = malloc(sizeof(Gen3CartProgressView));
    memset(pv, 0, sizeof(*pv));

    pv->view = view_alloc();
    view_set_context(pv->view, pv);
    view_allocate_model(pv->view, ViewModelTypeLocking, sizeof(Gen3CartProgressModel));
    with_view_model(
        pv->view,
        Gen3CartProgressModel * model,
        {
            memset(model, 0, sizeof(*model));
            model->percent = -1;
        },
        false);
    view_set_draw_callback(pv->view, gen3_cart_progress_draw_callback);
    view_set_input_callback(pv->view, gen3_cart_progress_input_callback);
    view_set_enter_callback(pv->view, gen3_cart_progress_enter_callback);
    view_set_exit_callback(pv->view, gen3_cart_progress_exit_callback);

    view_dispatcher_add_view(vd, view_id, pv->view);
    return pv;
}

void gen3_cart_progress_free(ViewDispatcher* vd, uint32_t view_id, void* p) {
    furi_assert(p);
    Gen3CartProgressView* pv = p;

    view_dispatcher_remove_view(vd, view_id);
    gen3_cart_progress_exit_callback(pv); /* frees the timer if the view was still shown */
    view_free(pv->view);
    free(pv);
}

void gen3_cart_progress_set(
    void* p,
    const char* title,
    const char* line1,
    const char* line2,
    int8_t percent) {
    furi_assert(p);
    Gen3CartProgressView* pv = p;

    with_view_model(
        pv->view,
        Gen3CartProgressModel * model,
        {
            snprintf(model->title, sizeof(model->title), "%s", title ? title : "");
            snprintf(model->line1, sizeof(model->line1), "%s", line1 ? line1 : "");
            snprintf(model->line2, sizeof(model->line2), "%s", line2 ? line2 : "");
            model->percent = percent > 100 ? 100 : percent;
        },
        true);
}

void gen3_cart_progress_set_hint(void* p, const char* hint) {
    furi_assert(p);
    Gen3CartProgressView* pv = p;

    with_view_model(
        pv->view,
        Gen3CartProgressModel * model,
        { snprintf(model->hint, sizeof(model->hint), "%s", hint ? hint : ""); },
        true);
}

void gen3_cart_progress_set_back(void* p, Gen3CartProgressCallback cb, void* ctx) {
    furi_assert(p);
    Gen3CartProgressView* pv = p;

    pv->back_cb = cb;
    pv->back_ctx = ctx;
}

void gen3_cart_progress_set_tick(void* p, Gen3CartProgressCallback cb, void* ctx) {
    furi_assert(p);
    Gen3CartProgressView* pv = p;

    pv->tick_cb = cb;
    pv->tick_ctx = ctx;
}
