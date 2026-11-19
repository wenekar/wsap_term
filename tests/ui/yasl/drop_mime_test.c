#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "ui/event.h"
#include "ui/yasl/drop.h"

/* A source that holds a text payload and refuses every other type. */
struct text_source {
    const char *text;
    size_t len;
};

static bool read_text(void *userdata, const char *mime, const char **data,
                      size_t *len)
{
    struct text_source *src = userdata;
    if (strcmp(mime, "text/plain;charset=utf-8") != 0)
        return false;
    *data = src->text;
    *len = src->len;
    return true;
}

/* The target asks for a type that the source refuses. The read fails, so the
 * target takes no drop. */
static void refused_type_test(void)
{
    static const char text[] = "hello";
    struct text_source src = { .text = text, .len = sizeof text - 1 };
    static const char *const offers[] = { "text/plain;charset=utf-8" };
    struct ui_drag_event drag = {
        .mimes = offers,
        .mime_count = 1,
        .source = &(struct ui_drop_source){ .read = read_text, .userdata = &src },
    };
    const char *data = NULL;
    size_t len = 0;
    assert(!ui_drag_event_read(&drag, "text/uri-list", &data, &len));
    assert(!data && !len);
    assert(ui_drag_event_read(&drag, "text/plain;charset=utf-8", &data, &len));
    assert(len == sizeof text - 1 && memcmp(data, text, len) == 0);
    /* A drag from outside the app has no source, so it answers nothing. */
    drag.source = NULL;
    assert(!ui_drag_event_read(&drag, "text/plain;charset=utf-8", &data, &len));
}

/* The DROP event carries the position in surface coordinates, the offered
 * types, and the type that the target asked for. */
static void drop_event_payload_test(void)
{
    static const char *const offers[] = { "text/uri-list", "text/plain" };
    struct ui_drag_event drag = {
        .pos = { .x = 12, .y = 34 },
        .mimes = offers,
        .mime_count = 2,
        .mime = "text/uri-list",
    };
    struct ui_event event = ui_event_drag(UI_EVENT_DROP, &drag, 7);
    assert(event.kind == UI_EVENT_DROP);
    assert(event.time_ms == 7);
    assert(event.data.drag.pos.x == 12 && event.data.drag.pos.y == 34);
    assert(event.data.drag.mime_count == 2);
    assert(!strcmp(event.data.drag.mimes[0], "text/uri-list"));
    assert(!strcmp(event.data.drag.mime, "text/uri-list"));
    assert(ui_drag_offers(&event.data.drag, "text/plain"));
    assert(!ui_drag_offers(&event.data.drag, "image/png"));

    struct ui_event enter = ui_event_drag(UI_EVENT_DRAG_ENTER, &drag, 8);
    assert(enter.kind == UI_EVENT_DRAG_ENTER);
    struct ui_event over = ui_event_drag(UI_EVENT_DRAG_OVER, &drag, 9);
    assert(over.kind == UI_EVENT_DRAG_OVER);
    struct ui_event leave = ui_event_drag(UI_EVENT_DRAG_LEAVE, &drag, 10);
    assert(leave.kind == UI_EVENT_DRAG_LEAVE);
}

/* The first acceptable type wins, and a star in an offer gives its major
 * type. */
static void pick_mime_test(void)
{
    static const char *const accepted[] = { "text/uri-list", "text/plain",
                                            "image/png" };
    static const char *const offers_uri[] = { "text/plain", "text/uri-list" };
    assert(!strcmp(drop_pick_mime(offers_uri, 2, accepted, 3), "text/uri-list"));
    static const char *const offers_text[] = { "text/plain;charset=utf-8" };
    assert(drop_pick_mime(offers_text, 1, accepted, 3) == NULL);
    static const char *const offers_image[] = { "image/*" };
    assert(!strcmp(drop_pick_mime(offers_image, 1, accepted, 3), "image/png"));
    static const char *const offers_none[] = { "application/json" };
    assert(drop_pick_mime(offers_none, 1, accepted, 3) == NULL);
}

/* A major type with a star in an offer matches its subtypes. */
static void drag_offers_star_test(void)
{
    static const char *const offers[] = { "image/*" };
    struct ui_drag_event drag = { .mimes = offers, .mime_count = 1 };
    assert(ui_drag_offers(&drag, "image/png"));
    assert(ui_drag_offers(&drag, "image/svg+xml"));
    assert(!ui_drag_offers(&drag, "image/"));
    assert(!ui_drag_offers(&drag, "image"));
    assert(!ui_drag_offers(&drag, "text/plain"));

    /* A star alone takes every type. */
    static const char *const any[] = { "*" };
    drag.mimes = any;
    assert(ui_drag_offers(&drag, "text/plain"));

    /* An exact offer matches its type only. */
    static const char *const exact[] = { "text/plain" };
    drag.mimes = exact;
    assert(ui_drag_offers(&drag, "text/plain"));
    assert(!ui_drag_offers(&drag, "text/uri-list"));
}

/* A drag that moves within one widget sends no leave. A drag that moves to
 * another widget, or off the surface, sends exactly one. */
static void leave_once_test(void)
{
    struct drop_hover hover = {0};
    int a = 1, b = 2;
    assert(!drop_hover_retarget(&hover, &a));
    assert(!drop_hover_retarget(&hover, &a));
    assert(hover.active && hover.target == &a);
    assert(drop_hover_retarget(&hover, &b));
    assert(hover.active && hover.target == &b);
    assert(drop_hover_retarget(&hover, NULL));
    assert(!hover.active);
    /* The surface is already left, so a second leave sends nothing. */
    assert(!drop_hover_retarget(&hover, NULL));
}

/* A drop no longer becomes a file. */
static void no_file_drop_test(void)
{
    static const char png[] = { 'P', 'N', 'G', 0, 1 };
    assert(drop_save_image("image/png", png, sizeof png) == NULL);
    drop_cleanup();
}

/* The star offers match any major type, the parameters do not count, and the
 * letters match without case. A foreign drag from a file manager offers
 * "*" "*", and a text editor offers "text/plain". */
static void drag_offers_star_star_test(void)
{
    static const char *const any[] = { "*/*" };
    struct ui_drag_event drag = { .mimes = any, .mime_count = 1 };
    assert(ui_drag_offers(&drag, "text/plain"));
    assert(ui_drag_offers(&drag, "image/png"));
    assert(ui_drag_offers(&drag, "text/plain;charset=utf-8"));

    /* A major type with a star matches its subtypes, whatever the case. */
    static const char *const images[] = { "IMAGE/*" };
    drag.mimes = images;
    assert(ui_drag_offers(&drag, "image/png"));
    assert(ui_drag_offers(&drag, "image/svg+xml"));
    assert(!ui_drag_offers(&drag, "text/plain"));

    /* An exact offer matches a request that carries parameters, and it
     * ignores its own parameters. */
    static const char *const text[] = { "text/plain" };
    drag.mimes = text;
    assert(ui_drag_offers(&drag, "text/plain;charset=utf-8"));
    assert(ui_drag_offers(&drag, "TEXT/PLAIN"));
    assert(!ui_drag_offers(&drag, "text/html"));
    static const char *const text_params[] = { "text/plain; charset=utf-8" };
    drag.mimes = text_params;
    assert(ui_drag_offers(&drag, "text/plain"));
    assert(ui_drag_offers(&drag, "Text/Plain"));
}

/* A list of the accepted types answers for each one. */
static void drag_offers_foreign_types_test(void)
{
    static const char *const offers[] = { "*/*" };
    static const char *const accepted[] = { "text/uri-list", "image/png",
                                            "text/plain;charset=utf-8" };
    struct ui_drag_event drag = { .mimes = offers, .mime_count = 1 };
    for (size_t i = 0; i < sizeof accepted / sizeof accepted[0]; i++)
        assert(ui_drag_offers(&drag, accepted[i]));
}

int main(void)
{
    refused_type_test();
    drop_event_payload_test();
    pick_mime_test();
    drag_offers_star_test();
    drag_offers_star_star_test();
    drag_offers_foreign_types_test();
    leave_once_test();
    no_file_drop_test();
    return 0;
}
