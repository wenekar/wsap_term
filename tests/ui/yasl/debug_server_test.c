#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "ui/yasl/client.h"
#include "ui/yasl/testing.h"
#include "yasl/yasl.h"
#include "util/time.h"
#include "ui/yasl/inspector.h"
#include "terminal/internal.h"

struct client {
    struct yasl_loop *loop;
    int fd;
    char buf[1 << 16];
    size_t len;
    int want_lines;
    uint64_t deadline_ns;
};

static int count_lines(const struct client *c)
{
    int n = 0;
    for (size_t i = 0; i < c->len; i++)
        n += c->buf[i] == '\n';
    return n;
}

static uint64_t client_poll(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    struct client *c = ud;
    for (;;) {
        ssize_t n = recv(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len,
                         MSG_DONTWAIT);
        if (n <= 0)
            break;
        c->len += (size_t)n;
        c->buf[c->len] = '\0';
    }
    if (count_lines(c) >= c->want_lines || yasl_time_ns() >= c->deadline_ns)
        yasl_loop_quit(c->loop);
    else
        assert(yasl_loop_add_timer(c->loop, MS_TO_NS(2), client_poll, c) != 0);
    return 0;
}

static void exchange(struct client *c, const char *lines, int replies)
{
    assert(send(c->fd, lines, strlen(lines), 0) == (ssize_t)strlen(lines));
    c->want_lines = count_lines(c) + replies;
    c->deadline_ns = yasl_time_ns() + 2000000000ull;
    assert(yasl_loop_add_timer(c->loop, MS_TO_NS(1), client_poll, c) != 0);
    assert(yasl_loop_run(c->loop) == 0);
    assert(count_lines(c) == c->want_lines);
}

static const char *line_at(const struct client *c, int index)
{
    const char *p = c->buf;
    while (index-- > 0) {
        p = strchr(p, '\n');
        assert(p);
        p++;
    }
    return p;
}

static char font_asked[64];

static bool record_option(void *ud, const char *key, const char *value)
{
    (void)ud;
    if (strcmp(key, "font") != 0)
        return false;
    snprintf(font_asked, sizeof(font_asked), "%s", value);
    return true;
}

static uint64_t mark_fired(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    bool *fired = ud;
    *fired = true;
    return 0;
}

static uint64_t quit_loop(void *ud, uint64_t id, uint64_t interval_ns)
{
    (void)id;
    (void)interval_ns;
    yasl_loop_quit(ud);
    return 0;
}

static void capture_survives_next_build_test(void)
{
    struct test_app ta;
    test_app_init(&ta, "/bin/cat", (struct vector2i){ .x = 800, .y = 600 });
    struct window *w = test_terminal_window(&ta.app->host, &ta.app->config.terminal);
    assert(w && terminal_new_tab(w));
    ui_surface_set_scale(window_ui_surface(w), (struct css_scale_context){
                          .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
                      });
    ui_surface_resize(window_ui_surface(w), (struct vector2i){ .x = 800, .y = 600 });
    window_debug_toggle_overlay(w);

    struct ui_scene *scene = window_ui_scene_scratch(w);
    assert(ui_surface_build_scene(window_ui_surface(w), scene));
    window_debug_end_scene(w, WINDOW_DEBUG_MAIN, scene);
    char *before = window_debug_eval("scene");
    assert(before && strstr(before, "\"ok\":true"));

    scene = window_ui_scene_scratch(w);
    for (int i = 0; i < 4096; i++)
        assert(ui_scene_rect(scene, (struct rect2i){ .size = { .x = i + 1, .y = 1 } },
                             0xFF00FF00u));
    char *after = window_debug_eval("scene");
    assert(after && !strcmp(before, after));
    free(before);
    free(after);

    window_close(w);
    test_app_fini(&ta);
}

int main(void)
{
    capture_survives_next_build_test();
    char dir[] = "/tmp/wsap-debug-test-XXXXXX";
    assert(mkdtemp(dir));
    char path[128];
    snprintf(path, sizeof(path), "%s/sock", dir);

    struct test_app ta;
    test_app_init(&ta, "/bin/cat", (struct vector2i){ .x = 800, .y = 600 });
    struct yasl_loop *loop = ta.app->loop;
    struct window_host *windows = &ta.app->host;
    struct window *w = test_terminal_window(&ta.app->host, &ta.app->config.terminal);
    assert(w && terminal_new_tab(w) && terminal_new_tab(w));
    ui_surface_set_scale(window_ui_surface(w), (struct css_scale_context){
                          .dpi_scale = 1.0f, .cell_w = 8.0f, .cell_h = 16.0f,
                      });
    ui_surface_resize(window_ui_surface(w), (struct vector2i){ .x = 800, .y = 600 });
    ui_surface_tick(window_ui_surface(w), window_frame_time_ms(w) + 100000);

    window_debug_set_hooks(&(struct window_debug_hooks){ .option_set = record_option });
    assert(window_debug_server_start(loop, windows, path));
    struct stat st;
    assert(stat(path, &st) == 0 && S_ISSOCK(st.st_mode));
    assert((st.st_mode & 0077) == 0);
    assert(!window_debug_server_start(loop, windows, path));

    static struct client c;
    c.loop = loop;
    c.fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    assert(connect(c.fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    exchange(&c, "windows\ntree --depth 2\nbogus\nwidget tabbar\n", 4);
    assert(strstr(line_at(&c, 0), "{\"ok\":true,\"result\":[{\"index\":0,"));
    assert(strstr(line_at(&c, 0), "\"tabs\":2"));
    const char *tree = line_at(&c, 1);
    assert(strstr(tree, "\"type\":\"window-root\""));
    assert(strstr(tree, "\"type\":\"terminal-root\""));
    assert(strstr(tree, "\"type\":\"tabbar\""));
    assert(strstr(tree, "\"type\":\"scrollview\""));
    assert(!strncmp(line_at(&c, 2), "{\"ok\":false,\"error\":\"unknown command", 36));
    assert(strstr(line_at(&c, 3), "\"name\":\"tab\",\"index\":1"));

    exchange(&c, "screenshot /nonexistent.png\n", 1);
    assert(!strncmp(line_at(&c, 4), "{\"ok\":false,", 12));

    exchange(&c, "text a b\n", 1);
    assert(!strncmp(line_at(&c, 5), "{\"ok\":true,\"result\":3}", 22));

    exchange(&c, "font Mono\n", 1);
    assert(!strncmp(line_at(&c, 6), "{\"ok\":true,", 11));
    assert(!strcmp(font_asked, "Mono"));

    close(c.fd);
    bool fired = false;
    assert(yasl_loop_add_timer(loop, MS_TO_NS(20), mark_fired, &fired) != 0);
    assert(yasl_loop_add_timer(loop, MS_TO_NS(40), quit_loop, loop) != 0);
    assert(yasl_loop_run(loop) == 0);
    assert(fired);

    window_debug_server_stop();
    assert(stat(path, &st) < 0 && errno == ENOENT);
    rmdir(dir);

    char *reply = window_debug_eval("tree --depth 0");
    assert(reply && strstr(reply, "\"ok\":true") &&
           strstr(reply, "\"type\":\"window-root\""));
    free(reply);
    reply = window_debug_eval("-p x tree");
    assert(reply && !strncmp(reply, "{\"ok\":false,", 12));
    free(reply);
    reply = window_debug_eval("key a");
    assert(reply && !strncmp(reply, "{\"ok\":false,", 12));
    free(reply);
    reply = window_debug_eval("font Sans");
    assert(reply && !strncmp(reply, "{\"ok\":true,", 11));
    assert(!strcmp(font_asked, "Sans"));
    free(reply);

    window_close(w);
    test_app_fini(&ta);
    puts("PASS: debug server");
    return 0;
}
