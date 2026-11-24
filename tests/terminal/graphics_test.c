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
        .graphics = source.graphics.above_text,
        .behind_text = source.graphics.behind_text,
        .behind_cells = source.graphics.behind_cells,
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
    struct wsap_vtcore_vector2i size = wsap_vtcore_size(term);
    char bottom[96];

    /* The first scrollback line can have ID zero. Scrolling away from
     * the live frame must not discard its placements. */
    feed(term, "\033[2;2H\033_Ga=T,f=24,s=1,v=1,C=1,q=2;/wAA\033\\"
               "\033[3;2H\033P7;1q#1;2;0;100;0!4~\033\\");
    snprintf(bottom, sizeof bottom, "\033[%d;1H\r\nline\r\nline\r\nline\r\n", size.y);
    feed(term, bottom);
    struct rect2i grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, ch) != 0xFFFF0000u);
    wsap_vtcore_scroll_to(term, 4);
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, ch) == 0xFFFF0000u);
    assert(pixel(&fb, grid, cw, 2 * ch) == 0xFF00FF00u);
    feed(term, "\033[3J");
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, ch) != 0xFFFF0000u);
    feed(term, "\033c");

    feed(term, "\033[2;2H\033_Ga=T,f=100,i=7,C=1,q=2;"
         "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAIAAACQd1PeAAAADElEQVR4nGP4z8AAAAMBAQDJ/pLvAAAAAElFTkSuQmCC\033\\"
         "\033[3;2H\033P7;1q#1;2;0;100;0!4~\033\\");
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, ch) == 0xFFFF0000u);
    assert(pixel(&fb, grid, cw + 3, 2 * ch + 5) == 0xFF00FF00u);
    assert(pixel(&fb, grid, cw + 4, 2 * ch + 5) != 0xFF00FF00u);

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
    snprintf(bottom, sizeof bottom, "\033[%d;2H\033P0;2q\"1;1;4;%d#1;2;0;100;0!4~\033\\",
             size.y, ch + 2);
    feed(term, bottom);
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, cw, (size.y - 3) * ch) == 0xFF00FF00u);
    assert(wsap_vtcore_cursor(term).pos.y == size.y - 1);

    /* Deep placements sit behind a colored background. A regular negative
     * placement covers the background but leaves the glyph visible. */
    feed(term, "\033c\033[?25l\033[38;2;0;0;255;48;2;0;255;0mX \033[0m"
               "\033[H\033_Ga=T,f=24,s=1,v=1,c=1,z=-1,C=1,q=2;/wAA\033\\"
               "\033[1;2H\033_Ga=T,f=24,s=1,v=1,c=1,z=-2147483648,C=1,q=2;/wAA\033\\");
    grid = paint(&app, terminal, &fb);
    int red = 0, ink = 0;
    for (int y = (ch - cw) / 2; y < (ch - cw) / 2 + cw; y++) {
        for (int x = 0; x < cw; x++) {
            uint32_t p = pixel(&fb, grid, x, y);
            red += p == 0xFFFF0000u;
            ink += (p & 255) > 0;
        }
    }
    assert(red > 0 && ink > 0);
    assert(pixel(&fb, grid, cw + cw / 2, ch / 2) == 0xFF00FF00u);

    /* Decompression must produce the exact advertised byte count. Erasing
     * just the middle cell must leave the two sides of the image visible. */
    feed(term, "\033c\033_Ga=T,f=24,s=3,v=1,o=z,c=3,C=1,q=2;eJz7z8DwH4wAEfcC/g==\033\\"
               "\033[1;2H\033[X"
               "\033[2;1H\033_Ga=T,f=100,o=z,S=69,C=1,q=2;"
               "eJzrDPBz5+WS4mJgYOD19HAJYmBgYARhDiYGBoYJ5cH3GBgYeDxdHEMq5iT/OH+AgYGZkZHh5L9J7xkYGBg8Xf1c1jklNAEAzm0QjQ==\033\\"
               "\033[3;1H\033_Ga=T,f=24,s=2,v=1,o=z,C=1,q=2;eJz7z8AAAAMAAQA=\033\\");
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, 0, 0) == 0xFFFF0000u);
    assert(pixel(&fb, grid, cw, 0) != 0xFFFF0000u);
    assert(pixel(&fb, grid, 2 * cw, 0) == 0xFFFF0000u);
    assert(pixel(&fb, grid, 0, ch) == 0xFFFF0000u);
    assert(pixel(&fb, grid, 0, 2 * ch) != 0xFFFF0000u);

    feed(term, "\033c\033[3;3H\033[?80h\033P7;1q#1;2;0;100;0!4~\033\\");
    grid = paint(&app, terminal, &fb);
    assert(pixel(&fb, grid, 0, 0) == 0xFF00FF00u);
    assert(wsap_vtcore_cursor(term).pos.y == 2 && wsap_vtcore_cursor(term).pos.x == 2);

    if (history.destroy)
        history.destroy(history.data);
    free(fb.pixels);
    window_close(win);
    test_app_fini(&app);
    return 0;
}
