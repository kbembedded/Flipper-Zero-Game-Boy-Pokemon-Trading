#include "../pokemon_app.h"
#include "select_move1.hpp"

static void select_move1_render_callback(Canvas* canvas, void* context) {
    canvas_clear(canvas);

    SelectPokemonModel* model = (SelectPokemonModel*)context;
    const uint8_t current_index = model->current_move;
    char move_num[5];

    snprintf(move_num, sizeof(move_num), "#%03d", current_index + 1);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 55, 54 / 2, AlignLeft, AlignTop, move_table[current_index].name);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 55, 38, AlignLeft, AlignTop, move_num);
    canvas_draw_icon(canvas, 0, 0, pokemon_table[model->current_pokemon].icon);
    canvas_draw_icon(canvas, 128 - 80, 0, &I_Space_80x18);
    canvas_draw_str_aligned(canvas, (128 - 40), 5, AlignCenter, AlignTop, "Select Move #1");

    canvas_set_font(canvas, FontPrimary);
    elements_button_center(canvas, "OK");
}

static bool select_move1_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    SelectMove1* select_move1 = (SelectMove1*)context;
    bool consumed = false;

    if(event->type == InputTypePress && event->key == InputKeyOk) {
        with_view_model_cpp(
            select_move1->view,
            SelectPokemonModel*,
            model,
            {
                select_move1->app->current_move = model->current_move;
                select_move1->app->move1_hex_code = move_table[model->current_move].hex;
            },
            false);
        view_dispatcher_switch_to_view(select_move1->app->view_dispatcher, AppViewSelectMove2);
        consumed = true;
    } else if(event->type == InputTypePress && event->key == InputKeyBack) {
        view_dispatcher_switch_to_view(select_move1->app->view_dispatcher, AppViewSelectLevel);
        consumed = true;
    } else if(event->type == InputTypePress && event->key == InputKeyLeft) {
        with_view_model_cpp(
            select_move1->view,
            SelectPokemonModel*,
            model,
            {
                if(model->current_move == 0) {
                    model->current_move = 165;
                } else {
                    model->current_move--;
                }
            },
            true);
        consumed = true;
    } else if(event->type == InputTypePress && event->key == InputKeyDown) {
        with_view_model_cpp(
            select_move1->view,
            SelectPokemonModel*,
            model,
            {
                if(model->current_move >= 10) {
                    model->current_move -= 10;
                } else {
                    model->current_move = 165;
                }
            },
            true);
        consumed = true;
    } else if(event->type == InputTypePress && event->key == InputKeyRight) {
        with_view_model_cpp(
            select_move1->view,
            SelectPokemonModel*,
            model,
            {
                if(model->current_move == 165) {
                    model->current_move = 0;
                } else {
                    model->current_move++;
                }
            },
            true);
        consumed = true;
    } else if(event->type == InputTypePress && event->key == InputKeyUp) {
        with_view_model_cpp(
            select_move1->view,
            SelectPokemonModel*,
            model,
            {
                if(model->current_move <= 155) {
                    model->current_move += 10;
                } else {
                    model->current_move = 0;
                    ;
                }
            },
            true);
        consumed = true;
    }

    return consumed;
}

void select_move1_enter_callback(void* context) {
    furi_assert(context);
    SelectMove1* select_move1 = (SelectMove1*)context;
    with_view_model_cpp(
        select_move1->view,
        SelectPokemonModel*,
        model,
        {
            model->current_pokemon = select_move1->app->current_pokemon;
            model->pokemon_hex_code = select_move1->app->pokemon_hex_code;
            model->current_level = select_move1->app->current_level;
            model->move1_hex_code = select_move1->app->move1_hex_code;
        },
        true);
}

bool select_move1_custom_callback(uint32_t event, void* context) {
    UNUSED(event);
    furi_assert(context);
    SelectMove1* select_move1 = (SelectMove1*)context;
    view_dispatcher_send_custom_event(select_move1->app->view_dispatcher, 0);
    return true;
}

void select_move1_exit_callback(void* context) {
    furi_assert(context);
    UNUSED(context);
}

SelectMove1* select_move1_alloc(App* app) {
    SelectMove1* select_move1 = (SelectMove1*)malloc(sizeof(SelectMove1));
    select_move1->app = app;
    select_move1->view = view_alloc();
    view_set_context(select_move1->view, select_move1);
    view_allocate_model(select_move1->view, ViewModelTypeLockFree, sizeof(SelectPokemonModel));
    with_view_model_cpp(
        select_move1->view,
        SelectPokemonModel*,
        model,
        {
            model->current_move = app->current_move;
            model->move1_hex_code = app->move1_hex_code;
        },
        true);

    view_set_draw_callback(select_move1->view, select_move1_render_callback);
    view_set_input_callback(select_move1->view, select_move1_input_callback);
    view_set_enter_callback(select_move1->view, select_move1_enter_callback);
    view_set_custom_callback(select_move1->view, select_move1_custom_callback);

    view_set_exit_callback(select_move1->view, select_move1_exit_callback);
    return select_move1;
}

void select_move1_free(App* app) {
    furi_assert(app->select_move1);
    view_free(app->select_move1->view);
    free(app->select_move1);
}

View* select_move1_get_view(App* app) {
    furi_assert(app->select_move1);
    return app->select_move1->view;
}