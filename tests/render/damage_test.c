#define _POSIX_C_SOURCE 200809L
/*
 * Tests of the damage that render_draw() returns.
 *
 * Each case draws real terminal frames into a persistent canvas and
 * checks the exact damage rects. Every case also checks that the damage
 * covers every pixel that changed, and that the canvas equals a fresh
 * paint of the frame.
 */
#include "helpers/render_draw.h"

static struct font_instance *font;
static struct renderer *rend, *oracle;
static int cw, ch;

/* The grid sits inside the canvas, so that a band and the canvas differ. */
#define GRID_X 6
#define GRID_Y 4
#define COLS 30
#define ROWS 10

struct fixture {
    struct tterm tt;
    struct tcanvas canvas;
    struct ui_scene scene;
    struct ui_damage ui_damage;
    bool use_ui_damage;
    bool cursor;
    uint32_t *before, *want;
};

static struct vector2i canvas_size(void)
{
    return (struct vector2i){ .x = GRID_X + COLS * cw + 9, .y = GRID_Y + ROWS * ch + 7 };
}

static struct rect2i grid_rect(void)
{
    struct vector2i c = canvas_size();
    return trect(GRID_X, GRID_Y, c.x - GRID_X - 3, c.y - GRID_Y - 2);
}

/* The canvas band of grid rows [r0, r1). */
static struct rect2i band(int r0, int r1)
{
    struct rect2i g = grid_rect();
    return trect(g.x, g.y + r0 * ch, g.width, (r1 - r0) * ch);
}

static void fx_open(struct fixture *f)
{
    *f = (struct fixture){ .use_ui_damage = true };
    tterm_open(&f->tt, COLS, ROWS);
    for (int i = 0; i < ROWS; i++) {
        char line[64];
        snprintf(line, sizeof(line), "line %d: \033[3mWf\033[0m -> \033[38;5;%dmcolor\033[0m%s", i,
                 100 + i, i + 1 < ROWS ? "\r\n" : "");
        tterm_feed(&f->tt, line);
    }
    struct vector2i c = canvas_size();
    tcanvas_init(&f->canvas, c.x, c.y);
    ui_scene_init(&f->scene);
    f->before = malloc(tcanvas_px(&f->canvas) * sizeof(uint32_t));
    f->want = malloc(tcanvas_px(&f->canvas) * sizeof(uint32_t));
    TCHECK(f->before && f->want, "buffers");
}

static void fx_close(struct fixture *f)
{
    free(f->before);
    free(f->want);
    ui_scene_destroy(&f->scene);
    tcanvas_free(&f->canvas);
    tterm_close(&f->tt);
}

/* Draws one frame and checks the pixels and the damage coverage. */
static struct damage_list fx_draw(struct fixture *f, const char *what)
{
    memcpy(f->before, f->canvas.px, tcanvas_px(&f->canvas) * sizeof(uint32_t));
    const struct terminal_frame *tf = tterm_frame(&f->tt, f->cursor);
    struct render_frame fr = {
        .base = 0xFF282A36u,
        .grid = { .frame = tf, .font = font, .rect = grid_rect() },
        .ui = &f->scene,
        .ui_font = font,
        .ui_damage = f->use_ui_damage ? &f->ui_damage : NULL,
    };
    struct render_result res = tdraw(rend, &f->canvas, &fr);
    tdraw_fresh(oracle, f->want, f->canvas.size, &fr);
    tcheck_same(what, f->canvas.px, f->want, f->canvas.size);
    tcheck_damage(what, f->before, f->canvas.px, f->canvas.size, &res.damage);
    TCHECK(res.damage.n >= 0 && res.damage.n <= DAMAGE_RECTS_MAX, "%s: rect count %d", what,
           res.damage.n);
    f->ui_damage = (struct ui_damage){0};
    return res.damage;
}

static void check_rects(const char *what, const struct damage_list *d, const struct rect2i *want,
                        int n)
{
    TCHECK(d->any && !d->all, "%s: any %d all %d", what, d->any, d->all);
    if (d->n != n)
        for (int i = 0; i < d->n; i++)
            fprintf(stderr, "%s: rect %d: %d,%d %dx%d (row %d..%d)\n", what, i, d->rects[i].x,
                    d->rects[i].y, d->rects[i].width, d->rects[i].height,
                    (d->rects[i].y - GRID_Y) / ch, (d->rects[i].y + d->rects[i].height - GRID_Y) / ch);
    TCHECK(d->n == n, "%s: %d rects, want %d", what, d->n, n);
    for (int i = 0; i < n; i++)
        TCHECK(rect2i_eq(d->rects[i], want[i]),
               "%s: rect %d is %d,%d %dx%d, want %d,%d %dx%d", what, i, d->rects[i].x,
               d->rects[i].y, d->rects[i].width, d->rects[i].height, want[i].x, want[i].y,
               want[i].width, want[i].height);
}

static void check_all(const char *what, const struct damage_list *d)
{
    struct vector2i c = canvas_size();
    TCHECK(d->any && d->all, "%s: any %d all %d", what, d->any, d->all);
    TCHECK(d->n == 1 && rect2i_eq(d->rects[0], trect(0, 0, c.x, c.y)),
           "%s: full damage must hold the canvas rect", what);
}

static void test_first_and_idle(void)
{
    struct fixture f;
    fx_open(&f);
    struct damage_list d = fx_draw(&f, "first frame");
    check_all("first frame", &d);
    for (int i = 0; i < 5; i++) {
        d = fx_draw(&f, "idle");
        TCHECK(!d.any && !d.n, "an idle frame damages (%d rects)", d.n);
    }
    /* A scene that does not change is still idle. */
    TCHECK(ui_scene_rect(&f.scene, trect(20, 10, 40, 30), 0x80FFFFFFu), "rect");
    ui_damage_add(&f.ui_damage, trect(20, 10, 40, 30));
    d = fx_draw(&f, "overlay appears");
    for (int i = 0; i < 3; i++) {
        d = fx_draw(&f, "idle with overlay");
        TCHECK(!d.any, "an idle frame with a scene damages");
    }
    fx_close(&f);
}

static void test_typing(void)
{
    struct fixture f;
    fx_open(&f);
    f.cursor = true;
    tterm_feed(&f.tt, "\033[4;3H");
    fx_draw(&f, "setup");
    for (int i = 0; i < 6; i++) {
        tterm_feed(&f.tt, i % 2 ? "x" : "\033[3mW\033[23m");
        struct damage_list d = fx_draw(&f, "typing");
        struct rect2i want = band(3, 4);
        check_rects("typing", &d, &want, 1);
    }
    /* The cursor moves to another row: both rows change. */
    tterm_feed(&f.tt, "\033[8;1H");
    struct damage_list d = fx_draw(&f, "cursor move");
    struct rect2i want[2] = { band(3, 4), band(7, 8) };
    check_rects("cursor move", &d, want, 2);
    /* A hidden cursor changes its row only. */
    f.cursor = false;
    d = fx_draw(&f, "cursor hide");
    check_rects("cursor hide", &d, &want[1], 1);
    fx_close(&f);
}

static void test_scroll(void)
{
    struct fixture f;
    fx_open(&f);
    tterm_feed(&f.tt, "\033[10;1H");
    fx_draw(&f, "setup");
    /* A full scroll moves every row: all rows change. */
    tterm_feed(&f.tt, "\r\nnew line\033[3m W\033[0m");
    struct damage_list d = fx_draw(&f, "scroll");
    struct rect2i rows = band(0, ROWS);
    check_rects("scroll", &d, &rows, 1);

    /* A scroll region changes only its own rows. The term touches the rows
     * that the cursor visits, so the cursor moves in a frame of its own. */
    tterm_feed(&f.tt, "\033[3;7r\033[7;1H");
    fx_draw(&f, "region setup");
    tterm_feed(&f.tt, "\r\nregion line");
    d = fx_draw(&f, "region scroll");
    struct rect2i region = band(2, 7);
    check_rects("region scroll", &d, &region, 1);

    /* Delete one line in the middle: the rows from there to the bottom of
     * the region change. */
    tterm_feed(&f.tt, "\033[r\033[5;1H");
    fx_draw(&f, "delete setup");
    tterm_feed(&f.tt, "\033[M");
    d = fx_draw(&f, "delete line");
    struct rect2i tail = band(4, ROWS);
    check_rects("delete line", &d, &tail, 1);
    fx_close(&f);
}

static void test_overlay(void)
{
    struct fixture f;
    fx_open(&f);
    fx_draw(&f, "setup");
    struct rect2i a = trect(30, 15, 50, 2 * ch + 3);
    TCHECK(ui_scene_blend_rect(&f.scene, a, 0xFF3060C0u, 140), "rect");
    ui_damage_add(&f.ui_damage, a);
    struct damage_list d = fx_draw(&f, "overlay appears");
    check_rects("overlay appears", &d, &a, 1);

    /* It moves: the old and the new rect. */
    struct rect2i b = trect(a.x + 3 * cw + 1, a.y + 2 * ch, a.width, a.height);
    ui_scene_destroy(&f.scene);
    ui_scene_init(&f.scene);
    TCHECK(ui_scene_blend_rect(&f.scene, b, 0xFF3060C0u, 140), "rect");
    ui_damage_add(&f.ui_damage, a);
    ui_damage_add(&f.ui_damage, b);
    d = fx_draw(&f, "overlay moves");
    TCHECK(d.any && !d.all, "overlay moves: any %d all %d", d.any, d.all);
    struct rect2i both = rect2i_union(a, b);
    TCHECK(rect2i_eq(d.bounds, both), "overlay moves: bounds");

    /* It vanishes: its rect, and the pixels under it equal the grid again. */
    ui_scene_destroy(&f.scene);
    ui_scene_init(&f.scene);
    ui_damage_add(&f.ui_damage, b);
    d = fx_draw(&f, "overlay vanishes");
    check_rects("overlay vanishes", &d, &b, 1);

    /* An overlay outside the grid, over the base. */
    struct rect2i c = trect(0, 0, GRID_X + 4, GRID_Y + 3);
    TCHECK(ui_scene_rect(&f.scene, c, 0xFFE0E040u), "rect");
    ui_damage_add(&f.ui_damage, c);
    d = fx_draw(&f, "base overlay");
    check_rects("base overlay", &d, &c, 1);
    ui_scene_destroy(&f.scene);
    ui_scene_init(&f.scene);
    ui_damage_add(&f.ui_damage, c);
    d = fx_draw(&f, "base overlay vanishes");
    check_rects("base overlay vanishes", &d, &c, 1);

    /* No ui_damage: all of the UI changed. */
    f.use_ui_damage = false;
    d = fx_draw(&f, "null ui damage");
    struct vector2i size = canvas_size();
    struct rect2i canvas = trect(0, 0, size.x, size.y);
    check_rects("null ui damage", &d, &canvas, 1);
    fx_close(&f);
}

static void test_full(void)
{
    struct fixture f;
    fx_open(&f);
    fx_draw(&f, "setup");
    fx_draw(&f, "idle");

    f.canvas.fresh = true;
    struct damage_list d = fx_draw(&f, "fresh");
    check_all("fresh", &d);

    const struct terminal_frame *tf = tterm_frame(&f.tt, false);
    struct render_frame fr = tframe(tf, font, f.canvas.size);
    fr.grid.rect = grid_rect();
    fr.base = 0xFF282A36u;
    fr.ui = &f.scene;
    fr.ui_damage = &f.ui_damage;
    fr.repaint = true;
    struct render_result res = tdraw(rend, &f.canvas, &fr);
    check_all("repaint", &res.damage);

    /* A palette change repaints everything. */
    tterm_feed(&f.tt, "\033]11;rgb:10/20/30\033\\");
    d = fx_draw(&f, "palette");
    check_all("palette", &d);

    /* No history: every frame is full. */
    struct framebuffer fb = { .pixels = f.canvas.px, .size = f.canvas.size };
    fr.repaint = false;
    res = render_draw(rend, &fb, &fr);
    check_all("no history", &res.damage);

    /* No pixels: full damage, and the next frame with pixels is full too. */
    struct framebuffer empty = { .size = f.canvas.size, .history = &f.canvas.history };
    res = render_draw(rend, &empty, &fr);
    TCHECK(res.damage.any && res.damage.all, "no pixels: full damage");
    d = fx_draw(&f, "after no pixels");
    check_all("after no pixels", &d);
    fx_close(&f);
}

int main(void)
{
    TCHECK(render_init(), "render_init");
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    font = font_instance_default();
    TCHECK(font, "font");
    cw = font_instance_metrics(font)->cell_w;
    ch = font_instance_metrics(font)->cell_h;
    rend = renderer_create(NULL);
    oracle = renderer_create(&(struct render_options){
        .threads = RENDER_NONE, .cache_bytes = RENDER_NONE });
    TCHECK(rend && oracle, "renderers");
    test_first_and_idle();
    test_typing();
    test_scroll();
    test_overlay();
    test_full();
    renderer_destroy(rend);
    renderer_destroy(oracle);
    render_shutdown();
    puts("PASS: damage");
    return 0;
}
