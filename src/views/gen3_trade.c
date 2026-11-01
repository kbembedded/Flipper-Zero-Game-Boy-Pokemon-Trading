#include <src/views/gen3_trade.h>

#include <stdio.h>
#include <string.h>

#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view.h>

typedef struct {
    View* view;
    Gen3TradeEditCallback edit_callback;
    void* edit_context;
} Gen3TradeView;

/* 128x64: title, three guide lines, the Flipper's Pokemon, and "Edit" while OK works */
static void gen3_trade_view_draw_callback(Canvas* canvas, void* view_model) {
    furi_assert(view_model);
    const Gen3TradeViewState* model = view_model;

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 0, 10, "Gen III Trade");

    canvas_set_font(canvas, FontSecondary);
    if(model->errors > 0) {
        char errors[12]; /* "E" + up to 10 digits + NUL */
        snprintf(errors, sizeof(errors), "E%lu", (unsigned long)model->errors);
        canvas_draw_str_aligned(canvas, 128, 10, AlignRight, AlignBottom, errors);
    }
    for(int i = 0; i < GEN3_GUIDE_LINES; i++) {
        canvas_draw_str(canvas, 0, 21 + 10 * i, model->guide[i]);
    }
    canvas_draw_str(canvas, 0, 51, model->flipper_line);

    if(model->can_edit) elements_button_center(canvas, "Edit");
}

/* App thread. A short OK press while can_edit edits; Back goes to the scene manager. */
static bool gen3_trade_view_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    Gen3TradeView* trade_view = context;
    bool can_edit = false;

    if(event->type != InputTypeShort || event->key != InputKeyOk) return false;

    with_view_model(
        trade_view->view, Gen3TradeViewState * model, { can_edit = model->can_edit; }, false);
    if(!can_edit) return false;

    if(trade_view->edit_callback) trade_view->edit_callback(trade_view->edit_context);
    return true;
}

void* gen3_trade_view_alloc(ViewDispatcher* view_dispatcher, uint32_t view_id) {
    Gen3TradeView* trade_view = malloc(sizeof(Gen3TradeView));
    memset(trade_view, 0, sizeof(*trade_view));

    trade_view->view = view_alloc();
    view_set_context(trade_view->view, trade_view);
    view_allocate_model(trade_view->view, ViewModelTypeLocking, sizeof(Gen3TradeViewState));
    with_view_model(
        trade_view->view, Gen3TradeViewState * model, { memset(model, 0, sizeof(*model)); }, false);
    view_set_draw_callback(trade_view->view, gen3_trade_view_draw_callback);
    view_set_input_callback(trade_view->view, gen3_trade_view_input_callback);

    view_dispatcher_add_view(view_dispatcher, view_id, trade_view->view);
    return trade_view;
}

void gen3_trade_view_free(ViewDispatcher* view_dispatcher, uint32_t view_id, void* view) {
    furi_assert(view);
    Gen3TradeView* trade_view = view;

    view_dispatcher_remove_view(view_dispatcher, view_id);
    view_free(trade_view->view);
    free(trade_view);
}

void gen3_trade_view_set_edit_callback(void* view, Gen3TradeEditCallback cb, void* context) {
    furi_assert(view);
    Gen3TradeView* trade_view = view;

    trade_view->edit_callback = cb;
    trade_view->edit_context = context;
}

void gen3_trade_view_update(void* view, const Gen3TradeViewState* state) {
    furi_assert(view);
    furi_assert(state);
    Gen3TradeView* trade_view = view;
    bool changed = false;

    /* The tick runs every 100 ms: redraw only when something shown changed */
    with_view_model(
        trade_view->view,
        Gen3TradeViewState * model,
        {
            changed = memcmp(model, state, sizeof(*model)) != 0;
            if(changed) memcpy(model, state, sizeof(*model));
        },
        changed);
}
