#define _POSIX_C_SOURCE 200809L
/*
 * Incremental equals fresh.
 *
 * A seeded random walk drives real terminals and a UI scene through
 * render_draw() into one persistent canvas. After each step:
 * - the canvas must equal a fresh paint of the same frame by a second
 *   renderer that has no cell cache and no workers;
 * - the returned damage must cover every pixel that changed since the
 *   previous step;
 * - a step that changed nothing must report no damage.
 *
 * The walk covers text (ASCII, CJK, emoji, combining marks, clusters,
 * ligatures, icons, right-to-left text), SGR with every underline style, faint, colors, scroll,
 * scroll regions, line and character edits, clear screen (ED 2), selection, cursor styles and
 * visibility, DECSCNM, palette changes, the scrollback view, the alternate
 * screen, terminal and canvas resizes, a font change, a tab switch between
 * two terminals that draw into one canvas, UI overlays that appear, move,
 * and vanish, a scrollbar overlay during scroll, a moved grid rect, and
 * fresh and repaint frames.
 *
 * The whole walk runs three times: with the scroll method that the
 * renderer picks from cost (WSAP_RENDER_SCROLL=auto), with every scroll
 * moved, and with every scroll painted. Pixels and damage must match in
 * all three.
 *
 * INCREMENTAL_STEPS=<n> sets the steps of each seed.
 */
#include "helpers/render_draw.h"

#include <time.h>

static uint64_t rng_state;

static uint32_t rnd(void)
{
    uint64_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    rng_state = x;
    return (uint32_t)(x >> 16);
}

static int rnd_n(int n)
{
    return n > 0 ? (int)(rnd() % (uint32_t)n) : 0;
}

static bool rnd_p(int percent)
{
    return rnd_n(100) < percent;
}

/* ---- The UI overlays -------------------------------------------------- */

enum { OV_RANDOM = 3, OV_SCROLLBAR = OV_RANDOM, OV_COUNT };

enum ov_kind { OV_RECT, OV_ROUNDED, OV_BLEND, OV_TEXT, OV_CLIPPED_TEXT, OV_KINDS };

struct overlay {
    bool on;
    enum ov_kind kind;
    struct rect2i rect;
    uint32_t color;
    const char *text;
    struct rect2i ink;
};

struct world {
    struct tterm term[2];
    int active;
    bool cursor;
    struct font_instance *fonts[2];
    int font;
    struct font_instance *ui_font;
    struct tcanvas canvas;
    struct rect2i grid;
    struct overlay ov[OV_COUNT];
    bool ov_changed[OV_COUNT];
    struct ui_scene scene;
    struct ui_damage ui_damage;
    bool repaint;
    int max_cols, max_rows;
    int steps;
};

static void scene_build(struct world *w)
{
    ui_scene_destroy(&w->scene);
    ui_scene_init(&w->scene);
    const struct font_metrics *m = font_instance_metrics(w->ui_font);
    for (int i = 0; i < OV_COUNT; i++) {
        struct overlay *o = &w->ov[i];
        if (!o->on)
            continue;
        struct ui_scene *s = &w->scene;
        int before = s->n_ops;
        switch (o->kind) {
        case OV_RECT:
            TCHECK(ui_scene_rect(s, o->rect, o->color), "rect");
            break;
        case OV_ROUNDED:
            TCHECK(ui_scene_rounded_rect(s, o->rect, 4, o->color), "rounded rect");
            break;
        case OV_BLEND:
            TCHECK(ui_scene_blend_rect(s, o->rect, o->color, 90), "blend rect");
            break;
        case OV_TEXT:
        case OV_CLIPPED_TEXT: {
            struct ui_text_style st = { .advance = m->cell_w, .cell_h = m->cell_h };
            if (o->kind == OV_CLIPPED_TEXT)
                TCHECK(ui_scene_push_clip(s, trect(o->rect.x, o->rect.y, o->rect.width / 2,
                                                   o->rect.height)), "clip");
            TCHECK(ui_scene_text_run(s, o->rect, st, o->color, o->text, strlen(o->text)),
                   "text");
            if (o->kind == OV_CLIPPED_TEXT)
                TCHECK(ui_scene_pop_clip(s), "pop");
            break;
        }
        default:
            break;
        }
        /* The ink of the overlay: the union of its ops, clipped. */
        o->ink = (struct rect2i){0};
        for (int k = before; k < s->n_ops; k++) {
            const struct ui_scene_op *op = &s->ops[k];
            if (op->kind == UI_SCENE_OP_PUSH_CLIP || op->kind == UI_SCENE_OP_POP_CLIP)
                continue;
            struct rect2i ink = op->kind == UI_SCENE_OP_TEXT ? ui_scene_text_ink(s, op) : op->rect;
            o->ink = rect2i_union(o->ink, ink);
        }
    }
    TCHECK(ui_scene_is_valid(&w->scene), "scene");
}

/* Records that overlay i changes: its old ink and, after the change, its new ink. */
static void ov_damage(struct world *w, int i)
{
    if (w->ov[i].on && !rect2i_is_empty(w->ov[i].ink))
        ui_damage_add(&w->ui_damage, w->ov[i].ink);
    w->ov_changed[i] = true;
}

static void ov_random(struct world *w, struct overlay *o)
{
    static const char *const texts[] = {
        "Find: needle", "tab 1", "⚠ warning", "cpu 42%", "->=>!=", "日本語メニュー",
    };
    struct vector2i c = w->canvas.size;
    o->on = true;
    o->kind = (enum ov_kind)rnd_n(OV_KINDS);
    /* Narrow rects put window edges next to spilling cells. */
    int wd = rnd_p(40) ? 2 + rnd_n(14) : 8 + rnd_n(c.x / 2), ht = 6 + rnd_n(c.y / 3);
    o->rect = trect(rnd_n(c.x) - 10, rnd_n(c.y) - 6, wd, ht);
    uint32_t alpha = rnd_p(50) ? 0xFF000000u : (uint32_t)(0x40 + rnd_n(0xA0)) << 24;
    o->color = alpha | (rnd() & 0xFFFFFFu);
    o->text = texts[rnd_n((int)(sizeof(texts) / sizeof(texts[0])))];
}

/* A scrollbar thumb at the right edge of the grid, placed by the view offset. */
static void ov_scrollbar(struct world *w)
{
    struct wsap_vtcore *t = w->term[w->active].t;
    int total = tt_history(t), offset = tt_view_offset(t);
    struct rect2i g = w->grid;
    int track = g.height > 8 ? g.height - 8 : 1;
    int thumb = track / 3 + 4;
    int y = total > 0 ? (track - thumb) * (total - offset) / total : track - thumb;
    struct overlay *o = &w->ov[OV_SCROLLBAR];
    o->on = true;
    o->kind = OV_ROUNDED;
    o->rect = trect(g.x + g.width - 8, g.y + 4 + y, 5, thumb);
    o->color = 0x80A6ADC8u;
}

/* ---- Terminal input ---------------------------------------------------- */

static void feedf(struct world *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#include <stdarg.h>
static void feedf(struct world *w, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    tterm_feed(&w->term[w->active], buf);
}

static void op_ascii(struct world *w)
{
    char buf[200];
    int n = 1 + rnd_n(120);
    for (int i = 0; i < n; i++) {
        int r = rnd_n(100);
        buf[i] = r < 12 ? ' ' : r < 14 && i ? '\n' : r < 15 && i ? '\r' : (char)('!' + rnd_n(94));
    }
    buf[n] = 0;
    feedf(w, "%s", buf);
}

static void op_unicode(struct world *w)
{
    static const char *const tokens[] = {
        "日本語のテキスト", "漢字", "한국어", "中文字符", "🙂", "🚀", "👍🏽", "🇺🇸", "👨‍👩‍👧",
        "e\xcc\x81", "a\xcc\x88o\xcc\x88", "fn main() -> Result<(), E> { x != y && a <= b }",
        "=> >= == === !== ::", "─│┌┐└┘├┤", "\xef\x84\x95", "\xee\x82\xb0\xee\x82\xb2",
        "WAVATAYA7A%A", "ﬁ ﬂ", "Ω≈ç√∫", "\xe2\x97\x8f\xe2\x97\x8f", " ", "\t", "x\r\n",
        "سلام", "مرحبا بالعالم", "שלום", "abc سلام 123 def", "\xd9\x84\xd8\xa7",
    };
    int n = 1 + rnd_n(6);
    for (int i = 0; i < n; i++)
        feedf(w, "%s", tokens[rnd_n((int)(sizeof(tokens) / sizeof(tokens[0])))]);
}

static void op_sgr(struct world *w)
{
    switch (rnd_n(16)) {
    case 0: feedf(w, "\033[0m"); break;
    case 1: feedf(w, "\033[1m"); break;
    case 2: feedf(w, "\033[3m"); break;
    case 3: feedf(w, "\033[2m"); break;
    case 4: feedf(w, "\033[4:%dm", rnd_n(6)); break;
    case 5: feedf(w, "\033[9m"); break;
    case 6: feedf(w, "\033[53m"); break;
    case 7: feedf(w, "\033[7m"); break;
    case 8: feedf(w, "\033[38;5;%dm", rnd_n(256)); break;
    case 9: feedf(w, "\033[48;5;%dm", rnd_n(256)); break;
    case 10: feedf(w, "\033[38;2;%d;%d;%dm", rnd_n(256), rnd_n(256), rnd_n(256)); break;
    case 11: feedf(w, "\033[48;2;%d;%d;%dm", rnd_n(256), rnd_n(256), rnd_n(256)); break;
    case 12: feedf(w, "\033[39;49m"); break;
    case 13: feedf(w, "\033[22;23;24;29;55;27m"); break;
    case 14: feedf(w, "\033]8;;https://example.com/%d\033\\link%d\033]8;;\033\\", rnd_n(9), rnd_n(9)); break;
    default: feedf(w, "\033[4m\033[3mitalic under\033[0m"); break;
    }
    op_ascii(w);
}

/* Fills every row with text, so that moved rows differ from each other. */
static void op_fill(struct world *w)
{
    struct wsap_vtcore *t = w->term[w->active].t;
    int rows = tt_rows(t), cols = tt_cols(t);
    feedf(w, "\033[H");
    for (int y = 0; y < rows; y++) {
        int n = rnd_n(cols + 1);
        char line[256];
        int len = 0;
        for (int i = 0; i < n && len < (int)sizeof(line) - 1; i++)
            line[len++] = (char)('!' + (y * 7 + i * 3 + rnd_n(3)) % 94);
        line[len] = 0;
        feedf(w, "\033[%dm%s\033[0m\033[K%s", rnd_p(30) ? 3 : 0, line, y + 1 < rows ? "\r\n" : "");
    }
    feedf(w, "\033[%d;%dH", 1 + rnd_n(rows), 1 + rnd_n(cols));
}

/* Scrolls part of the screen with the cursor in the middle of it. */
static void op_shift(struct world *w)
{
    struct wsap_vtcore *t = w->term[w->active].t;
    int rows = tt_rows(t);
    feedf(w, "\033[%d;%dH\033[%d%c", 2 + rnd_n(rows > 2 ? rows - 2 : 1), 1 + rnd_n(tt_cols(t)),
          1 + rnd_n(3), "STLM"[rnd_n(4)]);
}

static void op_cursor(struct world *w)
{
    struct wsap_vtcore *t = w->term[w->active].t;
    switch (rnd_n(5)) {
    case 0:
        feedf(w, "\033[%d;%dH", 1 + rnd_n(tt_rows(t)), 1 + rnd_n(tt_cols(t)));
        break;
    case 1:
        feedf(w, "\033[%d q", rnd_n(7));
        break;
    case 2:
        w->cursor = !w->cursor;
        break;
    case 3:
        feedf(w, rnd_p(50) ? "\033[?25l" : "\033[?25h");
        break;
    default:
        feedf(w, "\033[%dD", 1 + rnd_n(8));
        break;
    }
}

static void op_edit(struct world *w)
{
    if (rnd_p(40))
        feedf(w, "\033[48;5;%dm", rnd_n(256));
    static const char *const edits[] = { "J", "K", "X", "L", "M", "@", "P", "S", "T" };
    feedf(w, "\033[%d%s", rnd_n(4), edits[rnd_n((int)(sizeof(edits) / sizeof(edits[0])))]);
}

static void op_scroll(struct world *w)
{
    int n = 1 + rnd_n(rnd_p(20) ? 40 : 4);
    for (int i = 0; i < n; i++)
        feedf(w, "line %d %s\r\n", rnd_n(1000), rnd_p(30) ? "\033[3mitalic\033[23m -> ok" : "plain");
    ov_damage(w, OV_SCROLLBAR);
    ov_scrollbar(w);
}

static void op_region(struct world *w)
{
    struct wsap_vtcore *t = w->term[w->active].t;
    int rows = tt_rows(t);
    int top = 1 + rnd_n(rows / 2), bottom = top + 1 + rnd_n(rows - top);
    feedf(w, "\033[%d;%dr\033[%d;1H", top, bottom, bottom);
    int n = 1 + rnd_n(5);
    for (int i = 0; i < n; i++)
        feedf(w, "\r\nregion %d -> %s", rnd_n(100), rnd_p(50) ? "日本" : "ab");
    if (rnd_p(30))
        feedf(w, "\033[%dL", 1 + rnd_n(3));
    if (rnd_p(60))
        feedf(w, "\033[r");
}

static void op_select(struct world *w)
{
    struct wsap_vtcore *t = w->term[w->active].t;
    if (rnd_p(30)) {
        wsap_vtcore_select_clear(t);
        return;
    }
    int cols = tt_cols(t), rows = tt_rows(t);
    tt_select(t, rnd_n(cols), rnd_n(rows), rnd_n(cols), rnd_n(rows));
}

static void op_view(struct world *w)
{
    struct wsap_vtcore *t = w->term[w->active].t;
    int total = tt_history(t);
    if (rnd_p(30))
        wsap_vtcore_scroll_to(t, 0);
    else
        wsap_vtcore_scroll(t, rnd_n(9) - 4);
    (void)total;
    ov_damage(w, OV_SCROLLBAR);
    ov_scrollbar(w);
}

static void op_palette(struct world *w)
{
    switch (rnd_n(5)) {
    case 0:
        feedf(w, "\033]4;%d;rgb:%02x/%02x/%02x\033\\", rnd_n(256), rnd_n(256), rnd_n(256),
              rnd_n(256));
        break;
    case 1:
        feedf(w, "\033]10;rgb:%02x/%02x/%02x\033\\", 128 + rnd_n(128), 128 + rnd_n(128),
              128 + rnd_n(128));
        break;
    case 2:
        feedf(w, "\033]11;rgb:%02x/%02x/%02x\033\\", rnd_n(80), rnd_n(80), rnd_n(80));
        break;
    case 3:
        feedf(w, "\033]12;rgb:%02x/%02x/%02x\033\\", rnd_n(256), rnd_n(256), rnd_n(256));
        break;
    default:
        feedf(w, "\033]104\033\\\033]110\033\\\033]111\033\\\033]112\033\\");
        break;
    }
}

static void op_resize(struct world *w)
{
    struct wsap_vtcore *t = w->term[w->active].t;
    int cols = w->max_cols / 3 + rnd_n(w->max_cols - w->max_cols / 3 + 1);
    int rows = 2 + rnd_n(w->max_rows - 1);
    TCHECK(tt_resize(t, cols, rows), "resize %dx%d", cols, rows);
}

static void op_overlay(struct world *w)
{
    int i = rnd_n(OV_RANDOM + 1);
    ov_damage(w, i);
    if (i == OV_SCROLLBAR) {
        if (w->ov[i].on && rnd_p(50))
            w->ov[i].on = false;
        else
            ov_scrollbar(w);
        return;
    }
    struct overlay *o = &w->ov[i];
    if (o->on && rnd_p(35)) {
        o->on = false;
    } else if (o->on && rnd_p(50)) {
        o->rect.x += rnd_n(41) - 20;
        o->rect.y += rnd_n(21) - 10;
    } else {
        ov_random(w, o);
    }
}

static void op_grid_rect(struct world *w)
{
    struct vector2i c = w->canvas.size;
    int x = rnd_p(50) ? 0 : 1 + rnd_n(9), y = rnd_p(50) ? 0 : 1 + rnd_n(7);
    w->grid = trect(x, y, c.x - x - rnd_n(5), c.y - y - rnd_n(5));
}

/* The UI ink of every overlay changes with the canvas, so all of it is damage. */
static void all_ui_damage(struct world *w)
{
    w->ui_damage.all = true;
}

static void op_canvas(struct world *w)
{
    const struct font_metrics *m = font_instance_metrics(w->fonts[0]);
    int cw = w->max_cols * m->cell_w + 11 + rnd_n(9);
    int ch = w->max_rows * m->cell_h + 7 + rnd_n(5);
    tcanvas_resize(&w->canvas, cw, ch);
    w->grid = trect(5, 3, cw - 5, ch - 3);
    all_ui_damage(w);
}

/* One random change. Returns false when it changed nothing. */
static bool step_change(struct world *w)
{
    int r = rnd_n(1000);
    if (r < 180) op_ascii(w);
    else if (r < 260) op_unicode(w);
    else if (r < 350) op_sgr(w);
    else if (r < 420) op_scroll(w);
    else if (r < 480) op_cursor(w);
    else if (r < 520) op_edit(w);
    else if (r < 560) op_region(w);
    else if (r < 600) op_select(w);
    else if (r < 640) op_view(w);
    else if (r < 655) feedf(w, rnd_p(50) ? "\033[?5h" : "\033[?5l");
    else if (r < 670) op_palette(w);
    else if (r < 685) op_resize(w);
    else if (r < 691) op_canvas(w);
    else if (r < 705) w->font ^= 1;
    else if (r < 730) {
        w->active ^= 1;
        ov_damage(w, OV_SCROLLBAR);
        w->ov[OV_SCROLLBAR].on = false;
    }
    else if (r < 810) op_overlay(w);
    else if (r < 817) w->canvas.fresh = true;
    else if (r < 824) w->repaint = true;
    else if (r < 834) feedf(w, rnd_p(50) ? "\033[?1049h" : "\033[?1049l");
    else if (r < 836) feedf(w, "\033[2J");
    else if (r < 842) {
        op_grid_rect(w);
        all_ui_damage(w);
    }
    else if (r < 900) op_fill(w);
    else if (r < 980) op_shift(w);
    else return false;
    return true;
}

/* ---- The walk --------------------------------------------------------- */

struct config {
    uint64_t seed;
    int cols, rows;
    int steps;
    struct render_options opt;
    enum font_aa_mode aa;
};

static void run(const struct config *cfg, struct font_instance *big)
{
    rng_state = cfg->seed * UINT64_C(0x9E3779B97F4A7C15) | 1;
    font_set_aa_mode(cfg->aa);
    struct renderer *r = renderer_create(&cfg->opt);
    struct renderer *oracle = renderer_create(&(struct render_options){
        .threads = RENDER_NONE, .cache_bytes = RENDER_NONE });
    TCHECK(r && oracle, "renderers");

    struct world w = {
        .cursor = true,
        .fonts = { font_instance_default(), big },
        .ui_font = font_instance_default(),
        .max_cols = cfg->cols,
        .max_rows = cfg->rows,
    };
    for (int i = 0; i < 2; i++) {
        tterm_open(&w.term[i], cfg->cols, cfg->rows);
        tterm_feed(&w.term[i], i ? "\033[38;5;214mtab two\033[0m\r\n" : "tab one\r\n");
    }
    ui_scene_init(&w.scene);
    op_canvas(&w);

    size_t cap = tcanvas_px(&w.canvas);
    uint32_t *want = malloc(cap * sizeof(*want));
    uint32_t *before = malloc(cap * sizeof(*before));
    TCHECK(want && before, "buffers");

    long damaged = 0, total = 0;
    for (int step = 0; step < cfg->steps; step++) {
        w.ui_damage = (struct ui_damage){0};
        memset(w.ov_changed, 0, sizeof(w.ov_changed));
        w.repaint = false;
        size_t n = tcanvas_px(&w.canvas);
        bool changed = step == 0 || step_change(&w);
        bool resized = n != tcanvas_px(&w.canvas) || w.canvas.fresh;
        n = tcanvas_px(&w.canvas);
        if (n > cap) {
            cap = n;
            want = realloc(want, cap * sizeof(*want));
            before = realloc(before, cap * sizeof(*before));
            TCHECK(want && before, "buffers");
        }
        if (!resized)
            memcpy(before, w.canvas.px, n * sizeof(*before));
        scene_build(&w);
        for (int i = 0; i < OV_COUNT; i++)
            if (w.ov[i].on && w.ov_changed[i])
                ui_damage_add(&w.ui_damage, w.ov[i].ink);

        const struct terminal_frame *tf = tterm_frame(&w.term[w.active], w.cursor);
        struct render_frame frame = {
            .base = 0xFF1E1E2Eu,
            .grid = { .frame = tf, .font = w.fonts[w.font], .rect = w.grid },
            .ui = &w.scene,
            .ui_font = w.ui_font,
            .ui_damage = &w.ui_damage,
            .repaint = w.repaint,
        };
        struct render_result res = tdraw(r, &w.canvas, &frame);
        tdraw_fresh(oracle, want, w.canvas.size, &frame);

        char what[96];
        snprintf(what, sizeof(what), "seed %llu step %d", (unsigned long long)cfg->seed, step);
        tcheck_same(what, w.canvas.px, want, w.canvas.size);
        if (resized)
            TCHECK(res.damage.all, "%s: a fresh canvas reports all damage", what);
        else
            tcheck_damage(what, before, w.canvas.px, w.canvas.size, &res.damage);
        if (!changed)
            TCHECK(!res.damage.any, "%s: an idle step damages (%d rects)", what, res.damage.n);
        total++;
        damaged += res.damage.all;
    }
    printf("seed %llu: %d steps, %ld full frames, isa %d, threads %d, cache %td\n",
           (unsigned long long)cfg->seed, cfg->steps, damaged, renderer_isa(r), cfg->opt.threads,
           cfg->opt.cache_bytes);
    /* The walk must stay mostly incremental, or it tests nothing. */
    TCHECK(damaged * 3 < total, "seed %llu: %ld of %ld frames were full",
           (unsigned long long)cfg->seed, damaged, total);

    free(want);
    free(before);
    ui_scene_destroy(&w.scene);
    tcanvas_free(&w.canvas);
    for (int i = 0; i < 2; i++)
        tterm_close(&w.term[i]);
    renderer_destroy(oracle);
    renderer_destroy(r);
}

int main(void)
{
    TCHECK(render_init(), "render_init");
    struct font_instance *big = font_instance_acquire(
        font_instance_size_pt(font_instance_default()), 180);
    TCHECK(big, "1.5x font");

    const char *env = getenv("INCREMENTAL_STEPS");
    int steps = env ? atoi(env) : 0;
    const struct config cfgs[] = {
        { 1, 64, 14, 700, { 0 }, FONT_AA_GRAYSCALE },
        { 2, 64, 14, 700, { .threads = 3, .cache_bytes = 64 << 10 }, FONT_AA_GRAYSCALE },
        { 3, 48, 12, 600, { .threads = RENDER_NONE }, FONT_AA_RGB },
        { 4, 160, 50, 600, { .threads = 3 }, FONT_AA_GRAYSCALE },
        { 5, 150, 44, 500, { .threads = 2, .cache_bytes = 64 << 10 }, FONT_AA_RGB },
        { 6, 70, 16, 600, { .threads = RENDER_NONE, .cache_bytes = 96 << 10 }, FONT_AA_GRAYSCALE },
    };
    static const char *const scroll_modes[] = { "auto", "move", "paint" };
    for (unsigned m = 0; m < sizeof(scroll_modes) / sizeof(scroll_modes[0]); m++) {
        /* The renderer reads the value once, in renderer_create(). */
        setenv("WSAP_RENDER_SCROLL", scroll_modes[m], 1);
        printf("scroll method: %s\n", scroll_modes[m]);
        for (unsigned i = 0; i < sizeof(cfgs) / sizeof(cfgs[0]); i++) {
            struct config c = cfgs[i];
            if (steps > 0)
                c.steps = steps;
            run(&c, big);
        }
    }
    unsetenv("WSAP_RENDER_SCROLL");
    font_set_aa_mode(FONT_AA_GRAYSCALE);
    font_instance_unref(big);
    render_shutdown();
    puts("PASS: incremental frames equal fresh frames");
    return 0;
}
