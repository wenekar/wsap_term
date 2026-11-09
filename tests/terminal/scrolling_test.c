#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "session/session.h"
#include "terminal/internal.h"
#include "terminal/terminal.h"
#include "ui/event.h"
#include "ui/surface.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"

static int scroll_pipe[2];
static struct window *test_window;

static void feed(struct wsap_vtcore *term, const char *bytes)
{
    assert(wsap_vtcore_feed(term, bytes, strlen(bytes), 0));
}

static void expect_bytes(const char *want, size_t len)
{
    char got[4096];
    assert(len <= sizeof(got));
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(scroll_pipe[0], got + off, len - off);
        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        assert(n == -1 && (errno == EAGAIN || errno == EINTR));
        if (n < 0 && errno == EINTR)
            continue;
        break;
    }
    assert(off == len);
    assert(!memcmp(got, want, len));
    char extra[1];
    assert(read(scroll_pipe[0], extra, sizeof(extra)) == -1 && errno == EAGAIN);
}

static void expect_no_bytes(void)
{
    char extra[1];
    assert(read(scroll_pipe[0], extra, sizeof(extra)) == -1 && errno == EAGAIN);
}

static void scroll(int lines, float delta)
{
    struct rect2i view = terminal_view_rect(test_window);
    assert(view.width > 0 && view.height > 0);
    struct vector2i pos = {
        .x = view.x + view.width / 2,
        .y = view.y + view.height / 2,
    };
    struct ui_surface *surface = window_ui_surface(test_window);
    ui_surface_prepare(surface);
    struct ui_event event = ui_event_pointer((struct ui_pointer_event){
        .kind = UI_POINTER_SCROLL,
        .pos = pos,
        .button = lines >= 0 ? UI_POINTER_WHEEL_BUTTON
                             : UI_POINTER_WHEEL_BUTTON + 1,
        .scroll_lines = lines,
        .scroll_delta = delta,
    }, window_frame_time_ms(test_window));
    assert(ui_surface_dispatch(surface, &event));
}

static void expect_arrows(char key, unsigned lines)
{
    char want[4096];
    assert(lines * 3 <= sizeof(want));
    for (unsigned i = 0; i < lines; i++) {
        want[i * 3] = '\033';
        want[i * 3 + 1] = '[';
        want[i * 3 + 2] = key;
    }
    expect_bytes(want, (size_t)lines * 3);
}

/* Mode 1007 sends a cursor key for each wheel line. A large line count is
 * split into chunks and is not truncated. */
int main(void)
{
    struct test_app app;
    test_app_init(&app, "/bin/cat", (struct vector2i){ .x = 800, .y = 480 });
    struct window *w = test_terminal_window(&app.app->host, &app.app->config.terminal);
    assert(w);
    test_window = w;
    ui_surface_set_scale(window_ui_surface(w), (struct css_scale_context){
        .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
    });
    ui_surface_resize(window_ui_surface(w), (struct vector2i){ .x = 800, .y = 480 });
    assert(terminal_new_tab(w));
    struct session *session = terminal_active_session(w);
    assert(session);

    assert(pipe(scroll_pipe) == 0);
    for (int i = 0; i < 2; i++)
        assert(fcntl(scroll_pipe[i], F_SETFL,
                     fcntl(scroll_pipe[i], F_GETFL) | O_NONBLOCK) == 0);
    assert(session_write_queue_bind(&session->input_writer, session->loop,
                                    scroll_pipe[1]));

    feed(session->term, "\033[?1049h\033[?1007h");

    scroll(30, 0.0f);
    expect_arrows('A', 30);
    scroll(-30, 0.0f);
    expect_arrows('B', 30);
    scroll(1000, 0.0f);
    expect_arrows('A', 1000);
    scroll(-1000, 0.0f);
    expect_arrows('B', 1000);

    scroll(0, 25.5f);
    expect_arrows('A', 25);
    scroll(0, 0.5f);
    expect_arrows('A', 1);
    scroll(0, -30.5f);
    expect_arrows('B', 30);
    scroll(0, -0.5f);
    expect_arrows('B', 1);

    feed(session->term, "\033[?1007l");
    scroll(30, 0.0f);
    expect_no_bytes();

    window_close(w);
    test_app_fini(&app);
    close(scroll_pipe[0]);
    close(scroll_pipe[1]);
    return 0;
}
