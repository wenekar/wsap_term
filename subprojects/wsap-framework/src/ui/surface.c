#include "ui/surface.h"

#include "input/command.h"
#include "ui/internal/tree.h"
#include "paint/scene.h"
#include "ui/css.h"

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

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

bool ui_surface_can_copy(const struct ui_surface *surface)
{
    return surface && surface->impl.host.copy_text;
}

bool ui_surface_can_paste(const struct ui_surface *surface)
{
    return surface && surface->impl.host.paste_text;
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

bool ui_surface_can_copy_primary(const struct ui_surface *surface)
{
    return surface && surface->impl.host.copy_primary;
}

bool ui_surface_can_paste_primary(const struct ui_surface *surface)
{
    return surface && surface->impl.host.paste_primary;
}

bool ui_surface_copy_primary(struct ui_surface *surface, const char *text)
{
    if (!surface || !text || !surface->impl.host.copy_primary)
        return false;
    surface->impl.host.copy_primary(surface->impl.host_userdata, text);
    return true;
}

bool ui_surface_paste_primary(struct ui_surface *surface)
{
    if (!surface || !surface->impl.host.paste_primary)
        return false;
    surface->impl.paste_target = surface->impl.focused;
    surface->impl.host.paste_primary(surface->impl.host_userdata);
    return true;
}

bool ui_surface_set_text_input(struct ui_surface *surface,
                               const struct ui_text_input *input)
{
    if (!surface || !surface->impl.host.set_text_input)
        return false;
    surface->impl.host.set_text_input(surface->impl.host_userdata, input);
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
        return UI_DOUBLE_CLICK_FALLBACK_MS;
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

/* Appends `fn` to the queue. The queue grows, so a poster never has to free
 * storage inside a dispatch. */
static bool post_push(struct ui_surface *surface, ui_post_fn fn, void *userdata)
{
    if (surface->impl.post_count == surface->impl.post_cap) {
        size_t cap = surface->impl.post_cap ? surface->impl.post_cap * 2 : 4;
        struct ui_surface_post *grown =
            realloc(surface->impl.post, cap * sizeof(*grown));
        if (!grown)
            return false;
        surface->impl.post = grown;
        surface->impl.post_cap = cap;
    }
    surface->impl.post[surface->impl.post_count++] =
        (struct ui_surface_post){ .fn = fn, .userdata = userdata };
    return true;
}

bool ui_surface_post(struct ui_surface *surface, ui_post_fn fn, void *userdata)
{
    if (!fn)
        return false;
    if (!surface) {
        fn(NULL, userdata);
        return true;
    }
    if (!surface->impl.busy) {
        fn(surface, userdata);
        return true;
    }
    return post_push(surface, fn, userdata);
}

/* Runs the posted functions. Call it last in each entry point, after the busy
 * section ends, so that no widget in use is freed.
 *
 * The queue is taken before the functions run, because one of them can
 * destroy the surface and post again. The caller of the entry point can still
 * read the surface, so a posted function never frees its storage. A function
 * that posts again runs at once: the surface is not busy here. */
static void flush_posts(struct ui_surface *surface)
{
    size_t count = surface->impl.post_count;
    if (!count)
        return;
    struct ui_surface_post *posts = surface->impl.post;
    surface->impl.post = NULL;
    surface->impl.post_count = 0;
    surface->impl.post_cap = 0;
    uint64_t generation = surface->impl.generation;
    for (size_t i = 0; i < count; i++) {
        posts[i].fn(surface, posts[i].userdata);
        if (surface->impl.generation != generation)
            break;
    }
    free(posts);
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

static bool overlay_live(const struct ui_widget *overlay)
{
    return overlay && !overlay->impl.destroyed && !overlay->impl.hidden;
}

/* Removes overlay `index` from the list. It does not detach the widget. */
static void drop_overlay_at(struct ui_surface *surface, size_t index)
{
    if (index >= surface->impl.overlay_count)
        return;
    for (size_t i = index; i + 1 < surface->impl.overlay_count; i++)
        surface->impl.overlays[i] = surface->impl.overlays[i + 1];
    surface->impl.overlay_count--;
    surface->impl.overlays[surface->impl.overlay_count] = NULL;
}

/* Removes each overlay below `subtree`, without detaching it. A subtree that
 * is itself an overlay stays registered: a hide only hides it. */
static void drop_overlays_in(struct ui_surface *surface, struct ui_widget *subtree)
{
    for (size_t i = surface->impl.overlay_count; i > 0; i--) {
        struct ui_widget *overlay = surface->impl.overlays[i - 1];
        if (overlay != subtree && ui_widget_is_ancestor(subtree, overlay))
            drop_overlay_at(surface, i - 1);
    }
}

bool ui_surface_drop_overlay(struct ui_surface *surface,
                             struct ui_widget *overlay)
{
    if (!surface || !overlay)
        return false;
    for (size_t i = 0; i < surface->impl.overlay_count; i++)
        if (surface->impl.overlays[i] == overlay) {
            drop_overlay_at(surface, i);
            return true;
        }
    return false;
}

/* Makes room for one more overlay. The list grows, so an owner never reaches a
 * fixed limit. Returns false only when memory is short. */
static bool overlay_push(struct ui_surface *surface, struct ui_widget *overlay)
{
    if (surface->impl.overlay_count == surface->impl.overlay_cap) {
        size_t cap = surface->impl.overlay_cap ? surface->impl.overlay_cap * 2 : 4;
        struct ui_widget **grown =
            realloc(surface->impl.overlays, cap * sizeof(*grown));
        if (!grown)
            return false;
        surface->impl.overlays = grown;
        surface->impl.overlay_cap = cap;
    }
    surface->impl.overlays[surface->impl.overlay_count++] = overlay;
    return true;
}

bool ui_surface_add_overlay(struct ui_surface *surface,
                            struct ui_widget *overlay)
{
    if (!surface || !overlay || overlay->impl.destroyed || overlay->parent ||
        (overlay->impl.surface && overlay->impl.surface != surface) ||
        !overlay_push(surface, overlay))
        return false;
    ui_widget_attach(overlay, surface);
    ui_widget_invalidate(overlay, UI_WIDGET_INVALIDATE_MEASURE);
    ui_surface_sync_modal(surface);
    ui_surface_prepare(surface);
    ui_surface_request_frame(surface);
    return true;
}

bool ui_surface_remove_overlay(struct ui_surface *surface,
                               struct ui_widget *overlay)
{
    if (!ui_surface_drop_overlay(surface, overlay))
        return false;
    ui_widget_detach(overlay);
    ui_surface_sync_modal(surface);
    ui_surface_prepare(surface);
    ui_surface_request_frame(surface);
    return true;
}

size_t ui_surface_overlay_count(const struct ui_surface *surface)
{
    return surface ? surface->impl.overlay_count : 0;
}

struct ui_widget *ui_surface_overlay_at(const struct ui_surface *surface,
                                        size_t index)
{
    return surface && index < surface->impl.overlay_count
        ? surface->impl.overlays[index] : NULL;
}

static void measure_overlays(struct ui_surface *surface)
{
    struct rect2i viewport = { .size = surface->impl.viewport };
    /* Downwards: a measure or a place can close an overlay, which shifts the
     * rest of the list down. The overlays below the entry are unaffected. */
    for (size_t i = surface->impl.overlay_count; i > 0; i--) {
        struct ui_widget *overlay = surface->impl.overlays[i - 1];
        if (!overlay_live(overlay))
            continue;
        if (overlay->impl.invalidation & UI_WIDGET_INVALIDATE_MEASURE)
            ui_widget_measure(overlay, viewport_constraints(surface));
        ui_widget_place_child(overlay, viewport);
    }
}

static void measure_and_layout(struct ui_surface *surface)
{
    struct ui_widget *root = surface->impl.root;
    if (!root) {
        measure_overlays(surface);
        return;
    }
    if (root->impl.invalidation & UI_WIDGET_INVALIDATE_MEASURE)
        ui_widget_measure(root, viewport_constraints(surface));
    if (root->impl.invalidation & UI_WIDGET_INVALIDATE_MEASURE) {
        measure_overlays(surface);
        ui_surface_request_frame(surface);
        return;
    }
    struct vector2i size = surface->impl.viewport;
    if ((root->impl.invalidation & UI_WIDGET_INVALIDATE_LAYOUT) &&
        size.x > 0 && size.y > 0)
        ui_widget_layout(root, (struct rect2i){ .size = size });
    /* The overlays follow the root, because a root layout can move them. */
    measure_overlays(surface);
}

/* Adds the hover and focus flags of the surface to each widget of the tree.
 * A widget keeps the flags that its own handlers set. */
static void sync_state_pseudo(struct ui_surface *surface, struct ui_widget *widget)
{
    if (!widget || widget->impl.destroyed)
        return;
    unsigned bits = 0;
    if (surface->impl.hovered &&
        ui_widget_is_ancestor(widget, surface->impl.hovered))
        bits |= CSS_PSEUDO_HOVER;
    if (surface->impl.focused &&
        ui_widget_is_ancestor(widget, surface->impl.focused))
        bits |= CSS_PSEUDO_FOCUS;
    unsigned current = ui_widget_style_pseudo(widget);
    unsigned next = (current & ~(unsigned)widget->impl.style_pseudo_auto) | bits;
    widget->impl.style_pseudo_auto = (uint16_t)bits;
    if (next != current)
        ui_widget_set_style_pseudo(widget, next);
    for (struct ui_widget *c = widget->first_child; c; c = c->next_sibling)
        sync_state_pseudo(surface, c);
}

/* Moves each widget toward its resolved CSS colors. The CSS
 * transition-duration sets the time. */
static void animate_styles(struct ui_surface *surface)
{
    uint64_t previous = surface->impl.anim_deadline;
    uint64_t next = 0;
    if (surface->impl.root) {
        sync_state_pseudo(surface, surface->impl.root);
        ui_style_context_apply(&surface->impl.style, surface->impl.root);
        next = ui_widget_animate_style(surface->impl.root,
                                       surface->impl.now_ms);
    }
    for (size_t i = 0; i < surface->impl.overlay_count; i++) {
        struct ui_widget *overlay = surface->impl.overlays[i];
        if (!overlay || overlay->impl.destroyed)
            continue;
        sync_state_pseudo(surface, overlay);
        ui_style_context_apply(&surface->impl.style, overlay);
        uint64_t child = ui_widget_animate_style(overlay, surface->impl.now_ms);
        if (child && (!next || child < next))
            next = child;
    }
    /* A deadline that this pass published last time gives way to the new
     * one. The caller reads the next deadline after this pass. */
    if (previous && surface->impl.deadline == previous)
        surface->impl.deadline = 0;
    surface->impl.anim_deadline = next;
    if (next && (!surface->impl.deadline ||
                 surface->impl.deadline <= surface->impl.now_ms ||
                 next < surface->impl.deadline))
        surface->impl.deadline = next;
}

static void prepare_overlays(struct ui_surface *surface)
{
    /* Downwards: a prepare can close an overlay and shift the list. */
    for (size_t i = surface->impl.overlay_count; i > 0; i--) {
        struct ui_widget *overlay = surface->impl.overlays[i - 1];
        ui_widget_prepare(overlay);
        ui_style_context_apply(&surface->impl.style, overlay);
    }
}

static void prepare(struct ui_surface *surface)
{
    struct ui_widget *root = surface->impl.root;
    if (!root) {
        prepare_overlays(surface);
        measure_and_layout(surface);
        animate_styles(surface);
        ui_surface_sync_modal(surface);
        return;
    }
    ui_widget_prepare(root);
    ui_style_context_apply(&surface->impl.style, root);
    prepare_overlays(surface);
    root = surface->impl.root;
    if (!root) {
        measure_and_layout(surface);
        animate_styles(surface);
        return;
    }
    measure_and_layout(surface);
    animate_styles(surface);
    ui_surface_sync_modal(surface);
}

void ui_surface_prepare(struct ui_surface *surface)
{
    if (!surface || surface->impl.busy)
        return;
    surface->impl.busy = true;
    prepare(surface);
    surface->impl.busy = false;
    flush_posts(surface);
}

bool ui_surface_set_root(struct ui_surface *surface, struct ui_widget *root)
{
    if (!surface || (root && (root->impl.destroyed || root->parent)))
        return false;
    if (surface->impl.root != root) {
        while (surface->impl.overlay_count)
            ui_surface_remove_overlay(surface,
                surface->impl.overlays[surface->impl.overlay_count - 1]);
        while (surface->impl.root)
            ui_widget_detach(surface->impl.root);
        surface->impl.focused = NULL;
        surface->impl.captured = NULL;
        surface->impl.hovered = NULL;
        surface->impl.modal = NULL;
        surface->impl.focus_count = 0;
        surface->impl.default_focus = NULL;
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

void ui_surface_set_overlay_sheet(struct ui_surface *surface,
                                  const struct css_sheet *sheet)
{
    if (!surface)
        return;
    ui_style_context_set_overlay_sheet(&surface->impl.style, sheet);
    if (surface->impl.root)
        ui_widget_invalidate(surface->impl.root, UI_WIDGET_INVALIDATE_MEASURE);
}

void ui_surface_set_scale(struct ui_surface *surface,
                          struct css_scale_context scale)
{
    if (!surface)
        return;
    uint64_t before = ui_style_context_generation(&surface->impl.style);
    ui_style_context_set_scale(&surface->impl.style, scale);
    if (before != ui_style_context_generation(&surface->impl.style) &&
        surface->impl.root)
        ui_widget_invalidate(surface->impl.root, UI_WIDGET_INVALIDATE_MEASURE);
    ui_surface_prepare(surface);
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
    if (!surface)
        return;
    if (!vector2i_eq(surface->impl.viewport, viewport)) {
        surface->impl.viewport = viewport;
        if (surface->impl.root)
            ui_widget_invalidate(surface->impl.root,
                                 UI_WIDGET_INVALIDATE_MEASURE);
        for (size_t i = 0; i < surface->impl.overlay_count; i++)
            ui_widget_invalidate(surface->impl.overlays[i],
                                 UI_WIDGET_INVALIDATE_MEASURE);
    }
    ui_surface_prepare(surface);
}

struct vector2i ui_surface_viewport(const struct ui_surface *surface)
{
    return surface ? surface->impl.viewport : (struct vector2i){0};
}


static void send(struct ui_widget *widget, struct ui_event event)
{
    if (widget && !widget->impl.destroyed)
        ui_widget_deliver(widget, &event);
}

static void leave(struct ui_widget *widget)
{
    send(widget, ui_event_pointer(
        (struct ui_pointer_event){ .kind = UI_POINTER_LEAVE }, 0));
}

struct ui_widget *ui_surface_drag_hover(struct ui_surface *surface,
                                        struct ui_widget *target, bool *enter)
{
    if (enter)
        *enter = false;
    if (!surface)
        return NULL;
    struct drop_hover *hover = &surface->impl.drop_hover;
    struct ui_widget *previous = hover->target;
    bool moved = previous != target;
    bool left = hover->active && moved;
    hover->previous = moved ? previous : NULL;
    hover->entered = moved ? false : hover->active;
    hover->target = target;
    hover->active = target != NULL;
    if (enter)
        *enter = !hover->entered;
    return left ? hover->previous : NULL;
}

struct ui_widget *ui_surface_drag_target(const struct ui_surface *surface)
{
    return surface ? surface->impl.drop_hover.target : NULL;
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

/* Returns true when `widget` or an ancestor of it up to `subtree` is being
 * destroyed. */
static bool destroying(const struct ui_widget *widget,
                       const struct ui_widget *subtree)
{
    for (const struct ui_widget *w = widget; w; w = w->parent) {
        if (w->impl.destroyed)
            return true;
        if (w == subtree)
            break;
    }
    return false;
}

void ui_surface_forget(struct ui_surface *surface, struct ui_widget *subtree)
{
    drop_overlays_in(surface, subtree);
    struct ui_widget *captured = surface->impl.captured;
    if (captured && ui_widget_is_ancestor(subtree, captured))
        ui_surface_cancel_capture(surface);
    struct ui_widget *hovered = surface->impl.hovered;
    if (hovered && ui_widget_is_ancestor(subtree, hovered)) {
        surface->impl.hovered = NULL;
        leave(hovered);
    }
    struct drop_hover *drag = &surface->impl.drop_hover;
    if (drag->target && ui_widget_is_ancestor(subtree, drag->target)) {
        struct ui_widget *target = drag->target;
        bool active = drag->active;
        drag->target = NULL;
        drag->active = false;
        drag->entered = false;
        /* The drag mark clears as on a LEAVE. A subtree that a destroy
         * forgets gets no event. */
        if (active && !destroying(target, subtree)) {
            struct ui_drag_event leave_drag = {0};
            struct ui_event leave_event =
                ui_event_drag(UI_EVENT_DRAG_LEAVE, &leave_drag, 0);
            ui_widget_bubble_to(target, subtree, &leave_event);
        }
    }
    if (drag->previous && ui_widget_is_ancestor(subtree, drag->previous))
        drag->previous = NULL;
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
        &surface->impl.default_focus,
        &surface->impl.paste_target, &surface->impl.context_target,
    };
    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++)
        if (*refs[i] && ui_widget_is_ancestor(subtree, *refs[i]))
            *refs[i] = NULL;
    for (size_t i = 0; i < surface->impl.focus_count; i++) {
        struct ui_surface_focus_frame *frame = &surface->impl.focus_stack[i];
        if (frame->focus && ui_widget_is_ancestor(subtree, frame->focus))
            frame->focus = NULL;
    }
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

/* The topmost modal widget: one in an overlay masks the overlays below it and
 * the root. */
static struct ui_widget *surface_modal(struct ui_surface *surface)
{
    for (size_t i = surface->impl.overlay_count; i > 0; i--) {
        struct ui_widget *modal = ui_widget_modal(surface->impl.overlays[i - 1]);
        if (modal)
            return modal;
    }
    return ui_widget_modal(surface->impl.root);
}

/* True when `widget` still hangs from the root or from an overlay. */
static bool surface_holds(const struct ui_surface *surface,
                          const struct ui_widget *widget)
{
    if (!widget)
        return false;
    if (widget == surface->impl.root)
        return true;
    for (size_t i = 0; i < surface->impl.overlay_count; i++)
        if (surface->impl.overlays[i] == widget)
            return true;
    return false;
}

/* True while `widget` is a modal of `surface` that holds its focus. */
static bool modal_live(const struct ui_surface *surface,
                       const struct ui_widget *widget)
{
    if (!widget || widget->impl.destroyed || !widget->impl.modal ||
        widget->impl.surface != surface || !ui_widget_visible(widget))
        return false;
    for (const struct ui_widget *w = widget; w; w = w->parent)
        if (surface_holds(surface, w))
            return true;
    return false;
}

/* The modal of the top frame, or NULL when the stack is empty. */
static struct ui_widget *focus_top(const struct ui_surface *surface)
{
    return surface->impl.focus_count
        ? surface->impl.focus_stack[surface->impl.focus_count - 1].modal : NULL;
}

static bool focus_push(struct ui_surface *surface, struct ui_widget *modal,
                       struct ui_widget *focus)
{
    if (surface->impl.focus_count == surface->impl.focus_cap) {
        size_t cap = surface->impl.focus_cap ? surface->impl.focus_cap * 2 : 4;
        struct ui_surface_focus_frame *grown =
            realloc(surface->impl.focus_stack, cap * sizeof(*grown));
        if (!grown)
            return false;
        surface->impl.focus_stack = grown;
        surface->impl.focus_cap = cap;
    }
    surface->impl.focus_stack[surface->impl.focus_count++] =
        (struct ui_surface_focus_frame){ .modal = modal, .focus = focus };
    return true;
}

/* Pops each frame whose modal left the surface, and returns the oldest focus
 * that is still valid. A modal that closes below another one keeps its frame
 * until the modal above it closes too. */
static struct ui_widget *pop_retired_frames(struct ui_surface *surface)
{
    struct ui_widget *restore = NULL;
    while (surface->impl.focus_count) {
        const struct ui_surface_focus_frame *frame =
            &surface->impl.focus_stack[surface->impl.focus_count - 1];
        if (modal_live(surface, frame->modal))
            break;
        struct ui_widget *focus = frame->focus;
        surface->impl.focus_count--;
        if (focus && !ui_widget_stale(surface, focus))
            restore = focus;
    }
    return restore;
}

void ui_surface_sync_modal(struct ui_surface *surface)
{
    struct ui_widget *modal = surface_modal(surface);
    if (modal == focus_top(surface)) {
        surface->impl.modal = modal;
        if (modal && !ui_widget_is_ancestor(modal, surface->impl.focused))
            ui_surface_focus(surface, first_focusable(modal));
        return;
    }
    /* Each modal that takes the focus pushes a frame, and each frame pops when
     * its modal leaves. The stack thus follows the modals in any order. */
    struct ui_widget *restore = pop_retired_frames(surface);
    struct ui_widget *focused = surface->impl.focused;
    if (modal) {
        bool inside = focused && ui_widget_is_ancestor(modal, focused);
        focus_push(surface, modal, inside ? NULL : focused);
        surface->impl.modal = modal;
        if (surface->impl.captured &&
            !ui_widget_is_ancestor(modal, surface->impl.captured))
            ui_surface_cancel_capture(surface);
        struct ui_widget *hovered = surface->impl.hovered;
        if (hovered && !ui_widget_is_ancestor(modal, hovered)) {
            surface->impl.hovered = NULL;
            send(hovered, ui_event_pointer(
                (struct ui_pointer_event){ .kind = UI_POINTER_LEAVE }, 0));
        }
    } else {
        surface->impl.modal = NULL;
    }
    /* The restore gives way to the modal that is still open. */
    if (restore && (!modal || ui_widget_is_ancestor(modal, restore)) &&
        !ui_widget_stale(surface, restore)) {
        ui_surface_focus(surface, restore);
        return;
    }
    if (modal && !ui_widget_is_ancestor(modal, surface->impl.focused))
        ui_surface_focus(surface, first_focusable(modal));
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
    return surface && surface_modal((struct ui_surface *)surface) != NULL;
}

static void drop_retired(struct ui_surface *surface)
{
    if (surface->impl.focused && ui_widget_stale(surface, surface->impl.focused))
        surface->impl.focused = NULL;
    if (surface->impl.captured && ui_widget_stale(surface, surface->impl.captured))
        surface->impl.captured = NULL;
    if (surface->impl.hovered && ui_widget_stale(surface, surface->impl.hovered))
        surface->impl.hovered = NULL;
}

static bool is_interactive_widget(const struct ui_widget *widget)
{
    for (const struct ui_widget *w = widget; w; w = w->parent) {
        if (w->impl.keeps_press_focus)
            return true;
        if (w->impl.focusable)
            return true;
    }
    return false;
}

struct ui_widget *ui_surface_hit_test(struct ui_surface *surface,
                                      struct vector2i pos)
{
    if (!surface)
        return NULL;
    for (size_t i = surface->impl.overlay_count; i > 0; i--) {
        struct ui_widget *overlay = surface->impl.overlays[i - 1];
        if (!overlay_live(overlay))
            continue;
        /* A modal overlay takes a press that misses it, so that it can
         * close, and it masks everything below. */
        struct ui_widget *hit = ui_widget_hit_test(overlay, pos);
        if (hit)
            return hit;
    }
    return surface->impl.root
        ? ui_widget_hit_test(surface->impl.root, pos) : NULL;
}

struct ui_widget *ui_surface_drop_target(struct ui_surface *surface,
                                         struct vector2i pos)
{
    struct ui_widget *hit = ui_surface_hit_test(surface, pos);
    struct ui_widget *modal = hit ? surface_modal(surface) : NULL;
    /* A modal masks the widgets below it, and the hit test gives the modal
     * itself for a point that misses it. A part of the modal can lie outside
     * the bounds of the modal, so only a hit on the modal itself checks the
     * bounds. */
    if (modal && (!ui_widget_is_ancestor(modal, hit) ||
                  (hit == modal && !rect2i_contains(modal->bounds, pos))))
        return NULL;
    return hit;
}

bool ui_surface_drop_event(struct ui_surface *surface,
                           struct ui_widget *target,
                           const struct ui_event *event)
{
    if (!surface || !target || !event)
        return false;
    struct ui_widget *modal = surface_modal(surface);
    return ui_widget_bubble_to(target,
        modal && ui_widget_is_ancestor(modal, target) ? modal : NULL, event);
}

static bool dispatch_pointer(struct ui_surface *surface,
                             const struct ui_event *event)
{
    const struct ui_pointer_event *pe = &event->data.pointer;
    if (pe->kind != UI_POINTER_LEAVE)
        surface->impl.pointer = pe->pos;
    struct ui_widget *target = surface->impl.captured
        ? surface->impl.captured
        : ui_surface_hit_test(surface, pe->pos);
    if (pe->kind == UI_POINTER_LEAVE)
        target = NULL;
    if (pe->kind == UI_POINTER_PRESS && pe->button == 0 &&
        !surface->impl.modal && !is_interactive_widget(target)) {
        ui_surface_focus(surface, NULL);
        ui_surface_sync_modal(surface);
        target = surface->impl.captured
            ? surface->impl.captured
            : ui_surface_hit_test(surface, pe->pos);
    }
    if (pe->kind == UI_POINTER_MOTION || pe->kind == UI_POINTER_LEAVE) {
        struct ui_widget *previous = surface->impl.hovered;
        surface->impl.hovered = NULL;
        if (previous && previous != target) {
            /* Hover leave goes to the widget that lost the pointer only. */
            struct ui_event out = *event;
            out.data.pointer.kind = UI_POINTER_LEAVE;
            send(previous, out);
        }
        if (target && !ui_widget_stale(surface, target))
            surface->impl.hovered = target;
    }
    return target && !ui_widget_stale(surface, target) &&
           ui_widget_bubble(target, event);
}

#define UI_CAPTURE_PATH_MAX 64

struct ui_widget *ui_surface_key_target(const struct ui_surface *surface)
{
    if (!surface)
        return NULL;
    if (surface->impl.focused)
        return surface->impl.focused;
    if (surface->impl.modal)
        return surface->impl.modal;
    struct ui_widget *fallback = surface->impl.default_focus;
    if (fallback && !ui_widget_stale(surface, fallback))
        return fallback;
    return surface->impl.root;
}

uint8_t ui_surface_command_scope(const struct ui_surface *surface)
{
    for (struct ui_widget *w = ui_surface_key_target(surface); w; w = w->parent)
        if (w->command_scope != INPUT_COMMAND_SCOPE_APP &&
            w->command_scope < INPUT_COMMAND_SCOPE_COUNT)
            return w->command_scope;
    return INPUT_COMMAND_SCOPE_APP;
}

static bool capture_keyboard(struct ui_surface *surface,
                             struct ui_widget *target,
                             const struct ui_event *event)
{
    /* The path stops at the modal, or else at the top of the target's tree:
     * the root, or the overlay that holds it. */
    struct ui_widget *start = target;
    if (surface->impl.modal && ui_widget_is_ancestor(surface->impl.modal, target))
        start = surface->impl.modal;
    else
        while (start && start->parent)
            start = start->parent;
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
    struct ui_widget *target = ui_surface_key_target(surface);
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
    case UI_EVENT_COMPOSITION:
    case UI_EVENT_TEXT_EDIT:
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
        case UI_EVENT_COMPOSITION:
        case UI_EVENT_TEXT_EDIT:
            handled = dispatch_keyboard(surface, event);
            break;
        case UI_EVENT_PASTE:
            if (surface->impl.paste_target) {
                struct ui_widget *target = surface->impl.paste_target;
                surface->impl.paste_target = NULL;
                handled = !ui_widget_stale(surface, target) && ui_widget_bubble(target, event);
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
    if (handled && !keyboard_path(event->kind))
        ui_surface_request_frame(surface);
    flush_posts(surface);
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
    if (widget && ui_widget_stale(surface, widget))
        widget = NULL;
    if (surface->impl.focused == widget)
        return;
    ui_surface_request_frame(surface);
    struct ui_widget *previous = surface->impl.focused;
    surface->impl.focused = NULL;
    send(previous, ui_event_focus(false, 0));
    if (surface->impl.focused)
        return;
    if (widget && ui_widget_stale(surface, widget))
        widget = NULL;
    surface->impl.focused = widget;
    send(widget, ui_event_focus(true, 0));
    if (widget && surface->impl.focused == widget && ui_widget_stale(surface, widget))
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

static uint64_t sooner(uint64_t a, uint64_t b)
{
    if (!a)
        return b;
    if (!b)
        return a;
    return a < b ? a : b;
}

bool ui_surface_tick(struct ui_surface *surface, uint64_t now_ms)
{
    if (!surface || surface->impl.busy)
        return false;
    surface->impl.busy = true;
    surface->impl.now_ms = now_ms;
    uint64_t deadline = surface->impl.root
        ? ui_widget_tick(surface->impl.root, now_ms) : 0;
    /* Downwards: a tick can close an overlay and shift the list. */
    for (size_t i = surface->impl.overlay_count; i > 0; i--) {
        struct ui_widget *overlay = surface->impl.overlays[i - 1];
        deadline = sooner(deadline, ui_widget_tick(overlay, now_ms));
    }
    if (surface->impl.anim_deadline &&
        (!deadline || surface->impl.anim_deadline < deadline))
        deadline = surface->impl.anim_deadline;
    surface->impl.deadline = deadline;
    surface->impl.busy = false;
    bool next_frame = surface->impl.deadline &&
                      surface->impl.deadline <= now_ms;
    if (next_frame)
        ui_surface_request_frame(surface);
    flush_posts(surface);
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
    if (!surface) {
        ui_scene_init(scene);
        return true;
    }
    surface->impl.busy = true;
    prepare(surface);
    /* The root first, then each overlay, so that the overlays are on top. */
    ui_scene_init(scene);
    bool valid = true;
    if (surface->impl.root)
        valid = ui_widget_append_scene(surface->impl.root, scene);
    for (size_t i = 0; valid && i < surface->impl.overlay_count; i++)
        valid = ui_widget_append_scene(surface->impl.overlays[i], scene);
    surface->impl.busy = false;
    flush_posts(surface);
    return valid && ui_scene_is_valid(scene);
}

bool ui_surface_busy(const struct ui_surface *surface)
{
    return surface && surface->impl.busy;
}

bool ui_surface_destroy(struct ui_surface *surface)
{
    if (!surface)
        return false;
    /* A busy surface still reads its tree, so the owner posts its free. */
    assert(!surface->impl.busy);
    if (surface->impl.busy)
        return false;
    while (surface->impl.overlay_count)
        ui_surface_remove_overlay(surface,
            surface->impl.overlays[surface->impl.overlay_count - 1]);
    while (surface->impl.root)
        ui_widget_detach(surface->impl.root);
    /* A queued free still runs, so that no owner leaks its storage. */
    while (surface->impl.post_count) {
        struct ui_surface_post post = surface->impl.post[0];
        surface->impl.post_count--;
        memmove(&surface->impl.post[0], &surface->impl.post[1],
                surface->impl.post_count * sizeof(surface->impl.post[0]));
        post.fn(surface, post.userdata);
    }
    free(surface->impl.post);
    free(surface->impl.overlays);
    free(surface->impl.focus_stack);
    uint64_t generation = surface->impl.generation;
    ui_style_context_destroy(&surface->impl.style);
    *surface = (struct ui_surface){0};
    surface->impl.generation = generation + 1;
    return true;
}

void ui_shutdown(void)
{
    css_shutdown();
    ui_part_shutdown();
    ui_widget_shutdown();
}
