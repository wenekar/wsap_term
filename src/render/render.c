#define _GNU_SOURCE
#include "render/render.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "font/font.h"
#include "font/glyph_cache.h"
#include "paint/blend.h"
#include "paint/damage.h"
#include "paint/scene.h"
#include "paint/shadow.h"
#include "render/cpu/cell_cache.h"
#include "render/cpu/decor.h"
#include "render/cpu/isa.h"
#include "render/cpu/kernels.h"
#include "render/cpu/pool.h"
#include "render/cpu/raster.h"
#include "render/gpu/gpu.h"
#include "render/plan.h"
#include "render/scene_paint.h"
#include "render/surface.h"

/*
 * render_draw() runs one frame in the order of src/render/README.md,
 * "Frame order in render_draw". This file owns no pixel math. It plans,
 * moves rows, starts the row raster on the pool, and paints the UI.
 */

struct renderer {
    int isa;
    raster_row_fn row;
    const struct render_kernels *k;
    struct pool *pool;
    /* The main thread and each pool worker have one raster worker. */
    int n_workers;
    struct raster_worker *workers;
    struct cell_cache_worker *cache_workers[POOL_WORKERS_MAX + 1];
    struct cell_cache *cache;
    struct decor_cache *decor;
    struct render_gpu *gpu;
    struct render_surface scratch;
    bool force_full;
    /* WSAP_RENDER_SCROLL, read once. SCROLL_AUTO (0) follows the cost. */
    enum { SCROLL_AUTO, SCROLL_MOVE, SCROLL_PAINT } scroll_mode;
    /* The paint cost of one cell on one thread, in ps. 0: not known yet. */
    int64_t paint_ps;
    /* The frame arrays. They hold `cap` rows and grow only. */
    int cap;
    uint64_t *keys;
    struct plan_span *ui;
    uint16_t *reach;
    struct plan_row *ops;
    struct plan_move *moves;
    struct plan_window *windows;
    struct raster_job *jobs;
};

static int render_users;

bool render_init(void)
{
    if (render_users > 0) {
        render_users++;
        return true;
    }
    srgb_build_luts();
    if (!font_system_init())
        return false;
    render_users = 1;
    return true;
}

void render_shutdown(void)
{
    if (render_users == 0 || --render_users > 0)
        return;
    font_system_shutdown();
    ui_shadow_cache_destroy();
}

struct renderer *renderer_create(const struct render_options *opt)
{
    struct render_options o = opt ? *opt : (struct render_options){0};
    struct renderer *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->isa = render_isa_resolve(o.isa);
    r->row = raster_row_for(r->isa);
    r->k = render_kernels_for(r->isa);
    const char *full = getenv("WSAP_RENDER_FULL");
    r->force_full = full && !strcmp(full, "1");
    const char *scroll = getenv("WSAP_RENDER_SCROLL");
    if (scroll && !strcmp(scroll, "move"))
        r->scroll_mode = SCROLL_MOVE;
    else if (scroll && !strcmp(scroll, "paint"))
        r->scroll_mode = SCROLL_PAINT;

    int threads = o.threads == 0 ? pool_default_workers() : o.threads < 0 ? 0 : o.threads;
    if (threads > POOL_WORKERS_MAX)
        threads = POOL_WORKERS_MAX;
    if (threads > 0) {
        r->pool = pool_create(threads);
        if (!r->pool)
            goto fail;
    }
    ptrdiff_t budget = o.cache_bytes == 0 ? RENDER_CACHE_DEFAULT_BYTES : o.cache_bytes;
    if (budget > 0)
        r->cache = cell_cache_create((size_t)budget);

    r->n_workers = 1 + pool_workers(r->pool);
    r->workers = calloc((size_t)r->n_workers, sizeof(*r->workers));
    r->decor = decor_cache_create();
    if (!r->workers || !r->decor)
        goto fail;
    for (int i = 0; i < r->n_workers; i++) {
        if (!raster_worker_init(&r->workers[i], r->cache))
            goto fail;
        r->cache_workers[i] = r->workers[i].cache;
    }
    r->gpu = render_gpu_new();
    render_surface_init(&r->scratch);
    return r;
fail:
    renderer_destroy(r);
    return NULL;
}

void renderer_destroy(struct renderer *r)
{
    if (!r)
        return;
    pool_destroy(r->pool);
    if (r->workers)
        for (int i = 0; i < r->n_workers; i++)
            raster_worker_release(&r->workers[i]);
    free(r->workers);
    cell_cache_destroy(r->cache);
    decor_cache_destroy(r->decor);
    render_surface_release(&r->scratch);
    render_gpu_free(r->gpu);
    free(r->keys);
    free(r->ui);
    free(r->reach);
    free(r->ops);
    free(r->moves);
    free(r->windows);
    free(r->jobs);
    free(r);
}

void *renderer_device(struct renderer *r)
{
    return r ? render_gpu_native_device(r->gpu) : NULL;
}

int renderer_isa(const struct renderer *r)
{
    return r ? r->isa : render_isa_resolve(RENDER_ISA_AUTO);
}

/* Makes room for `rows` rows in the frame arrays. Allocates only to grow. */
static bool frame_reserve(struct renderer *r, int rows)
{
    if (rows <= r->cap && r->keys)
        return true;
    int cap = rows > r->cap ? rows : r->cap;
    size_t n = (size_t)cap + 1;
    size_t windows = (size_t)PLAN_WINDOWS_MAX(cap);
#define GROW(field, count)                                                  \
    do {                                                                    \
        void *p_ = realloc(r->field, (count) * sizeof(*r->field));          \
        if (!p_)                                                            \
            return false;                                                   \
        r->field = p_;                                                      \
    } while (0)
    GROW(keys, n);
    GROW(ui, n);
    GROW(reach, n);
    GROW(ops, n);
    GROW(moves, n);
    GROW(windows, windows);
    GROW(jobs, n + windows);
#undef GROW
    r->cap = cap;
    return true;
}

/* ---- Pixels outside the raster --------------------------------------- */

static void fill_rect(const struct renderer *r, uint32_t *px, int stride, struct rect2i q,
                      uint32_t color)
{
    if (rect2i_is_empty(q))
        return;
    uint32_t *p = px + (size_t)q.y * (size_t)stride + q.x;
    if (q.x == 0 && q.width == stride) {
        size_t n = (size_t)q.width * (size_t)q.height;
        while (n) {
            int m = n > (size_t)1 << 30 ? 1 << 30 : (int)n;
            r->k->fill(p, m, color);
            p += m;
            n -= (size_t)m;
        }
        return;
    }
    for (int y = 0; y < q.height; y++)
        r->k->fill(p + (size_t)y * (size_t)stride, q.width, color);
}

/* Fills q minus the grid rect with `color`. */
static void fill_outside(const struct renderer *r, uint32_t *px, int stride, struct rect2i q,
                         struct rect2i grid, uint32_t color)
{
    struct rect2i in = rect2i_intersect(q, grid);
    if (rect2i_is_empty(in)) {
        fill_rect(r, px, stride, q, color);
        return;
    }
    struct vector2i qe = rect2i_end(q), ie = rect2i_end(in);
    fill_rect(r, px, stride, (struct rect2i){
        .pos = q.pos, .size = { .x = q.width, .y = in.y - q.y } }, color);
    fill_rect(r, px, stride, (struct rect2i){
        .pos = { .x = q.x, .y = ie.y }, .size = { .x = q.width, .y = qe.y - ie.y } }, color);
    fill_rect(r, px, stride, (struct rect2i){
        .pos = { .x = q.x, .y = in.y }, .size = { .x = in.x - q.x, .y = in.height } }, color);
    fill_rect(r, px, stride, (struct rect2i){
        .pos = { .x = ie.x, .y = in.y }, .size = { .x = qe.x - ie.x, .y = in.height } }, color);
}

/* Copies the plan's move blocks. Each block covers the full grid width. */
static void run_moves(uint32_t *px, int stride, struct rect2i grid, int ch,
                      const struct plan_move *moves, int n)
{
    for (int i = 0; i < n; i++) {
        const struct plan_move *m = &moves[i];
        size_t lines = (size_t)m->count * (size_t)ch;
        uint32_t *dst = px + ((size_t)grid.y + (size_t)m->dst * ch) * stride + grid.x;
        uint32_t *src = px + ((size_t)grid.y + (size_t)m->src * ch) * stride + grid.x;
        if (grid.x == 0 && grid.width == stride) {
            memmove(dst, src, lines * (size_t)stride * sizeof(*px));
            continue;
        }
        /* Lines move one at a time. Go against the move so that no line is read
         * after it was written. */
        size_t row_bytes = (size_t)grid.width * sizeof(*px);
        if (m->dst < m->src) {
            for (size_t l = 0; l < lines; l++)
                memmove(dst + l * stride, src + l * stride, row_bytes);
        } else {
            for (size_t l = lines; l-- > 0;)
                memmove(dst + l * stride, src + l * stride, row_bytes);
        }
    }
}

/*
 * The cost of a move, in picoseconds per byte. A move is one memmove on
 * the main thread: about 80 GB/s for rows that the cache holds.
 */
#define MOVE_PS_PER_BYTE 12

/*
 * The cost of a paint comes from the paints of the last frames: the time
 * of the raster phase times its threads, over its cells. The cost of a
 * cell changes a lot with its content (a code cell hits L0, a cell of
 * one-off colors composes), so a constant does not work. A frame with
 * fewer cells than this gives no sample.
 */
#define PAINT_SAMPLE_CELLS 64

static int64_t clock_ps(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return ((int64_t)t.tv_sec * 1000000000 + t.tv_nsec) * 1000;
}

/*
 * Turns the moves of the plan into paints when the pool paints those rows
 * faster than the main thread copies them. Both give the same pixels, so
 * the choice can follow timing: it changes only the speed. The damage and
 * the UI regions of the plan stay right: they already hold the bands of
 * the moved rows. The windows of those rows go, because a full row paint
 * covers them. It paints only when the estimate wins by a margin of one
 * quarter, so that the noise of the samples does not cost time.
 */
static void paint_moves(const struct renderer *r, struct plan_output *out, int rows, int cols,
                        int ch, int width)
{
    if (!out->n_moves || r->scroll_mode == SCROLL_MOVE)
        return;
    if (r->scroll_mode == SCROLL_AUTO && (!r->pool || !r->paint_ps))
        return;
    int64_t moved = 0, painted = 0;
    for (int i = 0; i < out->n_moves; i++)
        moved += out->moves[i].count;
    for (int i = 0; i < rows; i++)
        painted += out->rows[i].op == PLAN_PAINT;
    int64_t cells = (moved + painted) * cols;
    int64_t threads = cells < POOL_SERIAL_CELLS ? 1 : r->n_workers;
    int64_t move_ps = moved * ch * width * (int64_t)sizeof(uint32_t) * MOVE_PS_PER_BYTE;
    int64_t paint_ps = moved * cols * r->paint_ps / threads;
    if (r->scroll_mode == SCROLL_AUTO && 4 * paint_ps >= 3 * move_ps)
        return;
    for (int i = 0; i < rows; i++)
        if (out->rows[i].op == PLAN_MOVE)
            out->rows[i] = (struct plan_row){ .op = PLAN_PAINT };
    out->n_moves = 0;
    int n = 0;
    for (int i = 0; i < out->n_windows; i++) {
        const struct plan_window *w = &out->windows[i];
        if (w->row >= rows || out->rows[w->row].op != PLAN_PAINT)
            out->windows[n++] = *w;
    }
    out->n_windows = n;
}

/* ---- The parallel phase ---------------------------------------------- */

struct phase {
    const struct renderer *r;
    const struct raster_frame *rf;
    const struct raster_job *jobs;
    int n_full;
    uint16_t *reach;
};

static void phase_run(void *ctx, int worker, int begin, int end)
{
    const struct phase *p = ctx;
    struct raster_worker *w = &p->r->workers[worker];
    for (int i = begin; i < end; i++) {
        int reach = p->r->row(p->rf, w, &p->jobs[i]);
        if (i < p->n_full)
            p->reach[p->jobs[i].row] = (uint16_t)(reach > UINT16_MAX ? UINT16_MAX : reach);
    }
}

/* ---- render_draw ----------------------------------------------------- */

static struct render_result result_all(void)
{
    struct render_result res = {0};
    damage_add_all(&res.damage);
    return res;
}

struct render_result render_draw(struct renderer *r, struct framebuffer *fb,
                                 const struct render_frame *frame)
{
    if (!r || !fb || !frame)
        return (struct render_result){0};

    struct render_surface *s = fb->history ? render_surface_of(fb->history) : NULL;
    bool keep = s != NULL;
    if (!s)
        s = &r->scratch;
    if (!fb->pixels || fb->size.x <= 0 || fb->size.y <= 0) {
        render_surface_invalidate(s);
        return result_all();
    }
    uint32_t *px = fb->pixels;
    const struct vector2i canvas = fb->size;
    const int stride = canvas.x;

    /* 1. Fonts. A refresh can rebuild, so it comes before any fetch and
     * before the metrics are read. */
    struct font_instance *gfont = frame->grid.font ? frame->grid.font : font_instance_default();
    struct font_instance *ufont = frame->ui_font ? frame->ui_font : font_instance_default();
    if (gfont)
        glyph_cache_refresh(gfont);
    if (ufont && ufont != gfont)
        glyph_cache_refresh(ufont);

    /* 2. The grid, the surface key, the row keys, and the UI ink. */
    const struct terminal_frame *tf = frame->grid.frame;
    struct rect2i grid = frame->grid.rect;
    int cw = 0, ch = 0, cols = 0, rows = 0;
    bool has_grid = tf && terminal_frame_valid(tf) && gfont && grid.x >= 0 && grid.y >= 0 &&
                    rect2i_clip(&grid, canvas);
    if (has_grid) {
        const struct font_metrics *m = font_instance_metrics(gfont);
        cw = m->cell_w;
        ch = m->cell_h;
        has_grid = cw > 0 && ch > 0;
    }
    if (has_grid) {
        cols = grid.width / cw < tf->cols ? grid.width / cw : tf->cols;
        rows = grid.height / ch < tf->rows ? grid.height / ch : tf->rows;
    } else {
        grid = (struct rect2i){0};
        tf = NULL;
    }

    struct render_surface_key key = { .canvas = canvas, .base = frame->base };
    if (has_grid) {
        key.grid = grid;
        /* The terminal size, not the visible size: a reflow keeps the row
         * keys of rows that it rewraps. */
        key.cells = (struct vector2i){ .x = tf->cols, .y = tf->rows };
        key.cell = (struct vector2i){ .x = cw, .y = ch };
        key.font_version = font_instance_version(gfont);
        key.palette_gen = tf->palette_gen;
        key.default_fg = tf->default_fg;
        key.default_bg = tf->default_bg;
        key.cursor_color = tf->cursor_color;
        key.selection_fg = terminal_frame_selection_fg(tf);
        key.selection_bg = terminal_frame_selection_bg(tf);
        key.reverse_video = tf->reverse_video;
    }

    if (!frame_reserve(r, rows) || !render_surface_reserve(s, rows)) {
        render_surface_invalidate(s);
        return result_all();
    }
    const struct ui_scene *scene = frame->ui;
    bool scene_ok = scene && ui_scene_is_valid(scene);
    for (int i = 0; i < rows; i++)
        r->keys[i] = render_surface_row_key(tf, i);
    plan_ui_ink(scene_ok ? scene : NULL, grid, ch, rows, r->ui);

    uint64_t ufont_version = ufont ? font_instance_version(ufont) : 0;
    const struct ui_damage *ui_damage = frame->ui_damage;
    if ((scene && !scene_ok) || ufont_version != s->ui_font_version)
        ui_damage = NULL;
    bool invalid = !keep || !s->valid || fb->fresh || frame->repaint || r->force_full ||
                   !render_surface_key_eq(&s->key, &key);

    /* 3. Plan. */
    struct plan_input in = {
        .canvas = canvas,
        .grid = grid,
        .rows = rows,
        .ch = ch,
        .invalid = invalid,
        .old_keys = s->keys,
        .new_keys = r->keys,
        .old_ui = s->ui,
        .ui_damage = ui_damage,
    };
    struct plan_output out = {
        .rows = r->ops,
        .moves = r->moves,
        .windows = r->windows,
    };
    plan_frame(&in, &out);

    /* 4. The GPU draws a whole frame or nothing. */
    if (fb->drawable && !frame->cpu_only) {
        switch (render_gpu_draw(r->gpu, s, fb, frame)) {
        case RENDER_GPU_PRESENTED: {
            render_surface_invalidate(s);
            struct render_result res = result_all();
            res.presented = true;
            return res;
        }
        case RENDER_GPU_DROPPED:
            return (struct render_result){0};
        case RENDER_GPU_UNUSED:
            break;
        }
    }

    /* 5. Moves, or paints where the pool is faster. */
    paint_moves(r, &out, rows, cols, ch, grid.width);
    run_moves(px, stride, grid, ch, out.moves, out.n_moves);

    /* 6. Raster: the painted rows, then the windows of kept and moved rows. */
    int n_jobs = 0, n_full = 0;
    long cells = 0;
    for (int i = 0; i < rows; i++) {
        switch (out.rows[i].op) {
        case PLAN_PAINT:
            r->jobs[n_jobs++] = (struct raster_job){ .row = i, .x1 = grid.width };
            cells += cols;
            break;
        case PLAN_KEEP:
            r->reach[i] = s->reach[i];
            break;
        case PLAN_MOVE:
            r->reach[i] = s->reach[out.rows[i].src];
            break;
        }
    }
    n_full = n_jobs;
    struct rect2i margin = {
        .pos = { .x = grid.x, .y = grid.y + rows * ch },
        .size = { .x = grid.width, .y = grid.height - rows * ch },
    };
    if (out.all)
        fill_rect(r, px, stride, margin, has_grid ? tf->default_bg : frame->base);
    for (int i = 0; i < out.n_windows; i++) {
        const struct plan_window *win = &out.windows[i];
        if (win->row >= rows) {
            struct rect2i q = margin;
            q.x = grid.x + win->x.x0;
            q.width = win->x.x1 - win->x.x0;
            fill_rect(r, px, stride, q, has_grid ? tf->default_bg : frame->base);
            continue;
        }
        int src = out.rows[win->row].op == PLAN_MOVE ? out.rows[win->row].src : win->row;
        r->jobs[n_jobs++] = (struct raster_job){
            .row = win->row, .x0 = win->x.x0, .x1 = win->x.x1, .reach = s->reach[src],
        };
        cells += (win->x.x1 - win->x.x0 + 2 * s->reach[src]) / cw + 2;
    }
    if (n_jobs) {
        const struct decor_set *decor = decor_get(r->decor, font_instance_metrics(gfont));
        uint32_t l0_fg = tf->reverse_video ? tf->default_bg : tf->default_fg;
        uint32_t l0_bg = tf->reverse_video ? tf->default_fg : tf->default_bg;
        bool serial = cells < POOL_SERIAL_CELLS || !r->pool;
        int n_cache = serial ? 1 : r->n_workers;
        struct cell_class *klass =
            cell_cache_begin(r->cache, key.cell, key.font_version, l0_fg, l0_bg,
                             cells > INT32_MAX ? INT32_MAX : (int)cells, r->cache_workers,
                             n_cache);
        struct raster_frame rf = {
            .font = gfont,
            .frame = tf,
            .decor = decor,
            .cache = klass,
            .font_version = key.font_version,
            .origin = px + (size_t)grid.y * (size_t)stride + grid.x,
            .stride = stride,
            .cw = cw,
            .ch = ch,
            .cols = cols,
            .width = grid.width,
        };
        struct phase ph = {
            .r = r, .rf = &rf, .jobs = r->jobs, .n_full = n_full, .reach = r->reach,
        };
        int64_t t0 = clock_ps();
        glyph_cache_set_parallel(true);
        pool_run(r->pool, n_jobs, serial ? n_jobs : 1, phase_run, &ph);
        glyph_cache_set_parallel(false);
        if (cells >= PAINT_SAMPLE_CELLS) {
            int64_t sample = (clock_ps() - t0) * (serial ? 1 : r->n_workers) / cells;
            r->paint_ps = r->paint_ps ? (3 * r->paint_ps + sample) / 4 : sample;
        }
        /* 7. Publish. */
        cell_cache_publish(r->cache, klass, r->cache_workers, n_cache);
    }

    /* 8. The UI regions: the base outside the grid, then the scene. */
    struct rect2i bounds = scene_ok ? ui_scene_bounds(scene) : (struct rect2i){0};
    for (int i = 0; i < out.ui.n; i++) {
        struct rect2i q = out.ui.rects[i];
        if (!rect2i_clip(&q, canvas))
            continue;
        fill_outside(r, px, stride, q, grid, frame->base);
        struct rect2i p = rect2i_intersect(q, bounds);
        if (scene_ok && ufont && !rect2i_is_empty(p))
            (void)ui_scene_paint_clipped(ufont, px, canvas.x, canvas.y, scene, p);
    }

    /* 9. Flush. 10. Commit. */
    glyph_cache_flush_frame();
    if (keep) {
        render_surface_commit(s, &key, rows, r->keys, r->ui, r->reach);
        s->ui_font_version = ufont_version;
    } else {
        render_surface_invalidate(s);
    }

    struct render_result res = { .damage = out.damage };
    if (res.damage.n < 0) {
        res.damage.n = 1;
        res.damage.rects[0] = res.damage.bounds;
    }
    /* 11. Present the CPU pixels into a drawable. */
    if (fb->drawable)
        res.presented = render_gpu_present(r->gpu, s, fb, &res.damage);
    return res;
}
