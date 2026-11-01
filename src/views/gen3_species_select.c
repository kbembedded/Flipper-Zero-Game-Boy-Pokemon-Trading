#include <gui/elements.h>
#include <gui/view.h>
#include <storage/storage.h>

#include <src/views/gen3_species_select.h>
#include <src/gen3/gen3_data.h>

/* One record in all_sprites.fxbm: a u32 record size, then a u32 width, a u32 height and 392
 * bytes of 56x56 XBM data. */
#define SPRITE_RECORD_SIZE 404
#define SPRITE_SIZE_FIELD  4
#define SPRITE_DATA_SIZE   (SPRITE_RECORD_SIZE - SPRITE_SIZE_FIELD)
#define SPRITE_HEADER_SIZE 8
#define SPRITE_DIM         56

typedef struct {
    uint16_t national;
    char name[GEN3_SPECIES_NAME_LEN + 1]; /* read from the SD card with the sprite */
    bool has_sprite;
    uint8_t sprite[SPRITE_DATA_SIZE]; /* width, height, then the pixels */
} Gen3SpeciesSelectModel;

typedef struct {
    View* view;
    Gen3SpeciesSelectCallback callback;
    void* callback_context;
} Gen3SpeciesSelect;

static void gen3_species_select_draw(Canvas* canvas, void* model_ptr) {
    const Gen3SpeciesSelectModel* model = model_ptr;
    const Gen3SpeciesInfo* info = gen3_species(model->national);
    char buf[24];

    canvas_clear(canvas);

    if(model->has_sprite) {
        canvas_draw_xbm(canvas, 0, 0, SPRITE_DIM, SPRITE_DIM, model->sprite + SPRITE_HEADER_SIZE);
    } else {
        canvas_draw_rframe(canvas, 4, 4, 48, 48, 4);
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 28, 28, AlignCenter, AlignCenter, "?");
    }

    elements_frame(canvas, 55, 0, 71, 18);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 90, 5, AlignCenter, AlignTop, "Select Pokemon");

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 58, 21, AlignLeft, AlignTop, model->name);

    canvas_set_font(canvas, FontSecondary);
    snprintf(buf, sizeof(buf), "#%03u", (unsigned)model->national);
    canvas_draw_str_aligned(canvas, 58, 33, AlignLeft, AlignTop, buf);

    if(info) {
        if(info->type[0] == info->type[1]) {
            snprintf(buf, sizeof(buf), "%s", gen3_type_name(info->type[0]));
        } else {
            snprintf(
                buf,
                sizeof(buf),
                "%s/%s",
                gen3_type_name(info->type[0]),
                gen3_type_name(info->type[1]));
        }
        canvas_draw_str_aligned(canvas, 58, 43, AlignLeft, AlignTop, buf);
    }

    canvas_set_font(canvas, FontPrimary);
    elements_button_center(canvas, "OK");
}

/* Reads the name and sprite for the current species into the model. Both come from the SD card,
 * so never from the draw callback, which runs in the GUI thread. */
static void gen3_species_select_load(View* view) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool is_open = storage_file_open(
        file, APP_ASSETS_PATH("all_sprites.fxbm"), FSAM_READ, FSOM_OPEN_EXISTING);

    with_view_model(
        view,
        Gen3SpeciesSelectModel * model,
        {
            gen3_species_name_get(model->national, model->name);
            model->has_sprite = false;
            if(is_open && model->national >= 1 && model->national <= GEN3_SPRITE_MAX_NATIONAL) {
                uint32_t offset =
                    (uint32_t)(model->national - 1) * SPRITE_RECORD_SIZE + SPRITE_SIZE_FIELD;
                if(storage_file_seek(file, offset, true) &&
                   storage_file_read(file, model->sprite, SPRITE_DATA_SIZE) ==
                       SPRITE_DATA_SIZE) {
                    model->has_sprite = true;
                }
            }
        },
        true);

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static bool gen3_species_select_input(InputEvent* event, void* context) {
    Gen3SpeciesSelect* select = context;
    uint16_t national = 0;
    bool consumed = false;

    if(event->type != InputTypePress && event->type != InputTypeRepeat) return false;

    with_view_model(
        select->view, Gen3SpeciesSelectModel * model, { national = model->national; }, false);

    switch(event->key) {
    case InputKeyOk:
        /* Only a press, so a held OK doesn't fire twice */
        if(event->type == InputTypePress && select->callback) {
            select->callback(select->callback_context, national);
        }
        return true;
    case InputKeyLeft:
        national = (national <= 1) ? GEN3_NATIONAL_COUNT : national - 1;
        consumed = true;
        break;
    case InputKeyRight:
        national = (national >= GEN3_NATIONAL_COUNT) ? 1 : national + 1;
        consumed = true;
        break;
    case InputKeyDown:
        national = (national > 10) ? national - 10 : GEN3_NATIONAL_COUNT;
        consumed = true;
        break;
    case InputKeyUp:
        national = (national + 10 <= GEN3_NATIONAL_COUNT) ? national + 10 : 1;
        consumed = true;
        break;
    default:
        break;
    }

    if(consumed) {
        with_view_model(
            select->view, Gen3SpeciesSelectModel * model, { model->national = national; }, true);
        gen3_species_select_load(select->view);
    }

    return consumed;
}

static void gen3_species_select_enter(void* context) {
    Gen3SpeciesSelect* select = context;

    gen3_species_select_load(select->view);
}

void* gen3_species_select_alloc(ViewDispatcher* view_dispatcher, uint32_t view_id) {
    furi_assert(view_dispatcher);
    Gen3SpeciesSelect* select = malloc(sizeof(Gen3SpeciesSelect));

    select->callback = NULL;
    select->callback_context = NULL;
    select->view = view_alloc();
    view_set_context(select->view, select);
    view_allocate_model(select->view, ViewModelTypeLocking, sizeof(Gen3SpeciesSelectModel));
    view_set_draw_callback(select->view, gen3_species_select_draw);
    view_set_input_callback(select->view, gen3_species_select_input);
    view_set_enter_callback(select->view, gen3_species_select_enter);

    with_view_model(
        select->view,
        Gen3SpeciesSelectModel * model,
        {
            model->national = 1;
            model->name[0] = '\0';
            model->has_sprite = false;
        },
        false);

    view_dispatcher_add_view(view_dispatcher, view_id, select->view);
    return select;
}

void gen3_species_select_free(ViewDispatcher* view_dispatcher, uint32_t view_id, void* select_ptr) {
    Gen3SpeciesSelect* select = select_ptr;

    view_dispatcher_remove_view(view_dispatcher, view_id);
    view_free(select->view);
    free(select);
}

void gen3_species_select_start(
    void* select_ptr,
    uint16_t national,
    Gen3SpeciesSelectCallback cb,
    void* context) {
    Gen3SpeciesSelect* select = select_ptr;

    select->callback = cb;
    select->callback_context = context;
    with_view_model(
        select->view,
        Gen3SpeciesSelectModel * model,
        { model->national = (national >= 1 && national <= GEN3_NATIONAL_COUNT) ? national : 1; },
        false);
}
