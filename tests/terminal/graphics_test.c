#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/tabs/tabs.h"
#include "font/font.h"
#include "helpers/app_fixture.h"
#include "helpers/terminal_window.h"
#include "session/session.h"
#include "terminal/render/term_frame.h"
#include "terminal/render/term_render.h"
#include "terminal/terminal.h"
#include "ui/surface.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"

static void feed(struct wsap_vtcore *term, const char *s)
{
    assert(wsap_vtcore_feed(term, s, strlen(s), 0));
}

static struct rect2i paint(struct test_app *app, struct terminal *terminal,
                            struct framebuffer *fb)
{
    struct terminal_frame_source source;
    assert(terminal_frame_source(terminal, 0, &source));
    struct terminal_frame grid;
    terminal_frame_init(&grid, source.snapshot, false);
    render_draw(app->app->renderer, fb, &(struct render_frame){
        .grid = {.frame = &grid, .font = source.font, .rect = source.rect},
        .graphics = source.graphics,
    });
    return source.rect;
}

static uint32_t pixel(const struct framebuffer *fb, struct rect2i grid, int x, int y)
{
    struct vector2i pos = {.x = grid.x + x, .y = grid.y + y};
    assert(pos.x >= 0 && pos.x < fb->size.x && pos.y >= 0 && pos.y < fb->size.y);
    return fb->pixels[(size_t)pos.y * (size_t)fb->size.x + (size_t)pos.x];
}

int main(void)
{
    const struct vector2i canvas = {.x = 800, .y = 480};
    struct test_app app;
    test_app_init(&app, "/bin/cat", canvas);
    struct window *win = test_terminal_window(app.app);
    assert(win && app_tabs_new(win));
    ui_surface_resize(window_ui_surface(win), canvas);
    ui_surface_prepare(window_ui_surface(win));
    struct terminal *terminal = app_tabs_active(win);
    terminal_commit_geometry(terminal);
    struct wsap_vtcore *term = terminal_session(terminal)->term;
    const struct font_metrics *font = font_instance_metrics(terminal_font(terminal));
    int cw = font->cell_w, ch = font->cell_h;
    struct framebuffer_history history = {0};
    struct framebuffer fb = {
        .size = canvas, .history = &history,
        .pixels = calloc((size_t)canvas.x * (size_t)canvas.y, sizeof(uint32_t)),
    };
    assert(fb.pixels);

    feed(term, "\033[2;2H\033_Ga=T,f=100,i=7,C=1,q=2;"
         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC\033\\"
         "\033[3;2H\033P7;1q#1;2;0;100;0!4~\033\\");
    struct rect2i grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, ch) == 0xFFFF0000u);
    assert(pixel(&fb, grid, cw + 3, 2 * ch + 5) == 0xFF00FF00u);
    assert(pixel(&fb, grid, cw + 4, 2 * ch + 5) != 0xFF00FF00u);

    struct wsap_vtcore_vector2i size = wsap_vtcore_size(term);
    session_resize(terminal_session(terminal), (struct vector2i){.x = size.x - 1, .y = size.y});
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, ch) == 0xFFFF0000u);
    assert(pixel(&fb, grid, cw, 2 * ch) == 0xFF00FF00u);

    /* Scrolling carries both images with their rows. The alternate screen
     * hides them without touching the normal screen's images. */
    feed(term, "\033[1S\033[?1049h");
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, 0) != 0xFFFF0000u);
    feed(term, "\033[?1049l");
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, 0) == 0xFFFF0000u);
    assert(pixel(&fb, grid, cw, ch) == 0xFF00FF00u);

    feed(term, "\033_Ga=d,d=I,i=7,q=2\033\\");
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, 0) != 0xFFFF0000u);
    assert(pixel(&fb, grid, cw, ch) == 0xFF00FF00u);
    feed(term, "\033[2J");
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, ch) != 0xFF00FF00u);

    /* A bitmap sent at the bottom makes room for its full height. */
    char bottom[96];
    snprintf(bottom, sizeof bottom, "\033[%d;2H\033P0;2q\"1;1;4;%d#1;2;0;100;0!4~\033\\",
             size.y, ch + 2);
    feed(term, bottom);
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, (size.y - 3) * ch) == 0xFF00FF00u);
    assert(wsap_vtcore_cursor(term).pos.y == size.y - 1);

    if (history.destroy)
        history.destroy(history.data);
    free(fb.pixels);
    window_close(win);
    test_app_fini(&app);
    return 0;
}
