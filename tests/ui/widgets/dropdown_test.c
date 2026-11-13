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

static void long_choice_label_test(void)
{
    char label[300];
    memset(label, 'x', sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    struct ui_dropdown dropdown = {0};
    struct ui_dropdown_item items[] = {
        { "Short", "short" }, { label, "long" },
    };
    assert(ui_dropdown_set_items(&dropdown, items, 2));
    assert(ui_dropdown_set_value(&dropdown, "long"));
    assert(ui_dropdown_set_value(&dropdown, "missing"));
    assert(!strcmp(ui_button_text(&dropdown.button), "Choose  v"));
    assert(ui_dropdown_set_value(&dropdown, "long"));
    const char *display = ui_button_text(&dropdown.button);
    assert(!strncmp(display, label, strlen(label)));
    assert(!strcmp(display + strlen(label), "  v"));
    ui_widget_destroy(ui_dropdown_widget(&dropdown));
}

static void view_dropdown_outside_scroll_test(enum ui_node_kind root_kind)
{
    const struct ui_dropdown_item items[] = {
        { "First", "first" }, { "Second", "second" },
    };
    struct ui_dropdown *dropdown = NULL;
    struct ui_scrollview *scroll = NULL;
    struct changes changes = {0};
    struct ui_node_desc dropdown_node = {
        .kind = UI_NODE_DROPDOWN, .key = "choice",
        .bind.dropdown = &dropdown,
        .props.dropdown = { .items = items, .count = 2,
                            .accessible_name = "Choice" },
    };
    struct ui_node_desc scroll_node = {
        .kind = UI_NODE_SCROLLVIEW, .key = "scroll",
        .bind.scrollview = &scroll,
        .children = &dropdown_node, .child_count = 1,
    };
    struct ui_node_desc children[] = {
        { .kind = UI_NODE_LABEL, .key = "heading",
          .props.label.text = "Heading" },
        scroll_node,
    };
    struct ui_node_desc root = {
        .kind = root_kind, .key = "form",
        .class_name = "test-form",
        .children = children, .child_count = 2,
        .on_action = changed, .action_userdata = &changes,
    };
    struct ui_view_diagnostic diagnostic;
    struct ui_view *view = ui_view_mount_ex(&root, &diagnostic);
    assert(view && dropdown && scroll);
    struct ui_widget *view_root = ui_view_root(view);
    assert(ui_debug_find(view_root, "@choice") == ui_dropdown_widget(dropdown));
    assert(ui_debug_find(view_root, "@form") == ui_box_widget(ui_view_box(view, "form")));
    ui_widget_set_fixed_size(ui_scrollview_widget(scroll),
                             (struct vector2i){ .x = 220, .y = 42 });
    ui_widget_set_fixed_size(ui_dropdown_widget(dropdown),
                             (struct vector2i){ .x = 200, .y = 32 });
    struct ui_label *heading = ui_view_label(view, "heading");
    assert(heading);
    ui_widget_set_fixed_size(ui_label_widget(heading),
                             (struct vector2i){ .x = 200, .y = 20 });
    struct ui_surface surface = {0};
    struct css_sheet sheet = {0};
    css_init_default(&sheet);
    assert(css_parse(&sheet,
        ".test-form { padding: 12px; background: #123456; }"));
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_set_root(&surface, view_root);
    ui_surface_resize(&surface, (struct vector2i){ .x = 480, .y = 360 });
    struct ui_widget *form = ui_view_widget(view, "form");
    assert(form && form->style.has_bg_color && form->style.bg_color == 0xff123456);
    assert(form->bounds.width == 480 && form->bounds.height == 360);
    struct ui_debug_parts parts = {0};
    ui_debug_collect_parts(form, &parts);
    assert(parts.n > 0 && !strcmp(parts.items[0].name, "content"));
    assert(parts.items[0].rect.x == form->bounds.x + 12);
    assert(parts.items[0].rect.y == form->bounds.y + 12);
    ui_debug_parts_free(&parts);
    ui_surface_focus(&surface, ui_button_widget(&dropdown->button));
    key(&surface, UI_KEY_ENTER);
    assert(ui_dropdown_is_open(dropdown));
    assert(ui_surface_has_modal(&surface));
    ui_surface_prepare(&surface);
    if (root_kind == UI_NODE_COLUMN) {
        int before = dropdown->panel.widget.bounds.y;
        ui_widget_set_hidden(ui_label_widget(heading), true);
        ui_surface_prepare(&surface);
        assert(dropdown->panel.widget.bounds.y < before);
    }
    struct rect2i item = ui_list_item_rect(&dropdown->list, 1);
    struct rect2i clip = ui_scrollview_widget(scroll)->bounds;
    assert(item.width > 0 && item.height > 0);
    assert(item.y >= clip.y + clip.height);
    lint(&surface, view_root);
    struct vector2i pos = { .x = item.x + item.width / 2,
                            .y = item.y + item.height / 2 };
    struct ui_event pointer = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = pos,
    }, 0);
    assert(ui_surface_dispatch(&surface, &pointer));
    pointer.data.pointer.kind = UI_POINTER_RELEASE;
    ui_surface_dispatch(&surface, &pointer);
    assert(!strcmp(ui_dropdown_value(dropdown), "second"));
    assert(changes.count == 1 && changes.index == 1);
    assert(!ui_surface_has_modal(&surface));
    assert(ui_surface_focused(&surface) == ui_button_widget(&dropdown->button));

    assert(ui_dropdown_open(dropdown));
    ui_widget_set_hidden(ui_scrollview_widget(scroll), true);
    ui_surface_prepare(&surface);
    assert(!ui_dropdown_is_open(dropdown));
    assert(!ui_surface_has_modal(&surface));
    ui_widget_set_hidden(ui_scrollview_widget(scroll), false);
    ui_surface_prepare(&surface);
    assert(ui_dropdown_open(dropdown));
    assert(ui_widget_remove_child(ui_scrollview_widget(scroll),
                                  ui_dropdown_widget(dropdown)));
    ui_surface_prepare(&surface);
    assert(!ui_dropdown_is_open(dropdown));
    assert(!ui_surface_has_modal(&surface));
    assert(ui_widget_add_child(ui_scrollview_widget(scroll),
                               ui_dropdown_widget(dropdown)));
    ui_surface_prepare(&surface);
    assert(ui_dropdown_open(dropdown));
    ui_view_destroy(view);
    assert(!ui_surface_root(&surface));
    assert(!ui_surface_has_modal(&surface));
    ui_surface_destroy(&surface);
    css_sheet_clear(&sheet);
}

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
    struct ui_event motion = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_MOTION, .pos = on_track,
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

static void popup_closes_when_scrolled_out_test(void)
{
    const struct ui_dropdown_item items[] = {
        { "First", "first" }, { "Second", "second" },
    };
    struct ui_dropdown *dropdown = NULL;
    struct ui_scrollview *scroll = NULL;
    struct ui_node_desc dropdown_node = {
        .kind = UI_NODE_DROPDOWN, .key = "choice",
        .bind.dropdown = &dropdown,
        .props.dropdown = { .items = items, .count = 2 },
    };
    struct ui_node_desc scroll_node = {
        .kind = UI_NODE_SCROLLVIEW, .key = "scroll",
        .bind.scrollview = &scroll,
        .children = &dropdown_node, .child_count = 1,
    };
    struct ui_node_desc root = {
        .kind = UI_NODE_COLUMN, .key = "form",
        .children = &scroll_node, .child_count = 1,
    };
    struct ui_view *view = ui_view_mount(&root, NULL);
    assert(view && dropdown && scroll);
    ui_widget_set_fixed_size(ui_scrollview_widget(scroll),
                             (struct vector2i){ .x = 220, .y = 40 });
    ui_widget_set_fixed_size(ui_dropdown_widget(dropdown),
                             (struct vector2i){ .x = 200, .y = 90 });
    struct ui_surface surface = {0};
    struct css_sheet sheet = {0};
    css_init_default(&sheet);
    ui_surface_set_sheet(&surface, &sheet);
    ui_surface_set_root(&surface, ui_view_root(view));
    ui_surface_resize(&surface, (struct vector2i){ .x = 320, .y = 300 });
    assert(ui_dropdown_open(dropdown));

    /* Scroll the button out of the viewport. The popup follows it out. */
    ui_scrollview_scroll_to(scroll, 90);
    ui_surface_prepare(&surface);
    assert(!ui_dropdown_is_open(dropdown));
    assert(!ui_surface_has_modal(&surface));
    ui_scrollview_scroll_to(scroll, 0);
    ui_surface_prepare(&surface);
    assert(ui_dropdown_open(dropdown));
    lint(&surface, ui_view_root(view));
    ui_view_destroy(view);
    ui_surface_destroy(&surface);
    css_sheet_clear(&sheet);
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
    struct ui_event wheel = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = 470, .y = 350 }, .button = UI_POINTER_WHEEL_BUTTON + 2,
    }, 0);
    ui_surface_dispatch(&surface, &wheel);
    assert(ui_dropdown_is_open(&dropdown));
    struct ui_event outside = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_PRESS, .pos = { .x = 470, .y = 350 },
    }, 0);
    assert(ui_surface_dispatch(&surface, &outside));
    assert(!ui_dropdown_is_open(&dropdown));
    lint(&surface, &root);
    assert(ui_dropdown_open(&dropdown));
    ui_surface_destroy(&surface);
    ui_widget_destroy(&root);
    long_list_bar_test();
    long_choice_label_test();
    view_dropdown_outside_scroll_test(UI_NODE_COLUMN);
    view_dropdown_outside_scroll_test(UI_NODE_STACK);
    popup_closes_when_scrolled_out_test();
    puts("PASS: dropdown search, selection, cancel, wheel steps, modal focus, and lint");
    return 0;
}
