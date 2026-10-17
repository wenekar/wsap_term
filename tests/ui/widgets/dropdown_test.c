#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "ui/debug.h"
#include "ui/ui.h"
#include "ui/widget_impl.h"

static const struct ui_widget_ops ROOT_OPS = { .name = "test-overlay" };

struct changes { int count, index; };

static bool changed(struct ui_widget *source, const struct ui_action *action, void *ud)
{
    (void)source;
    struct changes *changes = ud;
    if (action->kind != UI_ACTION_CHANGE) return false;
    changes->count++;
    changes->index = action->index;
    return true;
}

static void key(struct ui_surface *surface, enum ui_key key)
{
    struct ui_event event = ui_event_key((struct ui_key_event){ .key = key }, 0);
    assert(ui_surface_dispatch(surface, &event));
}

static void lint(struct ui_surface *surface, struct ui_widget *root)
{
    struct ui_scene scene = {0};
    assert(ui_surface_build_scene(surface, &scene));
    struct ui_debug_view view = { .root = root, .surface = surface, .scene = &scene };
    assert(ui_debug_lint_scene(&view, NULL, 0) == 0);
    ui_scene_destroy(&scene);
}

static struct ui_dropdown_item many_items[40];
static char many_labels[40][12];

static void long_list_bar_test(void)
{
    struct ui_widget root;
    ui_widget_init(&root, &ROOT_OPS, NULL);
    struct ui_dropdown dropdown = {0};
    struct ui_surface surface = {0};
    for (int i = 0; i < 40; i++) {
        snprintf(many_labels[i], sizeof(many_labels[i]), "item %02d", i);
        many_items[i] = (struct ui_dropdown_item){ many_labels[i], many_labels[i] };
    }
    assert(ui_dropdown_set_items(&dropdown, many_items, 40));
    ui_widget_set_fixed_size(ui_dropdown_widget(&dropdown), (struct vector2i){ .x = 200, .y = 32 });
    ui_widget_set_placement(&dropdown.widget, (struct ui_placement){ .kind = UI_PLACE_AT });
    assert(ui_widget_add_child(&root, ui_dropdown_widget(&dropdown)));
    ui_dropdown_set_overlay(&dropdown, &root);
    ui_surface_set_root(&surface, &root);
    ui_surface_resize(&surface, (struct vector2i){ .x = 480, .y = 360 });
    ui_surface_prepare(&surface);
    ui_surface_focus(&surface, ui_button_widget(&dropdown.button));
    key(&surface, UI_KEY_ENTER);
    assert(ui_dropdown_is_open(&dropdown));
    ui_surface_prepare(&surface);

    assert(ui_scrollview_content_height(&dropdown.scroll) >
           ui_scrollview_viewport_height(&dropdown.scroll));
    assert(!ui_scrollview_is_shown(&dropdown.scroll));

    struct rect2i view = dropdown.scroll.widget.bounds;
    struct vector2i on_track = { .x = rect2i_end(view).x - 2, .y = view.y + 10 };
    struct ui_event motion = ui_event_pointer((struct pointer_event){
        .kind = PTR_MOTION, .pos = on_track,
    }, 0);
    ui_surface_dispatch(&surface, &motion);
    ui_surface_prepare(&surface);
    assert(ui_scrollview_is_shown(&dropdown.scroll));

    struct vector2i off_track = { .x = view.x + 10, .y = view.y + 10 };
    motion.data.pointer.pos = off_track;
    ui_surface_dispatch(&surface, &motion);
    ui_surface_tick(&surface, 1000);
    assert(ui_scrollview_is_shown(&dropdown.scroll));
    ui_surface_tick(&surface, 5000);
    assert(!ui_scrollview_is_shown(&dropdown.scroll));

    ui_surface_destroy(&surface);
    ui_widget_destroy(&root);
}

int main(void)
{
    struct ui_widget root;
    ui_widget_init(&root, &ROOT_OPS, NULL);
    struct ui_dropdown dropdown = {0};
    struct changes changes = {0};
    struct ui_surface surface = {0};
    struct ui_dropdown_item items[] = {
        { "Default", "" }, { "Block", "block" }, { "Line", "line" },
    };
    assert(ui_dropdown_set_items(&dropdown, items, 3));
    ui_widget_set_fixed_size(ui_dropdown_widget(&dropdown), (struct vector2i){ .x = 200, .y = 32 });
    ui_widget_set_placement(&dropdown.widget, (struct ui_placement){ .kind = UI_PLACE_AT });
    ui_widget_set_action_handler(&root, changed, &changes);
    assert(ui_widget_add_child(&root, ui_dropdown_widget(&dropdown)));
    ui_dropdown_set_overlay(&dropdown, &root);
    ui_surface_set_root(&surface, &root);
    ui_surface_resize(&surface, (struct vector2i){ .x = 480, .y = 360 });
    ui_surface_prepare(&surface);
    ui_surface_focus(&surface, ui_button_widget(&dropdown.button));
    key(&surface, UI_KEY_ENTER);
    assert(ui_dropdown_is_open(&dropdown));
    assert(ui_surface_has_modal(&surface));
    lint(&surface, &root);

    struct ui_event text = ui_event_text("line", 4, 0);
    assert(ui_surface_dispatch(&surface, &text));
    key(&surface, UI_KEY_ENTER);
    assert(!strcmp(ui_dropdown_value(&dropdown), "line"));
    assert(changes.count == 1 && changes.index == 2);
    assert(!ui_surface_has_modal(&surface));
    assert(ui_surface_focused(&surface) == ui_button_widget(&dropdown.button));

    assert(ui_dropdown_open(&dropdown));
    text = ui_event_text("no match", 8, 0);
    assert(ui_surface_dispatch(&surface, &text));
    key(&surface, UI_KEY_ENTER);
    assert(ui_dropdown_is_open(&dropdown));
    key(&surface, UI_KEY_ESCAPE);
    assert(!strcmp(ui_dropdown_value(&dropdown), "line"));
    assert(changes.count == 1);

    assert(ui_dropdown_open(&dropdown));
    struct ui_event wheel = ui_event_pointer((struct pointer_event){
        .kind = PTR_PRESS, .pos = { .x = 470, .y = 350 }, .button = POINTER_WHEEL_LEFT,
    }, 0);
    ui_surface_dispatch(&surface, &wheel);
    assert(ui_dropdown_is_open(&dropdown));
    struct ui_event outside = ui_event_pointer((struct pointer_event){
        .kind = PTR_PRESS, .pos = { .x = 470, .y = 350 },
    }, 0);
    assert(ui_surface_dispatch(&surface, &outside));
    assert(!ui_dropdown_is_open(&dropdown));
    lint(&surface, &root);
    assert(ui_dropdown_open(&dropdown));
    ui_surface_destroy(&surface);
    ui_widget_destroy(&root);
    long_list_bar_test();
    puts("PASS: dropdown search, selection, cancel, wheel steps, modal focus, and lint");
    return 0;
}
