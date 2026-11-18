#include "platform/platform.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/window.h"
#include "ui/widgets/input.h"
#include "util/app_identity.h"

static struct window *root_window(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct window, ui.root);
}

static void root_prepare(struct ui_widget *widget)
{
    struct window *w = root_window(widget);
    struct ui_widget *content = ui_of(w)->spec.root;
    if (!content || widget->first_child == content)
        return;
    if (content->parent)
        ui_widget_remove_child(content->parent, content);
    ui_widget_insert_child(widget, content, widget->first_child);
}

static bool root_accessibility(const struct ui_widget *widget,
                               struct ui_accessibility_node_spec *out)
{
    const struct window *w = root_window(widget);
    const char *title = window_title(w);
    out->role = UI_ACCESSIBILITY_ROLE_WINDOW;
    const struct app_identity *identity = app_identity_current();
    out->name = title && title[0] ? title : (identity ? identity->id : "");
    return true;
}

static bool text_menu_action(struct ui_widget *source, const struct ui_action *action, void *userdata)
{
    (void)source;
    struct window *w = userdata;
    struct ui_widget *target = ui_surface_context_target(&ui_of(w)->screen);
    if (action->kind == UI_ACTION_ACTIVATE) {
        window_menu_close(w);
        if (target) {
            ui_surface_focus(&ui_of(w)->screen, target);
            ui_input_command(ui_container_of(target, struct ui_input, widget), action->id);
        }
        return true;
    }
    if (action->kind == UI_ACTION_CLOSE)
        ui_surface_set_context_target(&ui_of(w)->screen, NULL);
    return false;
}

static bool root_action(struct ui_widget *source, const struct ui_action *action, void *userdata)
{
    struct ui_input *in = ui_input_of(source);
    if (action->kind != UI_ACTION_MENU || !in)
        return false;
    struct window *w = userdata;
    ui_menu_item items[] = {
        UI_MENU_ITEM("Undo", UI_EDIT_UNDO),
        UI_MENU_ITEM("Redo", UI_EDIT_REDO),
        UI_MENU_SEPARATOR,
        UI_MENU_ITEM("Cut", UI_EDIT_CUT),
        UI_MENU_ITEM("Copy", UI_EDIT_COPY),
        UI_MENU_ITEM("Paste", UI_EDIT_PASTE),
        UI_MENU_SEPARATOR,
        UI_MENU_ITEM("Select All", UI_EDIT_SELECT_ALL),
    };
    for (size_t i = 0; i < sizeof(items) / sizeof(items[0]); i++)
        if (!(items[i].flags & UI_LIST_ITEM_SEPARATOR) && !ui_input_can_command(in, items[i].action))
            items[i].flags |= UI_LIST_ITEM_DISABLED;
    window_menu_close(w);
    ui_surface_set_context_target(&ui_of(w)->screen, source);
    if (!window_menu_open(w, items, sizeof(items) / sizeof(items[0]), action->pos,
            text_menu_action, NULL, w))
        ui_surface_set_context_target(&ui_of(w)->screen, NULL);
    return true;
}

static const struct ui_widget_ops WINDOW_ROOT_OPS = {
    .name = "window-root",
    .prepare = root_prepare,
    .accessibility = root_accessibility,
};

static void window_ui_request_frame(void *userdata)
{
    struct window *w = userdata;
    window_redraw(w);
}

static void window_ui_copy_text(void *userdata, const char *text)
{
    struct window *w = userdata;
    window_copy(w, YASL_CLIPBOARD, text);
}

static void window_ui_paste_text(void *userdata)
{
    struct window *w = userdata;
    window_paste(w, YASL_CLIPBOARD);
}

static void window_ui_copy_primary(void *userdata, const char *text)
{
    struct window *w = userdata;
    window_copy(w, YASL_PRIMARY, text);
}

static void window_ui_paste_primary(void *userdata)
{
    struct window *w = userdata;
    window_paste(w, YASL_PRIMARY);
}

static void window_ui_set_text_input(void *userdata,
                                     const struct ui_text_input *input)
{
    struct window *w = userdata;
    struct yasl_surface *surface = window_surface(w);
    if (!surface)
        return;
    if (!input) {
        yasl_surface_set_text_input(surface, NULL);
        return;
    }
    struct yasl_text_input native = {
        .purpose = (enum yasl_text_purpose)input->purpose,
        .text = input->text,
        .length = input->length,
        .selection_begin = input->selection_begin,
        .selection_end = input->selection_end,
        .revision = input->revision,
        .caret_x = input->caret_x,
        .caret_y = input->caret_y,
        .caret_width = input->caret_width,
        .caret_height = input->caret_height,
    };
    yasl_surface_set_text_input(surface, &native);
}

static void window_ui_open_url(void *userdata, const char *target)
{
    (void)userdata;
    platform_open_url(target);
}

static uint32_t window_ui_double_click_interval_ms(void *userdata)
{
    return window_double_click_ms(userdata);
}

const struct ui_surface_host window_ui_surface_host = {
    .request_frame = window_ui_request_frame,
    .copy_text = window_ui_copy_text,
    .paste_text = window_ui_paste_text,
    .copy_primary = window_ui_copy_primary,
    .paste_primary = window_ui_paste_primary,
    .set_text_input = window_ui_set_text_input,
    .open_url = window_ui_open_url,
    .double_click_interval_ms = window_ui_double_click_interval_ms,
};

void window_ui_arm(struct window *w, uint64_t now_ms)
{
    uint64_t next = ui_surface_next_deadline(&ui_of(w)->screen);
    for (int i = 0; i < ui_of(w)->menu_popups; i++) {
        uint64_t d = ui_surface_next_deadline(&ui_of(w)->menu_popup[i].ui);
        if (d && (!next || d < next))
            next = d;
    }
    window_wake_at(w, next > now_ms ? next : 0);
}

void window_ui_root_init(struct window *w)
{
    ui_widget_init(&ui_of(w)->root, &WINDOW_ROOT_OPS, NULL);
    ui_widget_set_action_handler(&ui_of(w)->root, root_action, w);
    ui_menu_init(&ui_of(w)->menu);
    ui_menu_layer_init(&ui_of(w)->menu_layer, &ui_of(w)->menu);
    ui_widget_add_child(&ui_of(w)->root, ui_menu_layer_widget(&ui_of(w)->menu_layer));
    ui_surface_init(&ui_of(w)->screen, &window_ui_surface_host, w);
    ui_surface_set_root(&ui_of(w)->screen, &ui_of(w)->root);
}

void window_ui_root_destroy(struct window *w)
{
    struct ui_widget *content = ui_of(w)->spec.root;
    if (content && content->parent == &ui_of(w)->root)
        ui_widget_remove_child(&ui_of(w)->root, content);
    ui_surface_destroy(&ui_of(w)->screen);
    ui_widget_destroy(&ui_of(w)->root);
}
