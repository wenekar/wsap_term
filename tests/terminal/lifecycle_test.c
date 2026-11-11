#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "app/tabs/tabs.h"
#include "font/font.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"
#include "yasl/yasl.h"

static int reply_pipe[2];

static void feed(struct wsap_vtcore *term, const char *bytes)
{
    assert(wsap_vtcore_feed(term, bytes, strlen(bytes), 0));
}

static void bind_replies(struct session *session)
{
    assert(pipe(reply_pipe) == 0);
    for (int i = 0; i < 2; i++)
        assert(fcntl(reply_pipe[i], F_SETFL,
                     fcntl(reply_pipe[i], F_GETFL) | O_NONBLOCK) == 0);
    assert(session_write_queue_bind(&session->input_writer, session->loop,
                                    reply_pipe[1]));
}

static void expect_reply(const char *want)
{
    char got[128];
    ssize_t n = read(reply_pipe[0], got, sizeof(got));
    assert(n == (ssize_t)strlen(want));
    assert(!memcmp(got, want, (size_t)n));
    assert(read(reply_pipe[0], got, sizeof(got)) == -1 && errno == EAGAIN);
}

static void expect_no_reply(void)
{
    char got[1];
    assert(read(reply_pipe[0], got, sizeof(got)) == -1 && errno == EAGAIN);
}

/* CSI 14 t and CSI 16 t report the font cell size at each font change. */
static void cell_pixels_test(struct window *w, struct session *session)
{
    struct terminal *content = app_tabs_active(w);
    assert(content);
    const struct font_metrics *m = font_instance_metrics(terminal_font(content));
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(session->term);
    char want[64];

    snprintf(want, sizeof(want), "\033[6;%d;%dt", m->cell_h, m->cell_w);
    feed(session->term, "\033[16t");
    expect_reply(want);

    snprintf(want, sizeof(want), "\033[4;%d;%dt", size.y * m->cell_h,
             size.x * m->cell_w);
    feed(session->term, "\033[14t");
    expect_reply(want);

    terminal_adjust_font_size(content, 2);
    app_tabs_relayout(w);
    m = font_instance_metrics(terminal_font(content));
    size = wsap_vtcore_size(session->term);
    snprintf(want, sizeof(want), "\033[6;%d;%dt", m->cell_h, m->cell_w);
    feed(session->term, "\033[16t");
    expect_reply(want);
    snprintf(want, sizeof(want), "\033[4;%d;%dt", size.y * m->cell_h,
             size.x * m->cell_w);
    feed(session->term, "\033[14t");
    expect_reply(want);
}

/* A theme change reports one color scheme. A host preference wins over the
 * theme colors. */
static void color_scheme_test(struct window *w, struct session *session)
{
    struct wsap_vtcore_theme light, dark;
    wsap_vtcore_theme_default(&light);
    light.bg = 0xFFFFFFFFu;
    light.fg = 0xFF000000u;
    wsap_vtcore_theme_default(&dark);
    dark.bg = 0xFF000000u;
    dark.fg = 0xFFFFFFFFu;

    unsigned appearance = yasl_get_appearance();
    bool host_scheme = appearance & (YASL_APPEARANCE_DARK | YASL_APPEARANCE_LIGHT);
    int dark_report = appearance & YASL_APPEARANCE_DARK ? 1 : 2;
    char want[16];

    terminal_set_theme(app_tabs_active(w), &light);
    feed(session->term, "\033[?996n");
    snprintf(want, sizeof(want), "\033[?997;%dn", host_scheme ? dark_report : 2);
    expect_reply(want);
    terminal_set_theme(app_tabs_active(w), &dark);
    feed(session->term, "\033[?996n");
    snprintf(want, sizeof(want), "\033[?997;%dn", host_scheme ? dark_report : 1);
    expect_reply(want);

    feed(session->term, "\033[?2031h");
    terminal_set_theme(app_tabs_active(w), &light);
    if (host_scheme) {
        expect_no_reply();
    } else {
        expect_reply("\033[?997;2n");
        expect_no_reply();
    }
}

/* Text blink follows the app clock. The snapshot text does not change. */
static void text_blink_test(struct window *w, struct session *session)
{
    struct terminal *content = app_tabs_active(w);
    assert(content);
    feed(session->term, "\033[2J\033[H\033[5mB");
    struct terminal_frame_source source;

    terminal_prepare(content, 1000);
    assert(terminal_frame_source(content, 1000, &source));
    assert(source.text_blink_on);
    const struct wsap_vtcore_cell *cells =
        wsap_vtcore_snapshot_row(source.snapshot, 0);
    assert(cells && cells[0].text == 'B');

    terminal_prepare(content, 1500);
    assert(terminal_frame_source(content, 1500, &source));
    assert(!source.text_blink_on);
    cells = wsap_vtcore_snapshot_row(source.snapshot, 0);
    assert(cells && cells[0].text == 'B');

    terminal_prepare(content, 2000);
    assert(terminal_frame_source(content, 2000, &source));
    assert(source.text_blink_on);

    /* New cells without blink stop the text blink. The app clock is in the
     * off phase, so only a new snapshot scan reports a steady text. */
    feed(session->term, "\033[2J\033[H\033[25mX");
    terminal_prepare(content, 2500);
    assert(terminal_frame_source(content, 2500, &source));
    assert(source.text_blink_on);
    cells = wsap_vtcore_snapshot_row(source.snapshot, 0);
    assert(cells && cells[0].text == 'X');
}

/* A frame without terminal_prepare() updates the snapshot itself. */
static void frame_source_alone_test(struct window *w, struct session *session)
{
    feed(session->term, "\033[2J\033[Hplain");
    struct terminal_frame_source source;
    struct terminal *content = app_tabs_active(w);
    assert(content);
    assert(terminal_frame_source(content, 3000, &source));
    const struct wsap_vtcore_cell *cells =
        wsap_vtcore_snapshot_row(source.snapshot, 0);
    assert(cells && cells[0].text == 'p');
}

int main(void)
{
    struct test_app app;
    test_app_init(&app, "/bin/cat", (struct vector2i){ .x = 800, .y = 480 });
    struct window *w = test_terminal_window(app.app);
    assert(w && app_tabs_new(w));
    struct session *session = terminal_session(app_tabs_active(w));
    assert(session);
    bind_replies(session);

    cell_pixels_test(w, session);
    color_scheme_test(w, session);
    text_blink_test(w, session);
    frame_source_alone_test(w, session);

    window_close(w);
    test_app_fini(&app);
    close(reply_pipe[0]);
    close(reply_pipe[1]);
    return 0;
}
