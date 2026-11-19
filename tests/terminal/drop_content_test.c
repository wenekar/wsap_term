#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "app/tabs/tabs.h"
#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "input/input.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "terminal/internal.h"
#include "ui/surface.h"
#include "ui/yasl/client.h"
#include "ui/yasl/internal.h"
#include "ui/yasl/native_input.h"
#include "ui/yasl/testing.h"

static void drain(struct window *w)
{
    struct input_event input;
    while (input_queue_take(&w->input, &input)) {
        window_ui_test_route(w, &input);
        input_event_release(&input);
    }
}

static void drop(struct window *w, const char *mime, const char *data,
                 size_t len)
{
    struct yasl_event event = {
        .type = YASL_EVENT_DROP,
        .action = YASL_DROP_RELEASE,
        .text = data,
        .length = len,
    };
    snprintf(event.drop.mime, sizeof(event.drop.mime), "%s", mime);
    window_native_input(w, &event);
    drain(w);
}

/* Returns the number of bytes that the terminal wrote, or 0 when none wait. */
static ssize_t take(int fd, char *out, size_t size)
{
    ssize_t length = read(fd, out, size);
    if (length < 0 && errno == EAGAIN)
        return 0;
    assert(length >= 0);
    return length;
}

static void expect_text(int fd, const char *value)
{
    char received[512];
    ssize_t length = take(fd, received, sizeof(received));
    if (length != (ssize_t)strlen(value) ||
        memcmp(received, value, (size_t)length)) {
        fprintf(stderr, "Expected terminal input: %s\nReceived bytes:", value);
        for (ssize_t i = 0; i < length; i++)
            fprintf(stderr, " %02x", (unsigned char)received[i]);
        fputc('\n', stderr);
        assert(false);
    }
    assert(take(fd, received, sizeof(received)) == 0);
}

int main(void)
{
    char runtime[] = "/tmp/wsap-drop-test-XXXXXX";
    assert(mkdtemp(runtime));
    assert(setenv("XDG_RUNTIME_DIR", runtime, 1) == 0);

    struct test_app app;
    test_app_init(&app, "/bin/cat", (struct vector2i){ .x = 800, .y = 480 });
    struct window *w = test_terminal_window(app.app);
    assert(w && app_tabs_new(w));
    struct terminal *content = app_tabs_active(w);
    struct session *session = terminal_session(content);
    assert(session);
    int fds[2];
    assert(pipe(fds) == 0);
    for (int i = 0; i < 2; i++)
        assert(fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK) == 0);
    assert(session_write_queue_bind(&session->input_writer, session->loop, fds[1]));
    /* The drop goes to the widget under the pointer. Put the pointer in the
     * terminal view, below the tab bar. */
    ui_surface_set_scale(window_ui_surface(w), (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(window_ui_surface(w), (struct vector2i){ .x = 800, .y = 480 });
    ui_surface_prepare(window_ui_surface(w));
    struct rect2i view = terminal_view_rect(content);
    w->pointer.pos = vector2i_add(view.pos,
                                  (struct vector2i){ .x = view.size.x / 2,
                                                     .y = view.size.y / 2 });

    /* An image drop saves the bytes and pastes the quoted file path. */
    const char image[] = "image bytes";
    drop(w, "image/png", image, sizeof(image) - 1);
    char pasted[512];
    ssize_t length = take(fds[0], pasted, sizeof(pasted));
    char prefix[256];
    int need = snprintf(prefix, sizeof(prefix), "'%s/wsap_term/drops/drop-",
                        runtime);
    assert(need > 0 && (size_t)need < sizeof(prefix));
    if (length < need + 5 || memcmp(pasted, prefix, (size_t)need) ||
        memcmp(pasted + length - 5, ".png'", 5)) {
        fprintf(stderr, "The image drop did not paste a saved path: %.*s\n",
                (int)length, pasted);
        assert(false);
    }
    pasted[length - 1] = 0;
    char *path = pasted + 1;
    FILE *file = fopen(path, "rb");
    assert(file);
    char saved[32];
    size_t saved_len = fread(saved, 1, sizeof(saved), file);
    fclose(file);
    assert(saved_len == sizeof(image) - 1);
    assert(memcmp(saved, image, saved_len) == 0);
    assert(take(fds[0], pasted, sizeof(pasted)) == 0);

    /* A uri list with no local file becomes one line, joined by spaces. */
    const char uris[] = "https://example.com/a\r\nhttps://example.com/b\n";
    drop(w, "text/uri-list", uris, sizeof(uris) - 1);
    expect_text(fds[0], "https://example.com/a https://example.com/b");

    window_close(w);
    test_app_fini(&app);
    unlink(path);
    return 0;
}
