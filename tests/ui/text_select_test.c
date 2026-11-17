#include <assert.h>
#include <string.h>

#include "ui/ui.h"

/* The gestures of label and input come from one controller. These tests run
 * the same pointer sequence on both widgets and compare the selection. */

struct fixture {
    struct ui_surface surface;
    struct ui_label label;
    struct ui_input input;
};

struct selected {
    char text[64];
    bool empty;
};

/* The selection after one, two, and three clicks. */
struct gestures {
    struct selected one, two, three;
};

static void init(struct fixture *f, struct ui_widget *root)
{
    ui_surface_init(&f->surface, NULL, NULL);
    ui_surface_set_scale(&f->surface, (struct css_scale_context){
        .dpi_scale = 1, .cell_w = 8, .cell_h = 16,
    });
    ui_surface_set_root(&f->surface, root);
    ui_surface_resize(&f->surface, (struct vector2i){ .x = 240, .y = 48 });
}

static void destroy(struct fixture *f)
{
    ui_surface_set_root(&f->surface, NULL);
    ui_surface_destroy(&f->surface);
    if (f->label.widget.ops)
        ui_widget_destroy(ui_label_widget(&f->label));
    if (f->input.widget.ops)
        ui_widget_destroy(ui_input_widget(&f->input));
}

static struct selected label_selected(const struct ui_label *label)
{
    struct selected out = { .empty = true };
    size_t lo = 0, hi = 0;
    if (!ui_label_selection(label, &lo, &hi) || hi - lo >= sizeof(out.text))
        return out;
    memcpy(out.text, ui_label_text(label) + lo, hi - lo);
    out.empty = false;
    return out;
}

static struct selected input_selected(const struct ui_input *in)
{
    struct selected out = { .empty = true };
    size_t lo = 0, hi = 0;
    if (!ui_input_selection(in, &lo, &hi) || hi - lo >= sizeof(out.text))
        return out;
    memcpy(out.text, ui_input_text(in) + lo, hi - lo);
    out.empty = false;
    return out;
}

static bool pointer(struct fixture *f, enum ui_pointer_kind kind,
                    struct vector2i pos, uint64_t time)
{
    struct ui_event event = ui_event_pointer((struct ui_pointer_event){
        .kind = kind, .pos = pos,
    }, time);
    return ui_surface_dispatch(&f->surface, &event);
}

static void click(struct fixture *f, struct vector2i pos, uint64_t time)
{
    assert(pointer(f, UI_POINTER_PRESS, pos, time));
    assert(pointer(f, UI_POINTER_RELEASE, pos, time + 1));
}

static struct gestures run_clicks(struct fixture *f,
                                  struct selected (*selected)(struct fixture *))
{
    struct vector2i word = { .x = 60, .y = 8 };
    struct gestures g;
    click(f, word, 100);
    g.one = selected(f);
    click(f, word, 200);
    g.two = selected(f);
    click(f, word, 300);
    g.three = selected(f);
    return g;
}

static struct selected label_of(struct fixture *f)
{
    return label_selected(&f->label);
}

static struct selected input_of(struct fixture *f)
{
    return input_selected(&f->input);
}

/* One click drops the selection. Two clicks select the word. Three clicks
 * select the line, which is the whole value in an input. */
static void click_gestures_match_test(void)
{
    struct fixture f = {0};
    struct gestures label, input;
    ui_label_init(&f.label);
    assert(ui_label_set_text(&f.label, "Hello caf\xC3\xA9 world"));
    ui_label_set_selectable(&f.label, true);
    init(&f, ui_label_widget(&f.label));
    label = run_clicks(&f, label_of);
    destroy(&f);

    ui_input_init(&f.input);
    assert(ui_input_set_text(&f.input, "Hello caf\xC3\xA9 world"));
    init(&f, ui_input_widget(&f.input));
    input = run_clicks(&f, input_of);
    destroy(&f);

    assert(label.one.empty && input.one.empty);
    assert(!label.two.empty && !strcmp(label.two.text, "caf\xC3\xA9"));
    assert(!strcmp(label.two.text, input.two.text));
    assert(!label.three.empty && !strcmp(label.three.text, "Hello caf\xC3\xA9 world"));
    assert(!strcmp(label.three.text, input.three.text));
}

/* A drag selects from the press offset to the pointer in both widgets. */
static void drag_selects_a_range_test(void)
{
    struct fixture f = {0};
    struct vector2i start = { .x = 0, .y = 8 };
    struct vector2i end = { .x = 400, .y = 8 };
    ui_label_init(&f.label);
    assert(ui_label_set_text(&f.label, "Hello caf\xC3\xA9 world"));
    ui_label_set_selectable(&f.label, true);
    init(&f, ui_label_widget(&f.label));
    assert(pointer(&f, UI_POINTER_PRESS, start, 0));
    assert(pointer(&f, UI_POINTER_MOTION, end, 1));
    assert(pointer(&f, UI_POINTER_RELEASE, end, 2));
    struct selected label_sel = label_selected(&f.label);
    assert(!label_sel.empty && !strcmp(label_sel.text, "Hello caf\xC3\xA9 world"));
    destroy(&f);

    ui_input_init(&f.input);
    assert(ui_input_set_text(&f.input, "Hello caf\xC3\xA9 world"));
    init(&f, ui_input_widget(&f.input));
    assert(pointer(&f, UI_POINTER_PRESS, start, 0));
    assert(pointer(&f, UI_POINTER_MOTION, end, 1));
    assert(pointer(&f, UI_POINTER_RELEASE, end, 2));
    struct selected input_sel = input_selected(&f.input);
    assert(!input_sel.empty && !strcmp(input_sel.text, "Hello caf\xC3\xA9 world"));
    destroy(&f);
}

/* A press that repeats after the interval of the surface is a single click. */
static void click_interval_comes_from_the_surface_test(void)
{
    struct fixture f = {0};
    struct vector2i word = { .x = 60, .y = 8 };
    ui_label_init(&f.label);
    assert(ui_label_set_text(&f.label, "Hello caf\xC3\xA9 world"));
    ui_label_set_selectable(&f.label, true);
    init(&f, ui_label_widget(&f.label));
    click(&f, word, 100);
    click(&f, word, 800);
    assert(label_selected(&f.label).empty);
    destroy(&f);
}

int main(void)
{
    click_gestures_match_test();
    drag_selects_a_range_test();
    click_interval_comes_from_the_surface_test();
    return 0;
}
