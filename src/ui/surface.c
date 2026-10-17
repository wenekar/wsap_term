#include "ui/surface.h"

#include "ui/box.h"
#include "ui/internal/tree.h"
#include "ui/internal/layout.h"
#include "paint/scene.h"
#include "ui/ui_util.h"
#include "ui/widgets/button.h"
#include "ui/widgets/label.h"

#include <stdlib.h>
#include <string.h>

struct ui_dialog_frame {
    struct ui_box card;
    struct ui_label title;
    struct ui_label msg;
    struct ui_box btn_row;
    struct ui_button btns[UI_DIALOG_MAX_BUTTONS];
    ui_action_fn on_action;
    void *userdata;
    struct ui_surface *surface;
    struct ui_dialog_frame *next_retired;
};

static void release_retired_dialogs(struct ui_surface *surface)
{
    while (surface && surface->impl.retired_dialogs) {
        struct ui_dialog_frame *frame = surface->impl.retired_dialogs;
        surface->impl.retired_dialogs = frame->next_retired;
        free(frame);
    }
}

void ui_surface_init(struct ui_surface *surface,
                     const struct ui_surface_host *host, void *host_userdata)
{
    if (!surface)
        return;
    *surface = (struct ui_surface){0};
    if (host)
        surface->impl.host = *host;
    surface->impl.host_userdata = host_userdata;
    ui_style_context_init(&surface->impl.style);
}

bool ui_surface_copy_text(struct ui_surface *surface, const char *text)
{
    if (!surface || !text || !surface->impl.host.copy_text)
        return false;
    surface->impl.host.copy_text(surface->impl.host_userdata, text);
    return true;
}

bool ui_surface_paste_text(struct ui_surface *surface)
{
    if (!surface || !surface->impl.focused || !surface->impl.host.paste_text)
        return false;
    surface->impl.paste_target = surface->impl.focused;
    surface->impl.host.paste_text(surface->impl.host_userdata);
    return true;
}

bool ui_surface_open_url(struct ui_surface *surface, const char *target)
{
    if (!surface || !target || !*target || !surface->impl.host.open_url)
        return false;
    surface->impl.host.open_url(surface->impl.host_userdata, target);
    return true;
}

uint32_t ui_surface_double_click_interval_ms(const struct ui_surface *surface)
{
    if (!surface || !surface->impl.host.double_click_interval_ms)
        return 400;
    return surface->impl.host.double_click_interval_ms(
        surface->impl.host_userdata);
}

void ui_surface_request_frame(struct ui_surface *surface)
{
    if (!surface || surface->impl.frame_requested)
        return;
    surface->impl.frame_requested = true;
    if (surface->impl.host.request_frame)
        surface->impl.host.request_frame(surface->impl.host_userdata);
}

bool ui_surface_frame_pending(const struct ui_surface *surface)
{
    return surface && surface->impl.frame_requested;
}

bool ui_surface_take_frame_request(struct ui_surface *surface)
{
    if (!surface)
        return false;
    bool pending = surface->impl.frame_requested;
    surface->impl.frame_requested = false;
    return pending;
}

static struct ui_constraints viewport_constraints(const struct ui_surface *s)
{
    return (struct ui_constraints){ .max = s->impl.viewport };
}

static void measure_and_layout(struct ui_surface *surface)
{
    struct ui_widget *root = surface->impl.root;
    if (!root)
        return;
    if (root->impl.invalidation & UI_WIDGET_INVALIDATE_MEASURE)
        ui_widget_measure(root, viewport_constraints(surface));
    if (root->impl.invalidation & UI_WIDGET_INVALIDATE_MEASURE) {
        ui_surface_request_frame(surface);
        return;
    }
    struct vector2i size = surface->impl.viewport;
    if ((root->impl.invalidation & UI_WIDGET_INVALIDATE_LAYOUT) &&
        size.x > 0 && size.y > 0)
        ui_widget_layout(root, (struct rect2i){ .size = size });
}

static void prepare(struct ui_surface *surface)
{
    struct ui_widget *root = surface->impl.root;
    if (!root)
        return;
    ui_widget_prepare(root);
    root = surface->impl.root;
    if (!root)
        return;
    ui_style_context_apply(&surface->impl.style, root);
    measure_and_layout(surface);
    ui_surface_sync_modal(surface);
}

void ui_surface_prepare(struct ui_surface *surface)
{
    if (!surface || surface->impl.busy)
        return;
    surface->impl.busy = true;
    prepare(surface);
    surface->impl.busy = false;
    release_retired_dialogs(surface);
}

bool ui_surface_set_root(struct ui_surface *surface, struct ui_widget *root)
{
    if (!surface || (root && (root->impl.destroyed || root->parent)))
        return false;
    if (surface->impl.root != root) {
        ui_dialog_close(surface);
        while (surface->impl.root)
            ui_widget_detach(surface->impl.root);
        surface->impl.focused = NULL;
        surface->impl.captured = NULL;
        surface->impl.hovered = NULL;
        surface->impl.modal = NULL;
        surface->impl.restore_focus = NULL;
    }
    surface->impl.root = root;
    if (root) {
        ui_widget_attach(root, surface);
        ui_widget_invalidate(root, UI_WIDGET_INVALIDATE_MEASURE);
        ui_surface_prepare(surface);
    }
    ui_surface_request_frame(surface);
    return true;
}

struct ui_widget *ui_surface_root(const struct ui_surface *surface)
{
    return surface ? surface->impl.root : NULL;
}

void ui_surface_set_sheet(struct ui_surface *surface,
                          const struct css_sheet *sheet)
{
    if (!surface)
        return;
    ui_style_context_set_sheet(&surface->impl.style, sheet);
    if (surface->impl.root)
        ui_widget_invalidate(surface->impl.root, UI_WIDGET_INVALIDATE_MEASURE);
}

void ui_surface_configure(struct ui_surface *surface, struct vector2i viewport,
                            struct css_scale_context scale)
{
    if (!surface)
        return;
    uint64_t before = ui_style_context_generation(&surface->impl.style);
    ui_style_context_set_scale(&surface->impl.style, scale);
    bool changed = before != ui_style_context_generation(&surface->impl.style) ||
                   !vector2i_eq(surface->impl.viewport, viewport);
    surface->impl.viewport = viewport;
    if (changed && surface->impl.root)
        ui_widget_invalidate(surface->impl.root, UI_WIDGET_INVALIDATE_MEASURE);
    ui_surface_prepare(surface);
}

void ui_surface_set_scale(struct ui_surface *surface,
                          struct css_scale_context scale)
{
    ui_surface_configure(surface, ui_surface_viewport(surface), scale);
}

struct css_scale_context ui_surface_scale(const struct ui_surface *surface)
{
    if (!surface)
        return (struct css_scale_context){0};
    struct css_scale_context defaults = ui_scale_ctx_default();
    struct css_scale_context scale = surface->impl.style.scale;
    return (struct css_scale_context){
        .dpi_scale = ui_positive_or(scale.dpi_scale, defaults.dpi_scale),
        .cell_w = ui_positive_or(scale.cell_w, defaults.cell_w),
        .cell_h = ui_positive_or(scale.cell_h, defaults.cell_h),
    };
}

void ui_surface_resize(struct ui_surface *surface, struct vector2i viewport)
{
    ui_surface_configure(surface, viewport, ui_surface_scale(surface));
}

struct vector2i ui_surface_viewport(const struct ui_surface *surface)
{
    return surface ? surface->impl.viewport : (struct vector2i){0};
}

static bool retired(const struct ui_surface *surface,
                    const struct ui_widget *widget)
{
    return widget->impl.surface != surface || !ui_widget_visible(widget);
}

static void send(struct ui_widget *widget, struct ui_event event)
{
    if (widget && !widget->impl.destroyed && widget->ops &&
        widget->ops->event)
        widget->ops->event(widget, &event);
}

static void leave(struct ui_widget *widget)
{
    send(widget, ui_event_pointer(
        (struct pointer_event){ .kind = PTR_LEAVE }, 0));
}

void ui_surface_cancel_capture(struct ui_surface *surface)
{
    if (!surface)
        return;
    struct ui_widget *captured = surface->impl.captured;
    surface->impl.captured = NULL;
    if (captured && captured->impl.surface == surface)
        send(captured, (struct ui_event){ .kind = UI_EVENT_CAPTURE_LOST });
}

void ui_surface_forget(struct ui_surface *surface, struct ui_widget *subtree)
{
    struct ui_widget *captured = surface->impl.captured;
    if (captured && ui_widget_is_ancestor(subtree, captured))
        ui_surface_cancel_capture(surface);
    struct ui_widget *hovered = surface->impl.hovered;
    if (hovered && ui_widget_is_ancestor(subtree, hovered)) {
        surface->impl.hovered = NULL;
        leave(hovered);
    }
    for (int pass = 0; pass < 2; pass++) {
        struct ui_widget *focused = surface->impl.focused;
        if (!focused || !ui_widget_is_ancestor(subtree, focused))
            break;
        surface->impl.focused = NULL;
        send(focused, ui_event_focus(false, 0));
    }
    struct ui_widget **refs[] = {
        &surface->impl.focused, &surface->impl.captured,
        &surface->impl.hovered, &surface->impl.modal,
        &surface->impl.restore_focus, &surface->impl.default_focus,
        &surface->impl.paste_target, &surface->impl.context_target,
    };
    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++)
        if (*refs[i] && ui_widget_is_ancestor(subtree, *refs[i]))
            *refs[i] = NULL;
}

static struct ui_widget *first_focusable(struct ui_widget *root)
{
    if (!root || root->impl.destroyed || root->impl.hidden)
        return NULL;
    if (root->impl.focusable)
        return root;
    for (struct ui_widget *c = root->first_child; c; c = c->next_sibling) {
        struct ui_widget *found = first_focusable(c);
        if (found)
            return found;
    }
    return NULL;
}

void ui_surface_sync_modal(struct ui_surface *surface)
{
    struct ui_widget *modal = ui_widget_modal(surface->impl.root);
    if (modal == surface->impl.modal &&
        (modal || !surface->impl.restore_focus)) {
        if (modal && !ui_widget_is_ancestor(modal, surface->impl.focused))
            ui_surface_focus(surface, first_focusable(modal));
        return;
    }
    if (modal) {
        struct ui_widget *focused = surface->impl.focused;
        if (!surface->impl.modal && !surface->impl.restore_focus &&
            focused && !ui_widget_is_ancestor(modal, focused))
            surface->impl.restore_focus = focused;
        surface->impl.modal = modal;
        if (surface->impl.captured &&
            !ui_widget_is_ancestor(modal, surface->impl.captured))
            ui_surface_cancel_capture(surface);
        struct ui_widget *hovered = surface->impl.hovered;
        if (hovered && !ui_widget_is_ancestor(modal, hovered)) {
            surface->impl.hovered = NULL;
            struct ui_event out = ui_event_pointer(
                (struct pointer_event){ .kind = PTR_LEAVE }, 0);
            ui_widget_bubble(hovered, &out);
        }
        if (!ui_widget_is_ancestor(modal, surface->impl.focused))
            ui_surface_focus(surface, first_focusable(modal));
    } else {
        surface->impl.modal = NULL;
        struct ui_widget *restore = surface->impl.restore_focus;
        surface->impl.restore_focus = NULL;
        if (restore && !retired(surface, restore))
            ui_surface_focus(surface, restore);
    }
}

enum ui_cursor ui_surface_cursor(const struct ui_surface *surface)
{
    if (!surface)
        return UI_CURSOR_DEFAULT;
    const struct ui_widget *w = surface->impl.captured ? surface->impl.captured
                                                       : surface->impl.hovered;
    for (; w && ui_widget_visible(w); w = w->parent) {
        enum ui_cursor c = w->ops && w->ops->cursor
            ? w->ops->cursor(w, surface->impl.pointer) : UI_CURSOR_AUTO;
        if (c == UI_CURSOR_AUTO && w->style.has_cursor)
            c = w->style.cursor;
        if (c != UI_CURSOR_AUTO)
            return c;
    }
    return UI_CURSOR_DEFAULT;
}

bool ui_surface_has_modal(const struct ui_surface *surface)
{
    return surface && ui_widget_modal(surface->impl.root) != NULL;
}

static void drop_retired(struct ui_surface *surface)
{
    if (surface->impl.focused && retired(surface, surface->impl.focused))
        surface->impl.focused = NULL;
    if (surface->impl.captured && retired(surface, surface->impl.captured))
        surface->impl.captured = NULL;
    if (surface->impl.hovered && retired(surface, surface->impl.hovered))
        surface->impl.hovered = NULL;
}

static bool is_interactive_widget(const struct ui_widget *widget,
                                  struct vector2i pos)
{
    if (widget && widget->ops && widget->ops->keeps_press_focus &&
        widget->ops->keeps_press_focus(widget, pos))
        return true;
    for (const struct ui_widget *w = widget; w; w = w->parent) {
        if (w->impl.focusable)
            return true;
    }
    return false;
}

static bool dispatch_pointer(struct ui_surface *surface,
                             const struct ui_event *event)
{
    const struct pointer_event *pe = &event->data.pointer;
    if (pe->kind != PTR_LEAVE)
        surface->impl.pointer = pe->pos;
    struct ui_widget *target = surface->impl.captured
        ? surface->impl.captured
        : ui_widget_hit_test(surface->impl.root, pe->pos);
    if (pe->kind == PTR_LEAVE)
        target = NULL;
    if (pe->kind == PTR_PRESS && pe->button == 0 &&
        !surface->impl.modal && !is_interactive_widget(target, pe->pos)) {
        ui_surface_focus(surface, NULL);
        ui_surface_sync_modal(surface);
        target = surface->impl.captured
            ? surface->impl.captured
            : ui_widget_hit_test(surface->impl.root, pe->pos);
    }
    if (pe->kind == PTR_MOTION || pe->kind == PTR_LEAVE) {
        struct ui_widget *previous = surface->impl.hovered;
        surface->impl.hovered = NULL;
        if (previous && previous != target) {
            struct ui_event out = *event;
            out.data.pointer.kind = PTR_LEAVE;
            ui_widget_bubble(previous, &out);
        }
        if (target && !retired(surface, target))
            surface->impl.hovered = target;
    }
    return target && !retired(surface, target) &&
           ui_widget_bubble(target, event);
}

#define UI_CAPTURE_PATH_MAX 64

static struct ui_widget *keyboard_target(const struct ui_surface *surface)
{
    if (surface->impl.focused)
        return surface->impl.focused;
    if (surface->impl.modal)
        return surface->impl.modal;
    struct ui_widget *fallback = surface->impl.default_focus;
    if (fallback && !retired(surface, fallback))
        return fallback;
    return surface->impl.root;
}

static bool capture_keyboard(struct ui_surface *surface,
                             struct ui_widget *target,
                             const struct ui_event *event)
{
    struct ui_widget *start = surface->impl.modal ? surface->impl.modal
                                                  : surface->impl.root;
    struct ui_widget *path[UI_CAPTURE_PATH_MAX];
    size_t depth = 0;
    struct ui_widget *w = target;
    for (; w; w = w->parent) {
        if (depth == UI_CAPTURE_PATH_MAX)
            return false;
        path[depth++] = w;
        if (w == start)
            break;
    }
    if (!w)
        return false;
    for (size_t i = depth; i > 0; i--) {
        struct ui_widget *c = path[i - 1];
        if (c->impl.destroyed || c->impl.surface != surface)
            break;
        if (c->impl.key_capture &&
            c->impl.key_capture(c, event, c->impl.key_capture_userdata))
            return true;
    }
    return false;
}

static bool dispatch_keyboard(struct ui_surface *surface,
                              const struct ui_event *event)
{
    struct ui_widget *target = keyboard_target(surface);
    if (capture_keyboard(surface, target, event) ||
        ui_widget_bubble(target, event))
        return true;
    if (event->kind != UI_EVENT_KEY)
        return false;
    const struct ui_key_event *key = &event->data.key;
    if (key->key != UI_KEY_TAB || key->action == UI_KEY_RELEASE ||
        (key->mods & (UI_MOD_ALT | UI_MOD_CTRL | UI_MOD_SUPER)))
        return false;
    return key->mods & UI_MOD_SHIFT ? ui_surface_focus_prev(surface)
                                    : ui_surface_focus_next(surface);
}

static bool keyboard_path(enum ui_event_kind kind)
{
    switch (kind) {
    case UI_EVENT_KEY:
    case UI_EVENT_TEXT:
    case UI_EVENT_PASTE:
    case UI_EVENT_SHORTCUT:
    case UI_EVENT_DROP_FILES:
        return true;
    default:
        return false;
    }
}

bool ui_surface_dispatch(struct ui_surface *surface,
                         const struct ui_event *event)
{
    if (!surface || !event || surface->impl.busy)
        return false;
    surface->impl.busy = true;
    surface->impl.key_text_kept = false;
    prepare(surface);
    bool handled = false;
    if (surface->impl.root) {
        drop_retired(surface);
        ui_surface_sync_modal(surface);
        switch (event->kind) {
        case UI_EVENT_POINTER:
            handled = dispatch_pointer(surface, event);
            break;
        case UI_EVENT_KEY:
        case UI_EVENT_TEXT:
        case UI_EVENT_SHORTCUT:
        case UI_EVENT_DROP_FILES:
            handled = dispatch_keyboard(surface, event);
            break;
        case UI_EVENT_PASTE:
            if (surface->impl.paste_target) {
                struct ui_widget *target = surface->impl.paste_target;
                surface->impl.paste_target = NULL;
                handled = !retired(surface, target) && ui_widget_bubble(target, event);
            } else {
                handled = dispatch_keyboard(surface, event);
            }
            break;
        default:
            handled = ui_widget_bubble(surface->impl.focused
                                       ? surface->impl.focused
                                       : surface->impl.root, event);
            break;
        }
        ui_surface_sync_modal(surface);
    }
    surface->impl.busy = false;
    release_retired_dialogs(surface);
    if (handled && !keyboard_path(event->kind))
        ui_surface_request_frame(surface);
    return handled;
}

void ui_surface_set_default_focus(struct ui_surface *surface,
                                  struct ui_widget *widget)
{
    if (surface)
        surface->impl.default_focus = widget;
}

void ui_surface_keep_key_text(struct ui_surface *surface)
{
    if (surface)
        surface->impl.key_text_kept = true;
}

bool ui_surface_key_text_kept(const struct ui_surface *surface)
{
    return surface && surface->impl.key_text_kept;
}

void ui_surface_focus(struct ui_surface *surface, struct ui_widget *widget)
{
    if (!surface)
        return;
    if (widget && retired(surface, widget))
        widget = NULL;
    if (surface->impl.focused == widget)
        return;
    ui_surface_request_frame(surface);
    struct ui_widget *previous = surface->impl.focused;
    surface->impl.focused = NULL;
    send(previous, ui_event_focus(false, 0));
    if (surface->impl.focused)
        return;
    if (widget && retired(surface, widget))
        widget = NULL;
    surface->impl.focused = widget;
    send(widget, ui_event_focus(true, 0));
    if (widget && surface->impl.focused == widget && retired(surface, widget))
        surface->impl.focused = NULL;
    if (widget && surface->impl.focused == widget)
        for (struct ui_widget *parent = widget->parent; parent; parent = parent->parent)
            if (parent->ops && parent->ops->reveal)
                parent->ops->reveal(parent, widget);
}

struct ui_widget *ui_surface_focused(const struct ui_surface *surface)
{
    return surface ? surface->impl.focused : NULL;
}

struct ui_widget *ui_surface_captured(const struct ui_surface *surface)
{
    return surface ? surface->impl.captured : NULL;
}

struct ui_widget *ui_surface_context_target(const struct ui_surface *surface)
{
    return surface ? surface->impl.context_target : NULL;
}

void ui_surface_set_context_target(struct ui_surface *surface, struct ui_widget *target)
{
    if (!surface)
        return;
    surface->impl.context_target = target && target->impl.surface == surface
        ? target : NULL;
}

struct focus_walk {
    const struct ui_widget *focused;
    struct ui_widget *first, *last;
    struct ui_widget *before, *after;
    bool seen;
};

static void walk_focusable(struct ui_widget *widget, struct focus_walk *fw)
{
    if (!widget || widget->impl.destroyed || widget->impl.hidden)
        return;
    if (widget->impl.focusable) {
        if (!fw->first)
            fw->first = widget;
        fw->last = widget;
        if (widget == fw->focused)
            fw->seen = true;
        else if (!fw->seen)
            fw->before = widget;
        else if (!fw->after)
            fw->after = widget;
    }
    for (struct ui_widget *c = widget->first_child; c; c = c->next_sibling)
        walk_focusable(c, fw);
}

static bool focus_step(struct ui_surface *surface, int direction)
{
    if (!surface || !surface->impl.root)
        return false;
    ui_surface_sync_modal(surface);
    struct focus_walk fw = { .focused = surface->impl.focused };
    walk_focusable(surface->impl.modal ? surface->impl.modal
                                       : surface->impl.root, &fw);
    if (!fw.first)
        return false;
    struct ui_widget *next;
    if (!fw.seen)
        next = direction > 0 ? fw.first : fw.last;
    else if (direction > 0)
        next = fw.after ? fw.after : fw.first;
    else
        next = fw.before ? fw.before : fw.last;
    ui_surface_focus(surface, next);
    return true;
}

bool ui_surface_focus_next(struct ui_surface *surface)
{
    return focus_step(surface, 1);
}

bool ui_surface_focus_prev(struct ui_surface *surface)
{
    return focus_step(surface, -1);
}

bool ui_surface_tick(struct ui_surface *surface, uint64_t now_ms)
{
    if (!surface || surface->impl.busy)
        return false;
    surface->impl.busy = true;
    surface->impl.deadline = surface->impl.root
        ? ui_widget_tick(surface->impl.root, now_ms) : 0;
    surface->impl.busy = false;
    release_retired_dialogs(surface);
    bool next_frame = surface->impl.deadline &&
                      surface->impl.deadline <= now_ms;
    if (next_frame)
        ui_surface_request_frame(surface);
    return next_frame;
}

uint64_t ui_surface_next_deadline(const struct ui_surface *surface)
{
    return surface ? surface->impl.deadline : 0;
}

bool ui_surface_build_scene(struct ui_surface *surface,
                            struct ui_scene *scene)
{
    if (!scene || (surface && surface->impl.busy))
        return false;
    if (!surface || !surface->impl.root) {
        ui_scene_init(scene);
        return true;
    }
    surface->impl.busy = true;
    prepare(surface);
    bool valid = ui_widget_build_scene(surface->impl.root, scene);
    surface->impl.busy = false;
    release_retired_dialogs(surface);
    return valid;
}

void ui_surface_destroy(struct ui_surface *surface)
{
    if (!surface || surface->impl.busy)
        return;
    ui_dialog_close(surface);
    release_retired_dialogs(surface);
    while (surface->impl.root)
        ui_widget_detach(surface->impl.root);
    ui_style_context_destroy(&surface->impl.style);
    ui_layout_destroy(surface->impl.layout);
    *surface = (struct ui_surface){0};
}

void *ui_surface_scratch_alloc(struct ui_surface *surface, size_t size)
{
    if (!surface || size == 0)
        return NULL;
    size_t align = sizeof(void *) > 8 ? sizeof(void *) : 8;
    size_t aligned = (surface->impl.scratch_used + (align - 1)) & ~(align - 1);
    if (aligned > sizeof(surface->impl.scratch_buf) ||
        size > sizeof(surface->impl.scratch_buf) - aligned)
        return NULL;
    void *ptr = &surface->impl.scratch_buf[aligned];
    memset(ptr, 0, size);
    surface->impl.scratch_used = aligned + size;
    return ptr;
}

void ui_surface_scratch_reset(struct ui_surface *surface)
{
    if (surface)
        surface->impl.scratch_used = 0;
}

void ui_dialog_close(struct ui_surface *surface)
{
    if (!surface || !surface->impl.dialog)
        return;
    struct ui_widget *dialog = surface->impl.dialog;
    struct ui_dialog_frame *frame = surface->impl.dialog_storage;
    surface->impl.dialog = NULL;
    surface->impl.dialog_storage = NULL;
    surface->impl.dialog_generation++;
    if (surface->impl.root)
        ui_widget_remove_child(surface->impl.root, dialog);
    ui_widget_destroy(dialog);
    if (frame) {
        if (surface->impl.busy) {
            frame->next_retired = surface->impl.retired_dialogs;
            surface->impl.retired_dialogs = frame;
        } else {
            free(frame);
        }
    }
    ui_surface_request_frame(surface);
}

static bool dialog_action_forwarder(struct ui_widget *source,
                                    const struct ui_action *action,
                                    void *userdata)
{
    struct ui_dialog_frame *f = userdata;
    struct ui_widget *dialog = UI_WIDGET(&f->card);
    struct ui_surface *surface = f->surface;
    uint64_t generation = surface->impl.dialog_generation;
    ui_action_fn on_action = f->on_action;
    void *action_userdata = f->userdata;
    bool activate = action->kind == UI_ACTION_ACTIVATE;
    bool handled = false;
    if (on_action)
        handled = on_action(source, action, action_userdata);
    if (activate) {
        if (surface->impl.dialog == dialog &&
            surface->impl.dialog_generation == generation)
            ui_dialog_close(surface);
        return true;
    }
    return handled;
}

struct ui_widget *ui_dialog_show(struct ui_surface *surface,
                                 const struct ui_dialog_desc *desc)
{
    if (!surface || !desc || !surface->impl.root)
        return NULL;
    ui_dialog_close(surface);

    struct ui_dialog_frame *f = calloc(1, sizeof(*f));
    if (!f)
        return NULL;

    f->surface = surface;
    f->on_action = desc->on_action;
    f->userdata = desc->userdata;

    ui_box_set_direction(&f->card, UI_BOX_COLUMN);
    ui_box_set_gap(&f->card, 8);
    ui_widget_set_style_identity(UI_WIDGET(&f->card), "dialog", NULL, "dialog");

    if (desc->title) {
        ui_label_set_text(&f->title, desc->title);
        ui_widget_set_style_identity(UI_WIDGET(&f->title), "dialog-title", NULL, NULL);
        ui_widget_add_child(UI_WIDGET(&f->card), UI_WIDGET(&f->title));
    }

    if (desc->message) {
        ui_label_set_text(&f->msg, desc->message);
        ui_widget_set_style_identity(UI_WIDGET(&f->msg), "dialog-message", NULL, NULL);
        ui_widget_add_child(UI_WIDGET(&f->card), UI_WIDGET(&f->msg));
    }

    int btn_count = desc->button_count;
    if (btn_count > UI_DIALOG_MAX_BUTTONS)
        btn_count = UI_DIALOG_MAX_BUTTONS;

    if (btn_count > 0) {
        ui_box_set_direction(&f->btn_row, UI_BOX_ROW);
        ui_box_set_gap(&f->btn_row, 8);
        ui_box_set_justify(&f->btn_row, UI_BOX_JUSTIFY_END);
        ui_widget_set_style_identity(UI_WIDGET(&f->btn_row), "dialog-buttons", NULL, NULL);

        for (int i = 0; i < btn_count; i++) {
            ui_button_set_text(&f->btns[i], desc->buttons[i].text ? desc->buttons[i].text : "OK");
            ui_button_set_action_id(&f->btns[i], desc->buttons[i].action_id);
            ui_widget_add_child(UI_WIDGET(&f->btn_row), UI_WIDGET(&f->btns[i]));
        }
        ui_widget_add_child(UI_WIDGET(&f->card), UI_WIDGET(&f->btn_row));
    }

    ui_widget_set_action_handler(UI_WIDGET(&f->card), dialog_action_forwarder, f);
    ui_widget_set_placement(UI_WIDGET(&f->card), (struct ui_placement){
        .kind = UI_PLACE_CENTER,
        .gap = 16,
        .y_fraction = 0.5f,
    });

    if (!ui_widget_add_child(surface->impl.root, UI_WIDGET(&f->card))) {
        ui_widget_destroy(UI_WIDGET(&f->card));
        free(f);
        return NULL;
    }

    surface->impl.dialog = UI_WIDGET(&f->card);
    surface->impl.dialog_storage = f;
    surface->impl.dialog_generation++;
    ui_widget_set_modal(UI_WIDGET(&f->card), true);

    if (btn_count > 0)
        ui_surface_focus(surface, UI_WIDGET(&f->btns[0]));

    ui_surface_request_frame(surface);
    return UI_WIDGET(&f->card);
}
