#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "input/input.h"
#include "ui/widgets/menu.h"
#include "ui/yasl/client.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "ui/yasl/testing.h"
#include "ui/yasl/window.h"

/*
 * Decision of the codebase owner (2026-11-19):
 * - A paste always goes to the focused widget.
 * - Only a drop release goes to the drop target.
 * - The drop location on the window surface gives the target of a release.
 *   Hover state from an earlier drag does not give it.
 * - Drop hover state clears on leave, drop, cancel, and widget or surface
 *   teardown.
 *
 * The cases feed native events through window_native_input(), so the order of
 * a real backend is part of the test. Wayland and X11 send LEAVE before the
 * release. Cocoa sends it after. A source that dies sends no LEAVE.
 */

struct probe {
    struct ui_widget widget;
    int enter;
    int leave;
    int drop;
    int paste;
};

static struct window_host host;

static bool probe_event(struct ui_widget *widget, const struct ui_event *event);

static const struct ui_widget_ops PROBE_OPS = {
    .name = "drop-route-regress-probe",
    .event = probe_event,
};

static bool probe_event(struct ui_widget *widget, const struct ui_event *event)
{
    struct probe *p = widget->userdata;
    if (event->kind == UI_EVENT_DRAG_ENTER || event->kind == UI_EVENT_DRAG_OVER)
        p->enter++;
    if (event->kind == UI_EVENT_DRAG_LEAVE)
        p->leave++;
    if (event->kind == UI_EVENT_DROP || event->kind == UI_EVENT_DROP_FILES)
        p->drop++;
    if (event->kind == UI_EVENT_PASTE)
        p->paste++;
    return true;
}

/* The top probe owns y 0..99 and the bottom probe y 100..199. The bottom
 * probe is on the heap, so a case can free it during a drag. */
struct fixture {
    struct ui_widget root;
    struct probe top;
    struct probe *bottom;
    struct window *window;
};

static const struct vector2i over_top = { .x = 40, .y = 20 };
static const struct vector2i over_bottom = { .x = 40, .y = 150 };

static void show(struct fixture *f)
{
    *f = (struct fixture){0};
    /* The host borrows the config, so it must outlive the host. */
    static const struct window_config config = { .size = { .x = 320, .y = 200 } };
    window_host_init(&host, NULL, &config, NULL);
    f->window = window_create(&host, &(struct window_spec){0});
    assert(f->window);
    f->bottom = calloc(1, sizeof *f->bottom);
    assert(f->bottom);
    ui_widget_init(&f->root, NULL, NULL);
    ui_widget_init(&f->top.widget, &PROBE_OPS, &f->top);
    ui_widget_init(&f->bottom->widget, &PROBE_OPS, f->bottom);
    ui_widget_set_fixed_size(&f->top.widget,
                             (struct vector2i){ .x = 320, .y = 100 });
    ui_widget_set_fixed_size(&f->bottom->widget,
                             (struct vector2i){ .x = 320, .y = 100 });
    ui_widget_set_placement(&f->top.widget, (struct ui_placement){
        .kind = UI_PLACE_AT,
    });
    ui_widget_set_placement(&f->bottom->widget, (struct ui_placement){
        .kind = UI_PLACE_AT, .anchor.pos = { .y = 100 },
    });
    ui_widget_set_focusable(&f->top.widget, true);
    ui_widget_set_focusable(&f->bottom->widget, true);
    assert(ui_widget_add_child(&f->root, &f->top.widget));
    assert(ui_widget_add_child(&f->root, &f->bottom->widget));
    assert(window_ui_mount(f->window, &(struct window_ui_spec){
        .root = &f->root }));
    /* A window with no native surface gets no relayout, so the test sizes the
     * screen itself. */
    ui_surface_resize(window_ui_key_surface(f->window),
                      (struct vector2i){ .x = 320, .y = 200 });
}

static void free_bottom(struct fixture *f)
{
    if (!f->bottom)
        return;
    ui_widget_destroy(&f->bottom->widget);
    free(f->bottom);
    f->bottom = NULL;
}

static void teardown(struct fixture *f)
{
    if (f->window)
        window_destroy(f->window);
    window_host_registry_remove(&host);
    /* The host has no yasl loop, so the test frees its window list. */
    free(host.windows);
    host = (struct window_host){0};
    free_bottom(f);
    ui_widget_destroy(&f->root);
}

static void focus(struct fixture *f, struct ui_widget *widget)
{
    ui_surface_focus(window_ui_key_surface(f->window), widget);
}

/* Sends one native event, then routes what it queued, as the host does. */
static void native(struct fixture *f, const struct yasl_event *e)
{
    window_native_input(f->window, e);
    struct input_event ev;
    while (input_queue_take(&f->window->input, &ev)) {
        window_ui_route(f->window, &ev);
        input_event_release(&ev);
    }
}

static void hover(struct fixture *f, const char *mime, struct vector2i pos)
{
    struct yasl_event e = {
        .type = YASL_EVENT_DROP, .action = YASL_DROP_OVER,
        .x = pos.x, .y = pos.y,
    };
    snprintf(e.drop.mime, sizeof e.drop.mime, "%s", mime);
    native(f, &e);
}

static void leave(struct fixture *f, struct vector2i pos)
{
    native(f, &(struct yasl_event){
        .type = YASL_EVENT_DROP, .action = YASL_DROP_LEAVE,
        .x = pos.x, .y = pos.y,
    });
}

static void release(struct fixture *f, const char *mime, const char *data,
                    struct vector2i pos)
{
    struct yasl_event e = {
        .type = YASL_EVENT_DROP, .action = YASL_DROP_RELEASE,
        .x = pos.x, .y = pos.y, .text = data, .length = strlen(data),
    };
    snprintf(e.drop.mime, sizeof e.drop.mime, "%s", mime);
    native(f, &e);
}

static void clipboard(struct fixture *f, const char *text)
{
    native(f, &(struct yasl_event){
        .type = YASL_EVENT_CLIPBOARD, .action = YASL_TRANSFER_OK,
        .text = text, .length = strlen(text),
    });
}

static const char FILE_URI[] = "file:///tmp/a\r\n";

/* 1. A paste while a drag hovers another widget goes to the focus. */
static void paste_during_a_drag_goes_to_the_focus_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "image/png", over_bottom);
    assert(f.bottom->enter == 1);
    clipboard(&f, "y");
    assert(f.top.paste == 1);
    assert(f.bottom->paste == 0);
    teardown(&f);
}

/* 2. A text drop with no LEAVE after it, then a paste after a focus change.
 * The paste goes to the new focus, not to the widget that took the drop. */
static void paste_after_a_text_drop_goes_to_the_focus_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "text/plain", over_top);
    assert(f.top.enter == 1);
    release(&f, "text/plain", "x", over_top);
    assert(f.top.paste == 1);
    focus(&f, &f.bottom->widget);
    clipboard(&f, "y");
    assert(f.bottom->paste == 1);
    assert(f.top.paste == 1);
    teardown(&f);
}

/* 3. A file drop with no LEAVE after it. A later paste goes to the focus. */
static void paste_after_a_file_drop_goes_to_the_focus_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "text/uri-list", over_bottom);
    assert(f.bottom->enter == 1);
    release(&f, "text/uri-list", FILE_URI, over_bottom);
    assert(f.bottom->drop == 1);
    clipboard(&f, "y");
    assert(f.top.paste == 1);
    assert(f.bottom->paste == 0);
    teardown(&f);
}

/* 4. A drop of another type with no LEAVE after it. A later paste goes to
 * the focus. */
static void paste_after_a_binary_drop_goes_to_the_focus_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "image/png", over_bottom);
    assert(f.bottom->enter == 1);
    release(&f, "image/png", "PNG", over_bottom);
    assert(f.bottom->drop == 1);
    clipboard(&f, "y");
    assert(f.top.paste == 1);
    assert(f.bottom->paste == 0);
    teardown(&f);
}

/* 5. Wayland and X11 order: LEAVE, then the release. A file drop goes to the
 * widget at the drop location, not to the focus. */
static void file_drop_after_leave_goes_to_its_location_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "text/uri-list", over_bottom);
    leave(&f, over_bottom);
    release(&f, "text/uri-list", FILE_URI, over_bottom);
    assert(f.bottom->drop == 1);
    assert(f.top.drop == 0);
    teardown(&f);
}

/* 6. Cocoa order: the release, then LEAVE. The file drop goes to its
 * location, and the next paste goes to the focus. */
static void file_drop_before_leave_goes_to_its_location_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "text/uri-list", over_bottom);
    release(&f, "text/uri-list", FILE_URI, over_bottom);
    leave(&f, over_bottom);
    assert(f.bottom->drop == 1);
    assert(f.top.drop == 0);
    clipboard(&f, "y");
    assert(f.top.paste == 1 && f.bottom->paste == 0);
    teardown(&f);
}

/* 7. A drag that hovered one widget and lost its LEAVE. The next drop lands
 * on another widget. The drop location wins over the old hover state. */
static void drop_goes_to_its_location_not_an_old_hover_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "image/png", over_bottom);
    release(&f, "image/png", "PNG", over_top);
    assert(f.top.drop == 1);
    assert(f.bottom->drop == 0);
    teardown(&f);
}

/* 8. The hover widget is freed during a drag (D1). The next OVER, LEAVE, and
 * release must not touch it. ASan reports a use after free. */
static void widget_freed_during_a_drag_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "image/png", over_bottom);
    assert(f.bottom->enter == 1);
    free_bottom(&f);
    hover(&f, "image/png", over_bottom);
    leave(&f, over_bottom);
    hover(&f, "text/uri-list", over_bottom);
    release(&f, "text/uri-list", FILE_URI, over_bottom);
    clipboard(&f, "y");
    assert(f.top.paste == 1);
    teardown(&f);
}

/* 9. The hover widget is freed during a drag that then gets a paste and a
 * release with no OVER between them. */
static void widget_freed_then_release_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "text/uri-list", over_bottom);
    free_bottom(&f);
    clipboard(&f, "y");
    assert(f.top.paste == 1);
    release(&f, "text/uri-list", FILE_URI, over_bottom);
    release(&f, "image/png", "PNG", over_bottom);
    teardown(&f);
}

/* 10. The window surface replaces its tree during a drag. The old tree is
 * freed. The next events must not touch it. */
static void tree_replaced_during_a_drag_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "image/png", over_bottom);
    struct ui_surface *screen = window_ui_surface_at(f.window, 0);
    struct ui_widget other;
    ui_widget_init(&other, NULL, NULL);
    assert(ui_surface_set_root(screen, &other));
    free_bottom(&f);
    leave(&f, over_bottom);
    clipboard(&f, "y");
    release(&f, "image/png", "PNG", over_bottom);
    assert(ui_surface_set_root(screen, NULL));
    ui_widget_destroy(&other);
    teardown(&f);
}

/* 11. The window closes during a drag. Its queued drop events are not
 * routed after the close. */
static void window_closed_during_a_drag_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    hover(&f, "image/png", over_bottom);
    window_destroy(f.window);
    f.window = NULL;
    free_bottom(&f);
    teardown(&f);
}

/* 12. A menu popup is open during a drag, so the key surface is the popup.
 * The hover widget on the window surface is freed. The next events must not
 * touch it. Before 16b1f8d the popup kept the target. */
static void widget_freed_while_a_popup_is_open_test(void)
{
    struct fixture f;
    show(&f);
    focus(&f, &f.top.widget);
    /* The state that open_popup makes: a popup surface above the window. */
    struct window_ui *ui = ui_of(f.window);
    ui->menu_popup = calloc(UI_MENU_MAX_LEVELS, sizeof(*ui->menu_popup));
    assert(ui->menu_popup);
    ui_surface_init(&ui->menu_popup[0].ui, NULL, NULL);
    ui->menu_popups = 1;
    assert(window_ui_key_surface(f.window) == &ui->menu_popup[0].ui);
    hover(&f, "image/png", over_bottom);
    assert(f.bottom->enter == 1);
    free_bottom(&f);
    hover(&f, "image/png", over_bottom);
    leave(&f, over_bottom);
    release(&f, "image/png", "PNG", over_bottom);
    clipboard(&f, "y");
    /* The test owns the fake popup, so it frees it before the window. */
    ui->menu_popups = 0;
    ui_surface_destroy(&ui->menu_popup[0].ui);
    free(ui->menu_popup);
    ui->menu_popup = NULL;
    teardown(&f);
}

int main(void)
{
    static void (*const cases[])(void) = {
        paste_during_a_drag_goes_to_the_focus_test,
        paste_after_a_text_drop_goes_to_the_focus_test,
        paste_after_a_file_drop_goes_to_the_focus_test,
        paste_after_a_binary_drop_goes_to_the_focus_test,
        file_drop_after_leave_goes_to_its_location_test,
        file_drop_before_leave_goes_to_its_location_test,
        drop_goes_to_its_location_not_an_old_hover_test,
        widget_freed_during_a_drag_test,
        widget_freed_then_release_test,
        tree_replaced_during_a_drag_test,
        window_closed_during_a_drag_test,
        widget_freed_while_a_popup_is_open_test,
    };
    /* CASE=n runs one case, so a run reports each failure. */
    const char *pick = getenv("CASE");
    int only = pick ? atoi(pick) : 0;
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (!only || only == (int)i + 1)
            cases[i]();
    return 0;
}
