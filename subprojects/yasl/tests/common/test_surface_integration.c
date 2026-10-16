#define _POSIX_C_SOURCE 200809L
#include <yasl/yasl.h>
#include "common/backend.h"
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#if YASL_HAS_COCOA
void yasl_test_frame_tick(struct yasl_surface *surface);
#endif
static void drain(void)
{
    yasl_event e;
    int n = 0;
    while (yasl_wait_event(&e, 0) > 0)
        assert(++n < 1000);
}
/* Waits up to 2 s for the window to enter or leave fullscreen; a system that
 * ignores the request leaves it as it is. */
static void wait_fullscreen(yasl_surface *s, bool want)
{
    uint64_t deadline = yasl_time_ns() + 2000000000ull;
    while (((yasl_surface_get_info(s).state & YASL_STATE_FULLSCREEN) != 0) != want &&
           yasl_time_ns() < deadline) {
        yasl_event e;
        assert(yasl_wait_event(&e, 20) >= 0);
    }
}
static void *wake_thread(void *unused)
{
    (void)unused;
    struct timespec delay = {0, 20000000};
    nanosleep(&delay, NULL);
    yasl_wake();
    return NULL;
}
static void *wrong_thread(void *data)
{
    assert(yasl_surface_set_title(data, "wrong") == -EPERM);
    int w = 1, h = 1;
    assert(!yasl_cpu_pixels(data, &w, &h) && errno == EPERM && !w && !h);
    struct yasl_native native;
    assert(yasl_surface_get_native(data, &native) == -EPERM && !native.wl_display && !native.x11_display);
    return NULL;
}
struct callback_state {
    yasl_loop *loop;
    yasl_surface *window;
    int refreshes, resizes, refreshes_at_resize, final_losses;
    bool animating;
    int wait_result, shutdown_result;
    yasl_surface *made;
    int made_destroy;
};
static void on_event(void *userdata, const yasl_event *e)
{
    struct callback_state *st = userdata;
    if (e->type == YASL_EVENT_DISPLAY_LOST && e->action == YASL_LOST_FINAL)
        st->final_losses++;
    if (e->type == YASL_EVENT_WINDOW && e->surface == st->window &&
        e->window.width == 120 && e->window.height == 80) {
        st->resizes++;
        st->refreshes_at_resize = st->refreshes;
    }
    if (e->type == YASL_EVENT_REFRESH && e->surface == st->window) {
        st->refreshes++;
        /* An animation asks for its next frame before it presents this one. */
        if (st->animating)
            assert(yasl_surface_request_frame(e->surface) == 0);
        int w = 0, h = 0;
        uint32_t *pixels = yasl_cpu_pixels(e->surface, &w, &h);
        assert(pixels && w > 0 && h > 0);
        pixels[0] = 0xff336699u;
        assert(yasl_cpu_present(e->surface) == 0);
    }
    yasl_event inner;
    st->wait_result = yasl_wait_event(&inner, 0);
    st->shutdown_result = yasl_shutdown();
}
/* A loop callback runs with the display idle, so surface creation and its
 * roundtrips are allowed there. */
static uint64_t create_in_timer(void *userdata, uint64_t id, uint64_t delay_ns)
{
    (void)id; (void)delay_ns;
    struct callback_state *st = userdata;
    st->made = yasl_surface_create(&(struct yasl_surface_options){
        .title = "made by a timer", .width = 64, .height = 64});
    assert(st->made);
    st->made_destroy = yasl_surface_destroy(st->made);
    return 0;
}
static uint64_t quit_in_timer(void *userdata, uint64_t id, uint64_t delay_ns)
{
    (void)id; (void)delay_ns;
    yasl_loop_quit(userdata);
    return 0;
}
static void step_until(struct callback_state *st, const int *counter, int want)
{
    uint64_t deadline = yasl_time_ns() + 5000000000ull;
    while (*counter < want && yasl_time_ns() < deadline)
        assert(yasl_loop_step(st->loop, 50) >= 0);
    assert(*counter >= want);
}
static void test_callback_mode(enum yasl_backend backend)
{
    struct callback_state st = {.loop = yasl_loop_create()};
    assert(st.loop);
    const struct yasl_options options = {
        .backend = backend, .loop = st.loop, .on_event = on_event, .userdata = &st};
    assert(yasl_init(&options) == 0);
    assert(yasl_init(&options) == -EALREADY);
    assert(yasl_loop_destroy(st.loop) == -EBUSY);
    yasl_event e;
    assert(yasl_wait_event(&e, 0) == -EBUSY);
    st.window = yasl_surface_create(&(struct yasl_surface_options){
        .title = "callback mode", .width = 96, .height = 64});
    assert(st.window);

    /* Events reach on_event while the application only steps its loop. */
    step_until(&st, &st.refreshes, 1);
    assert(st.wait_result == -EBUSY && st.shutdown_result == -EBUSY);

    /* Frame requests coalesce into one REFRESH, sent once the window can
     * show a frame. */
    int before = st.refreshes;
    assert(yasl_surface_request_frame(st.window) == 0);
    assert(yasl_surface_request_frame(st.window) == 0);
    step_until(&st, &st.refreshes, before + 1);
    for (int i = 0; i < 4; i++)
        assert(yasl_loop_step(st.loop, 25) >= 0);
    assert(st.refreshes == before + 1);

    /* The frame an animation presents after its request is the frame the
     * request waits for: one REFRESH per display refresh, not two. */
    st.animating = true;
    assert(yasl_surface_request_frame(st.window) == 0);
    before = st.refreshes;
    uint64_t start = yasl_time_ns();
    while (yasl_time_ns() - start < 1000000000ull)
        assert(yasl_loop_step(st.loop, 50) >= 0);
    st.animating = false;
    int frames = st.refreshes - before;
    fprintf(stderr, "%d REFRESH events in 1 s of animation\n", frames);
    assert(frames >= 20 && frames <= 75);

    /* A resize owes one REFRESH, after its WINDOW event. */
    for (int i = 0; i < 6; i++)
        assert(yasl_loop_step(st.loop, 25) >= 0);
    before = st.refreshes;
    assert(yasl_surface_request_size(st.window, 120, 80) == 0);
    step_until(&st, &st.resizes, 1);
    for (int i = 0; i < 8; i++)
        assert(yasl_loop_step(st.loop, 25) >= 0);
    fprintf(stderr, "%d REFRESH events for one resize, %d before its WINDOW event\n",
            st.refreshes - before, st.refreshes_at_resize - before);
    assert(st.resizes == 1 && st.refreshes_at_resize == before);
    assert(st.refreshes == before + 1);

    /* An idle loop waits instead of spinning. */
    int steps = 0;
    uint64_t end = yasl_time_ns() + 300000000ull;
    while (yasl_time_ns() < end) {
        assert(yasl_loop_step(st.loop, 100) >= 0);
        steps++;
    }
    assert(steps < 30);

    assert(yasl_loop_add_timer(st.loop, 0, create_in_timer, &st));
    while (!st.made)
        assert(yasl_loop_step(st.loop, 50) >= 0);
    assert(st.made_destroy == 0);

    /* A lost event ends the session: on_event hears it as a final loss. A
     * string event with no text cannot be queued. */
    struct yasl_private_event broken = {.type = YASL_PRIVATE_EVENT_PASTE, .surface = st.window,
                                        .text = {NULL, 1}};
    assert(!yasl_private_events_push(st.window->display, st.window, &broken));
    step_until(&st, &st.final_losses, 1);

    assert(yasl_loop_add_timer(st.loop, YASL_PRIVATE_MS_TO_NS(10), quit_in_timer, st.loop));
    assert(yasl_loop_run(st.loop) == 0);
    assert(yasl_shutdown() == 0);
    assert(yasl_loop_destroy(st.loop) == 0);
}
static void push(yasl_surface *s, struct yasl_private_event event)
{
    event.surface = s;
    assert(yasl_private_events_push(s->display, s, &event));
}
/* Drains the queue; returns how many WINDOW events came, each of this pixel size. */
static int windows_of(int pixel_width, int pixel_height)
{
    yasl_event e;
    int n = 0;
    while (yasl_wait_event(&e, 0) > 0)
        if (e.type == YASL_EVENT_WINDOW) {
            assert(e.window.pixel_width == pixel_width && e.window.pixel_height == pixel_height);
            n++;
        }
    return n;
}
static int refreshes;
/* Whether the display still lists the surface with this address; compared, never dereferenced. */
static bool listed(struct yasl_private_display *d, uintptr_t id)
{
    for (yasl_surface *p = d->surfaces; p; p = p->event_next)
        if ((uintptr_t)p == id)
            return true;
    return false;
}
static void refresh(yasl_surface *s)
{
    refreshes++;
    yasl_event e;
    assert(yasl_wait_event(&e, 0) == -EBUSY);
    assert(errno == EBUSY);
    assert(yasl_surface_destroy(s) == -EBUSY);
    struct yasl_surface_options options = {.width = 32, .height = 32};
    assert(!yasl_surface_create(&options) && errno == EBUSY);
    options.type = YASL_SURFACE_GL;
    assert(!yasl_surface_create(&options) && errno == EBUSY);
    /* The geometry being drawn is already current; its WINDOW event comes later. */
    struct yasl_surface_info info = yasl_surface_get_info(s);
    int w, h;
    assert(yasl_cpu_pixels(s, &w, &h) && yasl_cpu_present(s) == 0);
    assert(w == info.pixel_width && h == info.pixel_height);
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    enum yasl_backend backend = !strcmp(argv[1], "wayland") ? YASL_BACKEND_WAYLAND
                                : !strcmp(argv[1], "x11")   ? YASL_BACKEND_X11
                                                            : YASL_BACKEND_COCOA;
    const struct yasl_options options = {.backend = backend, .app_id = "org.yasl.surface-test"};
    assert(yasl_init(&options) == 0);
    assert(yasl_init(&options) == -EALREADY);
    /* Other options would need another display. */
    assert(yasl_init(&(struct yasl_options){.backend = backend}) == -EBUSY);
    assert(yasl_get_backend() == backend);
    unsigned features = yasl_get_features();
    yasl_surface *s = yasl_surface_create(
        &(struct yasl_surface_options){.title = "CPU contract", .width = 96, .height = 80});
    if (!s) {
        fprintf(stderr, "surface: %s\n", strerror(errno));
        return 1;
    }
    assert(yasl_surface_set_userdata(s, &backend) == 0);
    assert(yasl_surface_get_userdata(s) == &backend);
    struct yasl_native native;
    assert(yasl_surface_get_native(s, &native) == 0);
    assert(backend != YASL_BACKEND_WAYLAND || (native.wl_display && native.wl_surface));
    assert(backend != YASL_BACKEND_X11 || (native.x11_display && native.x11_window));
    assert(backend != YASL_BACKEND_COCOA || (native.ns_window && native.ns_view));
    assert(yasl_surface_get_native(s, NULL) == -EINVAL);
    assert(!yasl_metal_get_layer(s) && errno == EBUSY);
    if (backend == YASL_BACKEND_COCOA) {
        assert(!(features & YASL_FEATURE_OPENGL));
        assert(!yasl_surface_create(&(struct yasl_surface_options){
            .title = "unsupported GL", .width = 64, .height = 64, .type = YASL_SURFACE_GL}));
        assert(errno == ENOTSUP);
        assert(yasl_gl_make_current(NULL) == 0);
        assert(!yasl_gl_get_proc_address("glClear") && errno == ENOTSUP);
    }
    /* The first events report the window size, then ask for a frame. */
    yasl_event e;
    int fw = 0, fh = 0;
    bool window = false;
    for (int i = 0; i < 1000; i++) {
        assert(yasl_wait_event(&e, i ? 20 : 0) >= 0);
        if (e.type == YASL_EVENT_WINDOW) {
            window = true;
            fw = e.window.pixel_width;
            fh = e.window.pixel_height;
        }
        if (e.type == YASL_EVENT_REFRESH) {
            assert(window && fw > 0 && fh > 0 && e.surface == s);
            break;
        }
    }
    assert(e.type == YASL_EVENT_REFRESH);
    struct yasl_surface_info info = yasl_surface_get_info(s);
    assert(info.pixel_width == fw && info.pixel_height == fh && info.scale > 0);

    /* CPU pixels persist between frames and are the window's own buffer. */
    int w, h;
    uint32_t *pixels = yasl_cpu_pixels(s, &w, &h);
    assert(pixels && w == fw && h == fh);
    assert(pixels[0] == 0 && pixels[(size_t)w * h - 1] == 0);
    for (size_t i = 0; i < (size_t)w * h; i++)
        pixels[i] = 0xff102030u;
    pixels[(size_t)3 * w + 2] = 0xffaabbccu;
    assert(yasl_cpu_present(s) == 0);
    drain();
    pixels = yasl_cpu_pixels(s, &w, &h);
    assert(pixels[0] == 0xff102030u && pixels[(size_t)3 * w + 2] == 0xffaabbccu);

    /* Waiting ends only on an event, the timeout or yasl_wake, never early. */
    drain();
    uint64_t start = yasl_time_ns();
    int n = yasl_wait_event(&e, 200);
    assert(n == 1 || yasl_time_ns() - start >= 190000000ull);
    drain();
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, wake_thread, NULL));
    start = yasl_time_ns();
    n = yasl_wait_event(&e, 5000);
    pthread_join(thread, NULL);
    assert(n == 1 || (n == 0 && yasl_time_ns() - start < 4000000000ull));
    assert(!pthread_create(&thread, NULL, wrong_thread, s));
    pthread_join(thread, NULL);

    yasl_surface *second = yasl_surface_create(
        &(struct yasl_surface_options){.width = 32, .height = 32, .userdata = &backend});
    assert(second && yasl_surface_get_userdata(second) == &backend);
    assert(yasl_surface_destroy(second) == 0);

    /* Text input */
    int ime = yasl_surface_set_text_input(s, &(struct yasl_text_input){
        .text = "a\xc3\xa9z", .length = 4, .selection_begin = 1, .selection_end = 3,
        .revision = 7, .caret_x = 1, .caret_y = 2, .caret_width = 1, .caret_height = 10});
    assert(ime == 0 || ime == -ENOTSUP);
    assert(yasl_surface_set_text_input(s, &(struct yasl_text_input){
               .text = "a\xc3\xa9z", .length = 4, .selection_begin = 2, .selection_end = 3}) == -EILSEQ);
    assert(yasl_surface_set_text_input(s, &(struct yasl_text_input){
               .text = "\xc0\xaf", .length = 2}) == -EILSEQ);
    assert(yasl_surface_set_text_input(s, &(struct yasl_text_input){
               .text = "ab", .length = 2, .selection_end = 3}) == -EINVAL);
    assert(yasl_surface_set_text_input(s, &(struct yasl_text_input){.caret_x = 1e300}) == -EOVERFLOW);
    assert(yasl_surface_set_text_input(s, NULL) == 0 || ime == -ENOTSUP);

    /* Arguments are checked before the backend sees them. */
    assert(yasl_surface_begin_move_resize(s, YASL_EDGE_TOP | YASL_EDGE_BOTTOM) == -EINVAL);
    assert(yasl_surface_begin_move_resize(s, 16) == -EINVAL);
    assert(yasl_surface_request(s, (enum yasl_request)9) == -EINVAL);
    assert(yasl_surface_request_size(s, 0, 10) == -EINVAL);
    assert(yasl_surface_set_cursor(s, (enum yasl_cursor)99) == -EINVAL);
    int locked = yasl_surface_set_cursor(s, YASL_CURSOR_LOCKED);
    assert(locked == ((features & YASL_FEATURE_LOCKED_CURSOR) ? 0 : -ENOTSUP));
    assert(yasl_surface_set_cursor(s, YASL_CURSOR_ARROW) == 0);
    assert(yasl_drag_start(s, &(struct yasl_drag_image){.pixel_width = 4, .pixel_height = 4}) == -EINVAL);
    uint32_t icon[4] = {0};
    assert(yasl_drag_start(s, &(struct yasl_drag_image){
               .pixels = icon, .pixel_width = 2, .pixel_height = 2, .hotspot_x = 2}) == -EINVAL);
    assert(!yasl_popup_create(s, &(struct yasl_popup_options){.width = 10, .height = 10,
                                                              .anchor = YASL_EDGE_LEFT | YASL_EDGE_RIGHT}) &&
           errno == EINVAL);
    assert(yasl_clipboard_set((enum yasl_selection)5, "x") == -EINVAL);
    assert(yasl_clipboard_set(YASL_CLIPBOARD, NULL) == -EINVAL);
    assert(yasl_clipboard_set(YASL_CLIPBOARD, "\xff") == -EILSEQ);
    /* Offering a selection needs input on one of our windows; on a live desktop the new
     * window may not have keyboard focus yet. */
    int primary = yasl_clipboard_set(YASL_PRIMARY, "x");
    assert((features & YASL_FEATURE_PRIMARY_SELECTION) ? primary == 0 || primary == -ESTALE
                                                         : primary == -ENOTSUP);
    drain();

    /* Event conversion: positions are logical, text is owned until the next wait. Pushed
     * events are returned before the server is read, so draining first keeps a live
     * desktop's input out of their way. */
    drain();
    uint32_t real_scale = s->scale_120;
    s->scale_120 = 240;
    push(s, (struct yasl_private_event){
                .type = YASL_PRIVATE_EVENT_PEN,
                .pen = {.device_id = 10, .x = 25, .y = 18.5, .pressure = 0.3,
                        .axes_valid = YASL_PRIVATE_PEN_PRESSURE, .action = YASL_PRIVATE_PEN_DOWN,
                        .in_contact = true}});
    assert(yasl_wait_event(&e, 0) == 1 && e.type == YASL_EVENT_PEN && e.action == YASL_PEN_DOWN);
    assert(e.x == 12.5 && e.y == 9.25 && e.pen.device_id == 10 && e.pen.pressure == 0.3 &&
           (e.pen.axes & YASL_PEN_PRESSURE) && e.pen.in_contact);
    push(s, (struct yasl_private_event){
                .type = YASL_PRIVATE_EVENT_POINTER_DOWN,
                .pointer = {.pos = {20, 10}, .button = YASL_PRIVATE_BUTTON_SECONDARY,
                            .buttons = 4, .mods = YASL_PRIVATE_MOD_CTRL}});
    assert(yasl_wait_event(&e, 0) == 1 && e.type == YASL_EVENT_POINTER &&
           e.action == YASL_POINTER_DOWN && e.x == 10 && e.y == 5 && e.mods == YASL_MOD_CTRL &&
           e.pointer.button == YASL_BUTTON_SECONDARY && e.pointer.buttons == 4);
    s->scale_120 = real_scale;
    push(s, (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_TEXT, .text = {"owned", 5}});
    assert(yasl_wait_event(&e, 0) == 1 && e.type == YASL_EVENT_TEXT);
    assert(e.length == 5 && !strcmp(e.text, "owned"));
    push(s, (struct yasl_private_event){
                .type = YASL_PRIVATE_EVENT_PASTE,
                .paste = {.data = "", .request_id = 1, .status = YASL_PRIVATE_TRANSFER_UNAVAILABLE}});
    assert(yasl_wait_event(&e, 0) == 1 && e.type == YASL_EVENT_CLIPBOARD &&
           e.action == YASL_TRANSFER_UNAVAILABLE && e.length == 0);
    /* Losing focus releases held input; activation itself comes from the window state. */
    push(s, (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_FOCUS, .focus = {.focused = false}});
    assert(yasl_wait_event(&e, 0) == 1 && e.type == YASL_EVENT_INPUT_RESET);
    push(s, (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_FOCUS, .focus = {.focused = true}});
    /* Gaining focus reports nothing; an empty queue makes this wait read the server too. */
    while (yasl_wait_event(&e, 0) > 0)
        assert(e.type != YASL_EVENT_INPUT_RESET);
    push(s, (struct yasl_private_event){.type = YASL_PRIVATE_EVENT_DROP_HOVER,
                                        .drop = {YASL_PRIVATE_DROP_LEAVE, {0, 0}}});
    assert(yasl_wait_event(&e, 0) == 1 && e.type == YASL_EVENT_DROP && e.action == YASL_DROP_LEAVE);

    /* A resize keeps the overlap of the pixels and zeroes the rest. */
    assert(yasl_surface_request_size(s, 112, 96) == 0);
    uint64_t resize_deadline = yasl_time_ns() + 3000000000ull;
    do {
        assert(yasl_wait_event(&e, 20) >= 0);
        info = yasl_surface_get_info(s);
    } while ((info.width != 112 || info.height != 96) && yasl_time_ns() < resize_deadline);
    assert(info.width == 112 && info.height == 96);
    pixels = yasl_cpu_pixels(s, &w, &h);
    assert(pixels && w == info.pixel_width && h == info.pixel_height);
    assert(pixels[0] == 0xff102030u && pixels[(size_t)3 * w + 2] == 0xffaabbccu);
    assert(pixels[(size_t)w * h - 1] == 0);
    assert(yasl_cpu_present(s) == 0);

    /* The refresh callback is reserved for modal loops; ordinary waits deliver REFRESH. */
    s->refresh = refresh;
    for (int i = 0; i < 5; i++) {
        assert(yasl_cpu_pixels(s, &w, &h) && yasl_cpu_present(s) == 0);
        assert(yasl_wait_event(&e, 20) >= 0);
    }
    assert(refreshes == 0);
#if YASL_HAS_COCOA
    s->display->modal = true;
    yasl_test_frame_tick(s);
    uint64_t deadline = yasl_private_loop_now_ns() + 3000000000ull;
    while (!refreshes && yasl_private_loop_now_ns() < deadline)
        assert(yasl_wait_event(&e, 20) >= 0);
    assert(refreshes > 0);
    s->display->modal = false;
#endif
    s->refresh = NULL;

    /* WINDOW events report the current geometry: resizes queued while the
     * application was away fold into one, and a size it already has reports
     * nothing. */
    drain();
    struct yasl_private_geometry real = {s->size, s->scale_120};
    struct yasl_private_geometry wider = {{real.size.x + 8, real.size.y}, real.scale_120};
    struct yasl_private_geometry larger = {{real.size.x + 16, real.size.y + 8}, real.scale_120};
    yasl_private_surface_notify_configure(s, wider);
    yasl_private_surface_notify_configure(s, larger);
    assert(windows_of(larger.size.x, larger.size.y) == 1);
    yasl_private_surface_notify_configure(s, wider);
    yasl_private_surface_notify_configure(s, larger);
    assert(windows_of(0, 0) == 0);
    yasl_private_surface_notify_configure(s, real);
    assert(windows_of(real.size.x, real.size.y) == 1);

    /* Title and window requests: each either works or reports ENOTSUP. */
    assert(yasl_surface_set_title(s, "CPU contract renamed") == 0);
    /* Weston configures a minimized fullscreen window at 0 x 0, then rejects
     * every frame, so the window leaves fullscreen before it minimizes. */
    int fullscreen = yasl_surface_request(s, YASL_REQUEST_FULLSCREEN);
    assert(fullscreen == 0 || fullscreen == -ENOTSUP);
    if (fullscreen == 0)
        wait_fullscreen(s, true);
    static const enum yasl_request requests[] = {YASL_REQUEST_RESTORE, YASL_REQUEST_MAXIMIZE,
                                                 YASL_REQUEST_MINIMIZE};
    for (size_t i = 0; i < sizeof requests / sizeof requests[0]; i++) {
        int r = yasl_surface_request(s, requests[i]);
        assert(r == 0 || r == -ENOTSUP);
        if (i == 0 && r == 0)
            wait_fullscreen(s, false);
    }
    drain();
    int restored = yasl_surface_request(s, YASL_REQUEST_RESTORE);
    assert(restored == 0 || restored == -ENOTSUP);
    drain();
    int bell = yasl_surface_bell(s);
    assert(bell == 0 || bell == -ENOTSUP);

    /* Desktop queries and the drag cancel, which is a no-op without a drag. */
    assert(yasl_get_appearance() <= 7);
    int double_click = yasl_get_double_click_ms();
    assert(double_click >= 1 && double_click <= 10000);
    assert(yasl_drag_cancel() == 0);

    /* Nothing presses a button here, so calls that need a press report ESTALE. */
    int no_press[] = {yasl_surface_begin_move_resize(s, 0),
                      yasl_surface_begin_move_resize(s, YASL_EDGE_BOTTOM | YASL_EDGE_RIGHT),
                      yasl_surface_show_window_menu(s, 1, 1), yasl_drag_start(s, NULL)};
    for (size_t i = 0; i < sizeof(no_press) / sizeof(no_press[0]); i++)
        assert(no_press[i] == -ESTALE || no_press[i] == -ENOTSUP);
    drain();

    /* A popup needs an input grab to open; if it opens, its parent owns it. */
    struct yasl_private_display *display = s->display;
    struct yasl_popup_options popup_options = {
        .width = 40, .height = 30, .anchor_width = 1, .anchor_height = 1};
    errno = 0;
    yasl_surface *popup = yasl_popup_create(s, &popup_options);
    uintptr_t popup_id = (uintptr_t)popup;
    if (!popup) {
        assert(errno == ESTALE);
    } else {
        assert(listed(display, popup_id));
        assert(yasl_popup_reposition(popup, &popup_options) <= 0);
        int pw = 0, ph = 0;
        assert(yasl_cpu_pixels(popup, &pw, &ph) && pw > 0 && ph > 0);
    }

    /* Clipboard: offering text may be refused without input; if accepted, it comes back. */
    int offered = yasl_clipboard_set(YASL_CLIPBOARD, "yasl");
    assert(offered <= 0);
    if (offered == 0) {
        assert(yasl_clipboard_request(s, YASL_CLIPBOARD) == 0);
        bool got = false;
        uint64_t clipboard_deadline = yasl_time_ns() + 2000000000ull;
        while (!got && yasl_time_ns() < clipboard_deadline) {
            assert(yasl_wait_event(&e, 50) >= 0);
            if (e.type != YASL_EVENT_CLIPBOARD)
                continue;
            got = true;
            assert(e.surface == s && e.action <= YASL_TRANSFER_FAILED);
            if (e.action == YASL_TRANSFER_OK)
                assert(e.length == 4 && !strcmp(e.text, "yasl"));
        }
        assert(got);
    }

    /* Destroying the parent also destroys its popup. */
    assert(yasl_surface_destroy(s) == 0);
    if (popup)
        assert(!listed(display, popup_id));
    assert(yasl_init(&options) == -EALREADY);
    s = yasl_surface_create(&(struct yasl_surface_options){.title = "shared backend remains",
                                                           .width = 64, .height = 64});
    assert(s);
    yasl_shutdown();
    assert(yasl_init(&(struct yasl_options){.backend = backend}) == 0);
    s = yasl_surface_create(
        &(struct yasl_surface_options){.title = "reinitialized", .width = 64, .height = 64});
    assert(s);
    yasl_shutdown();
    test_callback_mode(backend);
    puts("public surface lifecycle, pixels, ownership, input, wake, refresh and callback mode passed");
    return 0;
}
