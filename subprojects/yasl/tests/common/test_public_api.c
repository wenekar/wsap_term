#include <yasl/yasl.h>
#include <assert.h>
#include <string.h>

/* Before yasl_init there is no display: argument errors are reported first, owner-thread
 * calls fail with ENODEV, and queries return their documented fallbacks. Nothing here may
 * open a display, so only invalid arguments reach the constructors and openers. */
int main(void)
{
    static char anchor; /* A fake handle; it is compared and never dereferenced. */
    yasl_surface *fake = (yasl_surface *)&anchor;

    assert(yasl_shutdown() == 0 && yasl_shutdown() == 0);
    assert(yasl_get_backend() == YASL_BACKEND_AUTO && yasl_get_features() == 0);
    assert(yasl_get_appearance() == 0 && yasl_get_double_click_ms() == 400);

    /* Invalid constructor arguments fail before any display is opened. */
    assert(!yasl_surface_create(&(struct yasl_surface_options){.width = 32767, .height = 32767}) &&
           errno == EINVAL);
    assert(!yasl_surface_create(&(struct yasl_surface_options){.width = 32768, .height = 1}) &&
           errno == EINVAL);
    assert(!yasl_surface_create(&(struct yasl_surface_options){
               .width = 8, .height = 8, .type = YASL_SURFACE_GL, .gl = {.share = fake}}) &&
           errno == EINVAL);

    /* Owner-thread calls need a display, whatever the surface argument is. */
    assert(yasl_surface_destroy(NULL) == 0 && yasl_surface_destroy(fake) == -ENODEV);
    assert(yasl_surface_set_userdata(NULL, &anchor) == -ENODEV);
    assert(yasl_surface_set_title(NULL, "x") == -ENODEV);
    assert(yasl_surface_set_title(fake, NULL) == -ENODEV);
    assert(yasl_surface_request(NULL, YASL_REQUEST_MAXIMIZE) == -ENODEV);
    assert(yasl_surface_request(fake, (enum yasl_request)9) == -ENODEV);
    assert(yasl_surface_request_size(NULL, 10, 10) == -ENODEV);
    assert(yasl_surface_set_cursor(NULL, YASL_CURSOR_ARROW) == -ENODEV);
    assert(yasl_surface_set_text_input(NULL, NULL) == -ENODEV);
    assert(yasl_surface_begin_move_resize(NULL, 0) == -ENODEV);
    assert(yasl_surface_show_window_menu(NULL, 1, 1) == -ENODEV);
    assert(yasl_surface_bell(NULL) == -ENODEV);
    assert(yasl_surface_set_background_color(NULL, 0) == -ENODEV);
    assert(yasl_surface_set_background_color(fake, 0) == -ENODEV);
    assert(yasl_surface_place_under_pointer(NULL, 1, 1) == -ENODEV);
    assert(yasl_drag_carry(NULL, 1, 1) == -ENODEV);
    assert(yasl_drag_start(NULL, NULL) == -ENODEV);
    assert(yasl_drag_cancel() == -ENODEV);
    assert(yasl_clipboard_set(YASL_CLIPBOARD, "x") == -ENODEV);
    assert(yasl_clipboard_set(YASL_PRIMARY, NULL) == -ENODEV);
    assert(yasl_clipboard_request(NULL, YASL_CLIPBOARD) == -ENODEV);
    assert(yasl_notify("title", NULL) == -ENODEV);
    assert(yasl_cpu_present(NULL) == -ENODEV);
    assert(yasl_gl_set_swap_interval(NULL, 1) == -ENODEV);
    assert(yasl_gl_present(NULL) == -ENODEV);
    assert(yasl_gl_make_current(fake) == -ENODEV);
    assert(!yasl_gl_get_proc_address("glClear") && errno == ENODEV);
    assert(!yasl_gl_get_proc_address(NULL) && errno == ENODEV);
    assert(!yasl_metal_get_layer(NULL) && errno == ENODEV);
    struct yasl_popup_options popup = {.width = 4, .height = 4};
    assert(yasl_popup_reposition(NULL, &popup) == -ENODEV);
    assert(!yasl_popup_create(NULL, &popup) && errno == ENODEV);

    /* Queries name their fallbacks. */
    assert(yasl_surface_get_userdata(NULL) == NULL && yasl_surface_get_userdata(fake) == NULL);
    struct yasl_surface_info info = yasl_surface_get_info(NULL);
    assert(info.width == 0 && info.height == 0 && info.pixel_width == 0 && info.pixel_height == 0 &&
           info.scale == 1.0 && info.state == 0);
    info = yasl_surface_get_info(fake);
    assert(info.width == 0 && info.pixel_height == 0 && info.scale == 1.0);
    int w = 7, h = 7;
    assert(!yasl_cpu_pixels(NULL, &w, &h) && errno == ENODEV && w == 0 && h == 0);
    assert(!yasl_cpu_pixels(NULL, NULL, NULL) && errno == ENODEV);
    struct yasl_native native = {.x11_window = 7};
    assert(yasl_surface_get_native(NULL, NULL) == -EINVAL);
    assert(yasl_surface_get_native(NULL, &native) == -ENODEV && native.x11_window == 0);

    /* Waiting needs a display; yasl_wake with no display does nothing and does not crash. */
    yasl_event e = {.type = YASL_EVENT_QUIT};
    assert(yasl_wait_event(&e, 0) == -ENODEV && e.type == YASL_EVENT_NONE);
    yasl_wake();
    yasl_wake();
    assert(yasl_shutdown() == 0);

    /* The event clock never runs backwards. */
    uint64_t last = yasl_time_ns();
    assert(last > 0);
    for (int i = 0; i < 1000; i++) {
        uint64_t now = yasl_time_ns();
        assert(now >= last);
        last = now;
    }

    /* yasl_open_uri rejects bad URIs without opening anything. */
    assert(yasl_open_uri(NULL) == -EINVAL);
    assert(yasl_open_uri("") == -EINVAL);
    assert(yasl_open_uri("no-colon") == -EINVAL);
    assert(yasl_open_uri("1http://x") == -EINVAL);
    assert(yasl_open_uri("http://x\n") == -EINVAL);

    /* yasl_drop_next_path walks a text/uri-list: local file lines only, comments skipped. */
    static const char uris[] =
        "file:///a/b\r\nhttp://x/y\r\n# comment\r\nfile://localhost/c%20d\r\n";
    const char *cursor = uris;
    char out[64] = "keep";
    assert(yasl_drop_next_path(&cursor, NULL, 0) == 4 && cursor == uris);
    assert(yasl_drop_next_path(&cursor, out, 4) == 4 && cursor == uris && !strcmp(out, "keep"));
    assert(yasl_drop_next_path(&cursor, out, 5) == 4 && cursor != uris && !strcmp(out, "/a/b"));
    assert(yasl_drop_next_path(&cursor, out, sizeof out) == 4 && !strcmp(out, "/c d"));
    assert(yasl_drop_next_path(&cursor, out, sizeof out) == 0);
    assert(yasl_drop_next_path(NULL, out, sizeof out) == 0);
    const char *fresh = uris;
    assert(yasl_drop_next_path(&fresh, NULL, 1) == 0 && fresh == uris);

    /* yasl_get_system_string: length first, the NUL only when the capacity exceeds it. */
    char path[4096], full[4096], locale[64];
    size_t n = yasl_get_system_string(YASL_EXECUTABLE_PATH, NULL, 0);
    assert(n > 0 && n < sizeof full);
    assert(yasl_get_system_string(YASL_EXECUTABLE_PATH, full, sizeof full) == n && strlen(full) == n);
    memset(path, 'x', sizeof path);
    assert(yasl_get_system_string(YASL_EXECUTABLE_PATH, path, n) == n && path[n - 1] == 'x' &&
           path[n] == 'x');
    assert(yasl_get_system_string(YASL_EXECUTABLE_PATH, path, n - 1) == n && path[n - 2] == 'x' &&
           path[n] == 'x');
    assert(yasl_get_system_string(YASL_EXECUTABLE_PATH, path, n + 1) == n && path[n] == '\0' &&
           !strcmp(path, full));
    size_t ln = yasl_get_system_string(YASL_LOCALE, locale, sizeof locale);
    assert(ln > 0 && ln < sizeof locale && strlen(locale) == ln && !strpbrk(locale, ".@"));
    assert(yasl_get_system_string(YASL_LOCALE, NULL, 16) == 0 && errno == EINVAL);
    assert(yasl_get_system_string((enum yasl_system_string)9, locale, sizeof locale) == 0 &&
           errno == EINVAL);
#if defined(__linux__)
    errno = 0;
    assert(yasl_get_system_string(YASL_RESOURCE_DIR, path, sizeof path) == 0 && errno == ENOTSUP);
#endif
    return 0;
}
