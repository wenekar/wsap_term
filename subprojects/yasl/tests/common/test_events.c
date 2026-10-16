#include "common/backend.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static struct input_queue *queue(struct yasl_surface *s) { return &s->input; }
static const struct yasl_private_backend_ops ops = {.surface_input_queue = queue};
static int released;
static pthread_t owner;
static void release(void *payload)
{
    assert(pthread_equal(owner, pthread_self()));
    assert(payload == &released);
    released++;
}

static void setup(struct yasl_private_display *d, struct yasl_surface *a, struct yasl_surface *b)
{
    *d = (struct yasl_private_display){.ops = &ops};
    *a = (struct yasl_surface){.display = d, .scale_120 = 120};
    *b = (struct yasl_surface){.display = d, .scale_120 = 120};
    assert(yasl_private_events_open(d));
    assert(yasl_private_events_register(a));
    assert(yasl_private_events_register(b));
}

static void teardown(struct yasl_private_display *d, struct yasl_surface *a, struct yasl_surface *b)
{
    yasl_private_events_unregister(a);
    yasl_private_events_unregister(b);
    input_queue_clear(&a->input);
    input_queue_clear(&b->input);
    yasl_private_events_close(d);
}

static void test_order_and_lifetimes(void)
{
    struct yasl_private_display d;
    struct yasl_surface a, b;
    setup(&d, &a, &b);
    uint64_t aid = yasl_private_surface_id(&a), bid = yasl_private_surface_id(&b);
    assert(aid && bid && aid != bid);
    yasl_private_surface_set_userdata(&a, &b);
    assert(yasl_private_surface_userdata(&a) == &b);
    char text[] = "text";
    assert(input_queue_push(&a.input, &(struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_TEXT, .surface = &a, .text = {text, 4}}));
    memset(text, 'x', 4);
    yasl_private_surface_notify_state(&b, YASL_PRIVATE_STATE_MAXIMIZED);
    yasl_private_surface_notify_configure(&a, (struct yasl_private_geometry){{640, 480}, 180});
    yasl_private_surface_notify_drag(&b, YASL_PRIVATE_DRAG_OVER, (struct yasl_private_vector2i){3, 4});
    yasl_private_surface_notify_composition(&a, "hello", 1, 3);
    yasl_private_surface_notify_composition(&a, NULL, -1, -1);
    yasl_private_surface_complete_paste(&b, 42, YASL_PRIVATE_TRANSFER_UNAVAILABLE, NULL, 0);
    const enum yasl_private_event_type expected[] = {YASL_PRIVATE_EVENT_TEXT, YASL_PRIVATE_EVENT_WINDOW_STATE,
        YASL_PRIVATE_EVENT_WINDOW_RESIZE, YASL_PRIVATE_EVENT_DRAG, YASL_PRIVATE_EVENT_COMPOSITION,
        YASL_PRIVATE_EVENT_COMPOSITION, YASL_PRIVATE_EVENT_PASTE};
    struct yasl_private_event ev;
    uint64_t sequence = 0;
    for (size_t i = 0; i < sizeof(expected) / sizeof(*expected); i++) {
        assert(yasl_private_display_poll_event(&d, &ev));
        assert(ev.type == expected[i] && ev.sequence > sequence && ev.time_ns);
        sequence = ev.sequence;
        if (i == 0) assert(!strcmp(ev.text.data, "text") && ev.surface_id == aid);
        if (i == 2) assert(ev.resize.scale_changed && ev.resize.geometry.size.x == 640);
        if (i == 4) assert(ev.composition.active && ev.composition.begin == 1 && !strcmp(ev.composition.data, "hello"));
        if (i == 5) assert(!ev.composition.active && !ev.composition.len);
        if (i == 6) assert(ev.paste.request_id == 42 && ev.paste.status == YASL_PRIVATE_TRANSFER_UNAVAILABLE);
    }
    assert(!yasl_private_display_poll_event(&d, &ev));
    assert(input_queue_push(&a.input, &(struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_TEXT, .surface = &a, .text = {"kept", 4}}));
    assert(yasl_private_display_poll_event(&d, &ev));
    const char *borrowed = ev.text.data;
    assert(input_queue_push(&a.input, &(struct yasl_private_event){.type = YASL_PRIVATE_EVENT_KEY, .surface = &a}));
    yasl_private_events_unregister(&a);
    assert(!strcmp(borrowed, "kept"));
    assert(!yasl_private_display_poll_event(&d, &ev));
    /* Reusing object memory must never reuse the identity. */
    assert(yasl_private_events_register(&a));
    assert(a.id != aid);
    errno = 0;
    assert(!yasl_private_display_post(&d, &(struct yasl_private_message){.target_id = aid}));
    assert(errno == ENOENT);
    teardown(&d, &a, &b);
}

struct producer { struct yasl_private_display *display; uint64_t target; };
static void *post_worker(void *ud)
{
    struct producer *producer = ud;
    for (uint32_t i = 0; i < 32; i++)
        assert(yasl_private_display_post(producer->display, &(struct yasl_private_message){
            .target_id = producer->target, .code = i, .payload = &released, .release = release}));
    return NULL;
}

static void test_post_cleanup(void)
{
    struct yasl_private_display d;
    struct yasl_surface a, b;
    setup(&d, &a, &b);
    owner = pthread_self();
    released = 0;
    pthread_t worker;
    struct producer producer = {&d, a.id};
    assert(!pthread_create(&worker, NULL, post_worker, &producer));
    assert(!pthread_join(worker, NULL));
    struct yasl_private_event ev;
    for (uint32_t i = 0; i < 8; i++) {
        assert(yasl_private_display_poll_event(&d, &ev));
        assert(ev.user.code == i && ev.surface == &a);
    }
    assert(released == 7); /* Last event remains borrowed. */
    yasl_private_events_unregister(&a);
    assert(released == 31); /* Undelivered target messages canceled. */
    assert(!yasl_private_display_poll_event(&d, &ev));
    assert(yasl_private_display_post(&d, &(struct yasl_private_message){.payload = &released, .release = release}));
    yasl_private_events_close(&d);
    assert(released == 33); /* Pending message and last consumed message. */
    errno = 0;
    assert(!yasl_private_display_post(&d, &(struct yasl_private_message){.payload = &released, .release = release}));
    assert(errno == EINVAL && released == 33);
}

static int damage_count;
static bool present(struct yasl_surface *s, const struct yasl_private_framebuffer *target,
                    const struct yasl_private_damage_list *damage)
{
    (void)s; (void)target;
    damage_count = damage->n;
    return true;
}
static void test_damage(void)
{
    struct yasl_private_backend_ops present_ops = {.surface_present = present};
    struct yasl_private_display d = {.ops = &present_ops};
    struct yasl_surface s = {.display = &d, .frame_generation = 1, .drawing = true};
    struct yasl_private_frame frame = {.surface = &s, .target = {.generation = 1, .size = {8, 8}}};
    s.active_frame = &frame;
    struct yasl_private_rect2i rects[YASL_PRIVATE_DAMAGE_MAX + 1] = {{0}};
    assert(yasl_private_frame_present(&frame, NULL, 0) && damage_count == -1);
    assert(!yasl_private_frame_present(&frame, NULL, 0) && errno == EALREADY);
    s.frame_presented = false;
    assert(yasl_private_frame_present(&frame, rects, 0) && damage_count == 0);
    s.frame_presented = false;
    assert(yasl_private_frame_present(&frame, rects, YASL_PRIVATE_DAMAGE_MAX + 1) && damage_count == -1);
    s.frame_presented = false;
    rects[YASL_PRIVATE_DAMAGE_MAX].width = -1;
    errno = 0;
    assert(!yasl_private_frame_present(&frame, rects, YASL_PRIVATE_DAMAGE_MAX + 1) && errno == EINVAL);
    assert(!yasl_private_frame_present(&frame, NULL, 1));
    s.drawing = false;
    assert(!yasl_private_frame_present(&frame, NULL, 0));
}

static void test_failures(void)
{
    struct yasl_private_display d;
    struct yasl_surface a, b;
    setup(&d, &a, &b);
    assert(!yasl_private_surface_set_size(&a, (struct yasl_private_vector2i){0, 10}) && errno == EINVAL);
    assert(!yasl_private_surface_set_size(&a, (struct yasl_private_vector2i){10, 10}) && errno == ENOTSUP);
    assert(!yasl_private_surface_set_fullscreen(&a, true) && errno == ENOTSUP);
    assert(!yasl_private_surface_set_maximized(&a, true) && errno == ENOTSUP);
    assert(!yasl_private_surface_minimize(&a) && errno == ENOTSUP);
    a.popup = true;
    assert(!yasl_private_surface_minimize(&a) && errno == EINVAL);
    assert(!yasl_private_clipboard_set(&d, YASL_PRIVATE_CLIPBOARD, "text") && errno == ENOTSUP);
    assert(!yasl_private_clipboard_request(&a, YASL_PRIVATE_CLIPBOARD) && errno == ENOTSUP);
    /* Queued native event allocation/validation failure must be observable. */
    assert(!yasl_private_events_push(&d, &a, &(struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_TEXT, .surface = &a, .text = {"x", SIZE_MAX}}));
    assert(yasl_private_events_error(&d) == ENOMEM);
    teardown(&d, &a, &b);
}


static void test_fifo_preserves_notifications(void)
{
    struct yasl_private_display d;
    struct yasl_surface a, b;
    setup(&d, &a, &b);
    yasl_private_surface_notify_state(&a, YASL_PRIVATE_STATE_ACTIVATED);
    yasl_private_surface_notify_close(&b);
    yasl_private_surface_notify_composition(&a, "preedit", 0, 7);
    assert(input_queue_push(&b.input, &(struct yasl_private_event){
        .type = YASL_PRIVATE_EVENT_PEN, .surface = &b}));
    const enum yasl_private_event_type types[] = {YASL_PRIVATE_EVENT_WINDOW_STATE,
        YASL_PRIVATE_EVENT_WINDOW_CLOSE, YASL_PRIVATE_EVENT_COMPOSITION, YASL_PRIVATE_EVENT_PEN};
    struct yasl_private_event ev;
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); ++i) {
        assert(yasl_private_display_poll_event(&d, &ev));
        assert(ev.type == types[i] && ev.surface == (i % 2 ? &b : &a));
    }
    assert(!yasl_private_events_pending(&d));
    teardown(&d, &a, &b);
}

int main(void)
{
    test_order_and_lifetimes();
    test_post_cleanup();
    test_damage();
    test_failures();
    test_fifo_preserves_notifications();
    return 0;
}
