#define _POSIX_C_SOURCE 200809L
#include "common/backend.h"
#include "common/input.h"
#include "common/timing.h"

#include <assert.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_timing_clock(void)
{
    struct yasl_private_frame_clock clock = {.refresh_ns = YASL_PRIVATE_REFRESH_DEFAULT_NS};
    yasl_private_clock_presented(&clock, 1000000000, true);
    assert(clock.last_present_ns == 1000000000);
    uint64_t next = yasl_private_clock_next_present_ns(&clock);
    assert(next > 1000000000);
    assert(yasl_private_clock_extend_ms(1000, 1000000000) == 1000000000);
    assert(yasl_private_clock_frame_timestamp_ns(&clock, next) == next);

    yasl_private_clock_note_resize(&clock, 500000);
    struct yasl_private_resize_stats stats;
    yasl_private_clock_resize_stats(&clock, &stats);
    assert(stats.count == 1);
    assert(stats.max_us == 500.0);
}

static void test_surface_state(void)
{
    /* Reading reported flags does not require a backend query or display. */
    struct yasl_surface surface = {.state = YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_ACTIVATED};
    assert(!yasl_private_surface_state(NULL));
    assert(yasl_private_surface_state(&surface) == (YASL_PRIVATE_STATE_FULLSCREEN | YASL_PRIVATE_STATE_ACTIVATED));
}

static void test_queue(void)
{
    struct input_queue q = {0};
    struct yasl_surface surface = {0};
    const enum yasl_private_event_type kinds[] = {
        YASL_PRIVATE_EVENT_TEXT, YASL_PRIVATE_EVENT_PASTE, YASL_PRIVATE_EVENT_DROP,
    };
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
        char *source = malloc(4);
        assert(source);
        memcpy(source, "a\0b", 4);
        struct yasl_private_event in = {.surface = &surface, .type = kinds[k], .text = {.data = source, .len = 3}};
        assert(input_queue_push(&q, &in));
        memset(source, 'x', 4);
        free(source);
        struct yasl_private_event out;
        assert(input_queue_pop(&q, &out));
        const char *payload = out.text.data;
        assert(out.surface == &surface && out.type == kinds[k]);
        assert(memcmp(payload, "a\0b", 4) == 0);
        assert(!input_queue_pop(&q, &out));
        assert(memcmp(payload, "a\0b", 4) == 0);
        /* Growth/wraparound must not invalidate a previously polled payload. */
        for (unsigned i = 0; i < 4096; i++) {
            in = (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_KEY, .key.key = i};
            assert(input_queue_push(&q, &in));
        }
        assert(memcmp(payload, "a\0b", 4) == 0);
        for (unsigned i = 0; i < 2048; i++) {
            assert(input_queue_pop(&q, &out));
            assert(out.key.key == i);
        }
        for (unsigned i = 4096; i < 6144; i++) {
            in.key.key = i;
            assert(input_queue_push(&q, &in));
        }
        for (unsigned i = 2048; i < 6144; i++) {
            assert(input_queue_pop(&q, &out));
            assert(out.key.key == i);
        }
        assert(!input_queue_pop(&q, &out));
    }
    struct yasl_private_event empty = {.surface = &surface, .type = YASL_PRIVATE_EVENT_PASTE};
    struct yasl_private_event out;
    assert(input_queue_push(&q, &empty));
    assert(input_queue_pop(&q, &out));
    assert(out.text.data && !out.text.len && out.text.data[0] == '\0');
    struct yasl_private_event in = {.surface = &surface, .type = YASL_PRIVATE_EVENT_TEXT,
                                 .text = {"owned", 5}};
    assert(input_queue_push(&q, &in));
    assert(input_queue_pop(&q, &out));
    const char *last = out.text.data;
    assert(input_queue_push(&q, &in));
    /* Exercise the allocation-failure reset path without allocating huge RAM. */
    in.text.len = SIZE_MAX;
    assert(!input_queue_push(&q, &in));
    assert(strcmp(last, "owned") == 0);
    in = (struct yasl_private_event){.surface = &surface, .type = YASL_PRIVATE_EVENT_KEY};
    assert(input_queue_push(&q, &in));
    assert(input_queue_pop(&q, &out) && out.type == YASL_PRIVATE_EVENT_INPUT_RESET);
    assert(out.surface == &surface);
    assert(input_queue_pop(&q, &out) && out.type == YASL_PRIVATE_EVENT_KEY);
    assert(!input_queue_pop(&q, &out));
    in = (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_PASTE, .text = {"pending", 7}};
    assert(input_queue_push(&q, &in));
    input_queue_clear(&q);
    input_queue_clear(&q);
    assert(!q.events && !q.last_text && !q.count && !q.reset_pending);
}

static int presents, copies, pastes, creates;
static struct yasl_private_damage_list received;
static bool present(struct yasl_surface *s, const struct yasl_private_framebuffer *fb,
                    const struct yasl_private_damage_list *damage)
{
    (void)s; (void)fb;
    presents++;
    received = *damage;
    return true;
}
static uint32_t caps(const struct yasl_private_display *d) { (void)d; return 0; }
static bool copy(struct yasl_private_display *d, enum yasl_private_selection sel, const char *text)
{ (void)d; (void)sel; (void)text; copies++; return true; }
static bool paste(struct yasl_private_display *d, enum yasl_private_selection sel, struct yasl_surface *s, uint64_t id)
{ (void)d; (void)sel; (void)s; assert(id); pastes++; return true; }
static struct yasl_surface *create(struct yasl_private_display *d, const struct yasl_private_window_spec *spec, void *graphics_device)
{ (void)d; (void)spec; (void)graphics_device; creates++; return NULL; }

static void *native_device;
static int native_queries;
static struct yasl_surface native_surface;
static uint32_t native_caps(const struct yasl_private_display *d)
{ (void)d; return YASL_PRIVATE_CAP_NATIVE_GRAPHICS; }
static struct yasl_surface *native_create(struct yasl_private_display *d,
        const struct yasl_private_window_spec *spec, void *device)
{
    (void)spec;
    native_device = device;
    native_surface = (struct yasl_surface){.display = d, .frame_generation = 1};
    return &native_surface;
}
static bool native_frame(struct yasl_surface *s, const struct yasl_private_framebuffer *fb,
                         struct yasl_private_native_frame *out)
{
    (void)s; (void)fb;
    native_queries++;
    *out = (struct yasl_private_native_frame){.drawable = native_device};
    return true;
}

static uint32_t push_buttons(struct input_queue *q, enum yasl_private_event_type type,
                             enum yasl_private_pointer_button button)
{
    struct yasl_private_event ev = {.type = type, .pointer.button = button};
    assert(input_queue_push(q, &ev));
    assert(input_queue_pop(q, &ev));
    return ev.pointer.buttons;
}

static void test_held_buttons(void)
{
    const uint32_t primary = YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_PRIMARY);
    const uint32_t secondary = YASL_PRIVATE_BUTTON_BIT(YASL_PRIVATE_BUTTON_SECONDARY);
    struct input_queue q = {0};
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_MOTION, 0) == 0);
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_DOWN, YASL_PRIVATE_BUTTON_PRIMARY) == primary);
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_MOTION, 0) == primary);
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_DOWN, YASL_PRIVATE_BUTTON_SECONDARY) == (primary | secondary));
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_UP, YASL_PRIVATE_BUTTON_PRIMARY) == secondary);
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_SCROLL, 0) == secondary);
    /* X11 and Cocoa keep the press across leave; the release still follows. */
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_LEAVE, 0) == secondary);
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_UP, YASL_PRIVATE_BUTTON_SECONDARY) == 0);
    /* A release without a press leaves nothing held. */
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_UP, YASL_PRIVATE_BUTTON_MIDDLE) == 0);

    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_DOWN, YASL_PRIVATE_BUTTON_PRIMARY) == primary);
    struct yasl_private_event reset = {.type = YASL_PRIVATE_EVENT_INPUT_RESET};
    assert(input_queue_push(&q, &reset));
    assert(input_queue_pop(&q, &reset));
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_MOTION, 0) == 0);

    q.leave_ends_press = true;
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_DOWN, YASL_PRIVATE_BUTTON_PRIMARY) == primary);
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_LEAVE, 0) == 0);
    assert(push_buttons(&q, YASL_PRIVATE_EVENT_POINTER_MOTION, 0) == 0);
    input_queue_clear(&q);
}

static void test_drop_paths(void)
{
    char host[256] = "";
    assert(gethostname(host, sizeof(host)) == 0);
    char list[1024];
    snprintf(list, sizeof(list),
        "# comment\r\n"
        "\r\n"
        "file:///home/user/a%%20b.txt\r\n"
        "https://example.com/x\r\n"
        "file://elsewhere.invalid/remote\r\n"
        "file://localhost/tmp/%%C3%%A9t%%C3%%A9  \r\n"
        "file://%s/srv/x\r\n"
        "FILE:/single/slash\n"
        "file:///bad%%2\r\n"
        "file:///nul%%00byte\r\n"
        "file:///q?query#frag\r\n"
        "file:relative\r\n"
        "file:///last",
        host);
    static const char *const want[] = {
        "/home/user/a b.txt", "/tmp/\xc3\xa9t\xc3\xa9", "/srv/x", "/single/slash",
        "/q", "/last",
    };
    const char *cursor = list;
    char path[64];
    for (size_t i = 0; i < sizeof(want) / sizeof(want[0]); i++) {
        size_t len = strlen(want[i]);
        /* A short buffer reports the length and leaves both outputs alone. */
        const char *before = cursor;
        memcpy(path, "keep", 5);
        assert(yasl_private_drop_next_path(&cursor, path, len) == len);
        assert(cursor == before && strcmp(path, "keep") == 0);
        assert(yasl_private_drop_next_path(&cursor, NULL, 0) == len && cursor == before);
        assert(yasl_private_drop_next_path(&cursor, path, sizeof(path)) == len);
        assert(strcmp(path, want[i]) == 0);
    }
    assert(yasl_private_drop_next_path(&cursor, path, sizeof(path)) == 0 && *cursor == '\0');
    assert(yasl_private_drop_next_path(&cursor, path, sizeof(path)) == 0);
    assert(yasl_private_drop_next_path(NULL, path, sizeof(path)) == 0);
    const char *none = NULL;
    assert(yasl_private_drop_next_path(&none, path, sizeof(path)) == 0);
    cursor = list;
    assert(yasl_private_drop_next_path(&cursor, NULL, 8) == 0 && cursor == list);
}

static void test_native(void)
{
    const struct yasl_private_backend_ops ops = {.display_caps = native_caps,
        .surface_create_toplevel = native_create, .surface_get_native_frame = native_frame};
    struct yasl_private_display d = {.ops = &ops};
    struct yasl_private_window_spec spec = {.size = {100, 80}};
    assert(yasl_private_surface_create_toplevel(&d, &spec) == &native_surface);
    assert(!native_device);
    assert(yasl_private_surface_create_native(&d, &spec, &d) == &native_surface);
    assert(native_device == &d);

    struct yasl_private_framebuffer fb = {.generation = 1, .size = {100, 80}};
    struct yasl_private_native_frame out = {0};
    assert(!yasl_private_surface_get_native_frame(NULL, &fb, &out));
    assert(!yasl_private_surface_get_native_frame(&native_surface, NULL, &out));
    assert(!yasl_private_surface_get_native_frame(&native_surface, &fb, NULL));
    fb.generation = 0;
    assert(!yasl_private_surface_get_native_frame(&native_surface, &fb, &out));
    fb.generation = 2;
    assert(!yasl_private_surface_get_native_frame(&native_surface, &fb, &out));
    assert(!native_queries && !out.drawable);
    fb.generation = 1;
    assert(yasl_private_surface_get_native_frame(&native_surface, &fb, &out));
    assert(native_queries == 1 && out.drawable == &d);
}

static void test_validation(void)
{
    const struct yasl_private_backend_ops ops = {.surface_present = present,
        .display_caps = caps, .display_copy_text = copy, .display_request_paste = paste,
        .surface_create_toplevel = create};
    struct yasl_private_display d = {.ops = &ops};
    struct yasl_surface s = {.display = &d, .frame_generation = 1};
    struct yasl_private_framebuffer fb = {.generation = 1, .size = {100, 80}};
    struct yasl_private_damage_list damage = {.n = -2};
    assert(!yasl_private_surface_present(&s, &fb, &damage));
    damage.n = YASL_PRIVATE_DAMAGE_MAX + 1;
    assert(!yasl_private_surface_present(&s, &fb, &damage));
    damage.n = 1;
    damage.rects[0] = (struct yasl_private_rect2i){0, 0, -1, 1};
    assert(!yasl_private_surface_present(&s, &fb, &damage));
    assert(!yasl_private_surface_present(&s, &fb, NULL));
    assert(!presents);
    damage.n = -1;
    fb.generation = 0;
    assert(!yasl_private_surface_present(&s, &fb, &damage));
    fb.generation = 2;
    assert(!yasl_private_surface_present(&s, &fb, &damage));
    fb.generation = 1;
    damage.n = 4;
    damage.rects[0] = (struct yasl_private_rect2i){-10, -10, 30, 30};
    damage.rects[1] = (struct yasl_private_rect2i){90, 70, INT_MAX, INT_MAX};
    damage.rects[2] = (struct yasl_private_rect2i){INT_MAX, INT_MAX, INT_MAX, INT_MAX};
    damage.rects[3] = (struct yasl_private_rect2i){INT_MIN, INT_MIN, INT_MAX, INT_MAX};
    assert(yasl_private_surface_present(&s, &fb, &damage));
    assert(received.n == 2 && received.rects[0].x == 0 && received.rects[0].width == 20);
    assert(received.rects[1].width == 10 && received.rects[1].height == 10);
    assert(damage.n == 4 && damage.rects[0].x == -10);
    damage.n = -1;
    assert(yasl_private_surface_present(&s, &fb, &damage) && received.n == -1);
    damage.n = 0;
    assert(yasl_private_surface_present(&s, &fb, &damage) && received.n == 0);
    assert(yasl_private_events_open(&d));
    yasl_private_clipboard_set(&d, YASL_PRIVATE_PRIMARY, "no alias");
    yasl_private_clipboard_set(&d, (enum yasl_private_selection)255, "invalid");
    yasl_private_clipboard_request(&s, YASL_PRIVATE_PRIMARY);
    yasl_private_clipboard_request(&s, (enum yasl_private_selection)255);
    assert(!copies && !pastes);
    yasl_private_clipboard_set(&d, YASL_PRIVATE_CLIPBOARD, "yes");
    yasl_private_clipboard_request(&s, YASL_PRIVATE_CLIPBOARD);
    assert(copies == 1 && pastes == 1);
    yasl_private_events_close(&d);
    struct yasl_private_window_spec spec = {.size = {100, 80}};
    assert(!yasl_private_surface_create_native(&d, &spec, &d) && errno == ENOTSUP);
    assert(!yasl_private_surface_create_native(&d, &spec, NULL) && errno == EINVAL);
    spec.size.x = 0;
    assert(!yasl_private_surface_create_toplevel(&d, &spec) && errno == EINVAL);
    assert(!creates);
}

static void test_scale(void)
{
    assert(yasl_private_scale_to_device(-10, 120) == -10);
    assert(yasl_private_scale_to_device(-0.5, 120) == -1);
    assert(yasl_private_scale_to_device(0.5, 120) == 1);
    assert(yasl_private_scale_to_device(NAN, 120) == 0);
    assert(yasl_private_scale_to_device(INFINITY, 120) == 0);
    assert(yasl_private_scale_to_device(DBL_MAX, 240) == INT_MAX);
    assert(yasl_private_scale_to_logical(-1, 240) == -1);
    assert(yasl_private_scale_to_logical(1, 240) == 1);
    assert(yasl_private_scale_to_logical(INT_MAX, 1) == INT_MAX);
    assert(yasl_private_scale_to_logical(INT_MIN, 1) == INT_MIN);
    assert(yasl_private_scale_to_logical(1, 0) == 0);
}

static int popup_calls;
static struct yasl_surface *popup_create(struct yasl_surface *parent,
        const struct yasl_private_popup_spec *spec)
{
    (void)parent; (void)spec;
    popup_calls++;
    return NULL; /* Backend fails without setting errno. */
}

static bool popup_reposition(struct yasl_surface *popup,
        const struct yasl_private_popup_spec *spec)
{
    (void)popup; (void)spec;
    popup_calls++;
    errno = EBUSY;
    return false;
}

static uint32_t popup_caps(const struct yasl_private_display *d)
{ (void)d; return YASL_PRIVATE_CAP_POPUPS; }

static struct yasl_surface *create_failure(struct yasl_private_display *d,
        const struct yasl_private_window_spec *spec, void *device)
{ (void)d; (void)spec; (void)device; return NULL; }

static int drag_starts;
static bool drag_without_listener(struct yasl_private_display *d, struct yasl_surface *s,
        const struct yasl_private_drag_image *image, const struct yasl_private_drag_listener *listener,
        void *ud)
{
    (void)d; (void)s; (void)image; (void)ud;
    /* Backends may require a table even when no completion callback is needed. */
    assert(listener && !listener->end);
    drag_starts++;
    return true;
}

static void test_errors(void)
{
    const struct yasl_private_backend_ops ops = {.display_caps = popup_caps,
        .surface_create_toplevel = create_failure,
        .surface_create_popup = popup_create,
        .surface_popup_reposition = popup_reposition,
        .display_drag_start = drag_without_listener};
    struct yasl_private_display d = {.ops = &ops};
    struct yasl_surface s = {.display = &d};
    struct yasl_private_popup_spec spec = {.size = {120, 80}, .anchor = {0, 0, 1, 1}};
    assert(!yasl_private_surface_create_popup(NULL, &spec) && errno == EINVAL);
    assert(!yasl_private_surface_create_popup(&s, NULL) && errno == EINVAL);
    spec.anchor.width = -1; /* Must not reach Wayland's protocol marshaler. */
    assert(!yasl_private_surface_create_popup(&s, &spec) && errno == EINVAL);
    assert(!yasl_private_surface_popup_reposition(&s, &spec) && errno == EINVAL);
    spec.anchor.width = 1;
    spec.content_offset.x = INT_MAX;
    spec.content_size.x = INT_MAX;
    assert(!yasl_private_surface_create_popup(&s, &spec) && errno == EINVAL);
    spec.content_offset.x = spec.content_size.x = 0;
    spec.size.x = 32768;
    assert(!yasl_private_surface_create_popup(&s, &spec) && errno == EINVAL);
    assert(!popup_calls);
    spec.size.x = 120;
    errno = ERANGE;
    assert(!yasl_private_surface_create_popup(&s, &spec) && errno == ENODEV);
    assert(!yasl_private_surface_popup_reposition(&s, &spec) && errno == EBUSY);
    assert(popup_calls == 2);
    const struct yasl_private_backend_ops unsupported = {0};
    d.ops = &unsupported;
    assert(!yasl_private_surface_create_popup(&s, &spec) && errno == ENOTSUP);
    assert(!yasl_private_surface_popup_reposition(&s, &spec) && errno == ENOTSUP);
    d.ops = &ops;
    struct yasl_private_window_spec toplevel = {.size = {120, 80}};
    errno = ERANGE;
    assert(!yasl_private_surface_create_toplevel(&d, &toplevel) && errno == ENODEV);
    assert(yasl_private_display_drag_start(&d, &s, NULL, NULL, NULL));
    struct yasl_private_drag_image image = {.size = {1, 1}};
    assert(!yasl_private_display_drag_start(&d, &s, &image, NULL, NULL));
    uint32_t pixel = 0;
    image.pixels = &pixel;
    image.size.x = INT_MAX;
    assert(!yasl_private_display_drag_start(&d, &s, &image, NULL, NULL));
    assert(drag_starts == 1);
}

int main(void)
{
    test_timing_clock();
    test_surface_state();
    test_queue();
    test_held_buttons();
    test_drop_paths();
    test_validation();
    test_native();
    test_scale();
    test_errors();
    return 0;
}
