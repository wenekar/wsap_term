#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "paint/scene.h"
#include "render/plan.h"
#include "render/surface.h"

/*
 * Tests of the frame planner (render/plan.h) and of the surface state
 * (render/surface.h).
 *
 * Most cases run the plan through a pixel model of the executor. The model
 * canvas stores, for each pixel, what it shows: a row content, the margin,
 * or the base, plus the UI op on top and how many times the UI blended
 * there. The executor model follows the frame order of render_draw(): the
 * moves, the painted rows and the windows, then the UI regions. The result
 * must equal a fresh paint of the new frame, and the damage must cover
 * every pixel that changed. A UI that blends twice over one pixel gives a
 * layer count of 2, so the model also finds a missing overlay window.
 */

#define BASE_ID UINT64_MAX
#define MARGIN_ID (UINT64_MAX - 1)

static struct rect2i rect(int x, int y, int w, int h)
{
    return (struct rect2i){ .pos = { .x = x, .y = y }, .size = { .x = w, .y = h } };
}

static struct vector2i vec(int x, int y)
{
    return (struct vector2i){ .x = x, .y = y };
}

static struct plan_span span(int x0, int x1)
{
    return (struct plan_span){ .x0 = x0, .x1 = x1 };
}

/* --- Test doubles for the two terminal_frame accessors of surface.c --- */

#define STUB_ROWS 8
static uint64_t stub_content[STUB_ROWS];
static struct wsap_vtcore_span stub_selection[STUB_ROWS];

uint64_t terminal_frame_row_key(const struct terminal_frame *frame, int row)
{
    (void)frame;
    return row >= 0 && row < STUB_ROWS ? stub_content[row] : 0;
}

struct wsap_vtcore_span terminal_frame_row_selection(const struct terminal_frame *frame, int row)
{
    (void)frame;
    return row >= 0 && row < STUB_ROWS ? stub_selection[row] : (struct wsap_vtcore_span){0};
}

/* --- Fixture --- */

struct fixture {
    int rows, ch;
    struct vector2i canvas;
    struct rect2i grid;
    uint64_t *old_keys, *new_keys;
    struct plan_span *old_ui;
    struct ui_damage ui_damage;
    struct plan_input in;
    struct plan_output out;
};

/* The arrays have the exact sizes from plan.h, so ASan sees any overrun. */
static struct fixture *fixture_new(struct vector2i canvas, struct rect2i grid, int rows, int ch)
{
    struct fixture *f = calloc(1, sizeof(*f));
    assert(f);
    size_t n = rows > 0 ? (size_t)rows : 1;
    f->rows = rows;
    f->ch = ch;
    f->canvas = canvas;
    f->grid = grid;
    f->old_keys = calloc(n, sizeof(*f->old_keys));
    f->new_keys = calloc(n, sizeof(*f->new_keys));
    f->old_ui = calloc((size_t)rows + 1, sizeof(*f->old_ui));
    f->out.rows = malloc(n * sizeof(*f->out.rows));
    f->out.moves = malloc(n * sizeof(*f->out.moves));
    f->out.windows = malloc((size_t)PLAN_WINDOWS_MAX(rows) * sizeof(*f->out.windows));
    assert(f->old_keys && f->new_keys && f->old_ui && f->out.rows && f->out.moves &&
           f->out.windows);
    f->in = (struct plan_input){
        .canvas = canvas,
        .grid = grid,
        .rows = rows,
        .ch = ch,
        .old_keys = f->old_keys,
        .new_keys = f->new_keys,
        .old_ui = f->old_ui,
        .ui_damage = &f->ui_damage,
    };
    return f;
}

static void fixture_free(struct fixture *f)
{
    free(f->old_keys);
    free(f->new_keys);
    free(f->old_ui);
    free(f->out.rows);
    free(f->out.moves);
    free(f->out.windows);
    free(f);
}

/*
 * The standard grid: 10 rows of 10 px at (10, 20), 80 px wide, with a 6 px
 * bottom margin. The canvas has room on every side.
 */
static struct fixture *standard(int rows)
{
    int grid_h = rows * 10 + 6;
    return fixture_new(vec(100, 20 + grid_h + 14), rect(10, 20, 80, grid_h), rows, 10);
}

/* Old keys 1..rows. New keys equal the old ones. */
static void keys_idle(struct fixture *f)
{
    for (int r = 0; r < f->rows; r++)
        f->old_keys[r] = f->new_keys[r] = (uint64_t)r + 1;
}

static struct rect2i band(const struct fixture *f, int r0, int r1)
{
    int y0 = f->grid.y + r0 * f->ch;
    int y1 = r1 > f->rows ? f->grid.y + f->grid.height : f->grid.y + r1 * f->ch;
    return rect(f->grid.x, y0, f->grid.width, y1 - y0);
}

static bool contains(struct rect2i outer, struct rect2i inner)
{
    return rect2i_is_empty(inner) || rect2i_eq(rect2i_intersect(outer, inner), inner);
}

static bool damage_has_pixel(const struct damage_list *d, int x, int y)
{
    if (d->all)
        return true;
    for (int i = 0; i < d->n; i++)
        if (rect2i_contains(d->rects[i], vec(x, y)))
            return true;
    return false;
}

static bool ui_has_rect(const struct ui_damage *ui, struct rect2i r)
{
    for (int i = 0; i < ui->n; i++)
        if (rect2i_eq(ui->rects[i], r))
            return true;
    return false;
}

/* --- Structure checks that hold for every plan --- */

static int row_of(const struct fixture *f, int y)
{
    int rows_end = f->grid.y + f->rows * f->ch;
    return y >= rows_end ? f->rows : (y - f->grid.y) / f->ch;
}

/* The vote of rule 3, written out the slow way. */
static int reference_shift(const struct fixture *f)
{
    int rows = f->rows, best = 0, best_votes = 0;
    for (int d = 1; d < rows; d++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            int shift = sign * d, votes = 0;
            for (int r = 0; r < rows; r++) {
                uint64_t k = f->new_keys[r];
                int src = r - shift;
                if (k && f->old_keys[r] != k && src >= 0 && src < rows && f->old_keys[src] == k)
                    votes++;
            }
            if (votes > best_votes) {
                best = shift;
                best_votes = votes;
            }
        }
    }
    return best;
}

/* Runs the moves with memmove on an int canvas, one int run per row. */
#define CELLS 3
static void run_moves(int rows, const struct plan_move *moves, int n, int (*canvas)[CELLS])
{
    for (int i = 0; i < n; i++) {
        const struct plan_move *m = &moves[i];
        assert(m->count > 0 && m->src >= 0 && m->dst >= 0);
        assert(m->src + m->count <= rows && m->dst + m->count <= rows);
        memmove(canvas[m->dst], canvas[m->src], (size_t)m->count * sizeof(canvas[0]));
    }
}

static void check_valid_plan(const struct fixture *f)
{
    const struct plan_output *o = &f->out;
    int rows = f->rows;
    assert(!o->all && !o->damage.all);

    /* Rules 2 and 3. */
    int shift = reference_shift(f);
    assert(o->shift == 0 || o->shift == shift);
    bool any_move = false;
    for (int r = 0; r < rows; r++) {
        uint64_t k = f->new_keys[r];
        int src = r - shift;
        if (k && f->old_keys[r] == k) {
            assert(o->rows[r].op == PLAN_KEEP);
        } else if (shift && k && src >= 0 && src < rows && f->old_keys[src] == k) {
            assert(o->rows[r].op == PLAN_MOVE && o->rows[r].src == src);
            any_move = true;
        } else {
            assert(o->rows[r].op == PLAN_PAINT);
        }
    }
    assert(any_move == (o->shift != 0) && any_move == (o->n_moves > 0));

    /* Rule 4: maximal blocks, and memmove in plan order gives the target. */
    int (*canvas)[CELLS] = calloc(rows > 0 ? (size_t)rows : 1, sizeof(*canvas));
    assert(canvas);
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < CELLS; c++)
            canvas[r][c] = r * CELLS + c;
    int moved = 0;
    for (int i = 0; i < o->n_moves; i++) {
        const struct plan_move *m = &o->moves[i];
        assert(m->src == m->dst - o->shift);
        assert(m->dst == 0 || o->rows[m->dst - 1].op != PLAN_MOVE);
        assert(m->dst + m->count == rows || o->rows[m->dst + m->count].op != PLAN_MOVE);
        moved += m->count;
    }
    run_moves(rows, o->moves, o->n_moves, canvas);
    int moves_seen = 0;
    for (int r = 0; r < rows; r++) {
        if (o->rows[r].op == PLAN_PAINT)
            continue;
        int src = o->rows[r].op == PLAN_MOVE ? o->rows[r].src : r;
        moves_seen += o->rows[r].op == PLAN_MOVE;
        for (int c = 0; c < CELLS; c++)
            assert(canvas[r][c] == src * CELLS + c);
    }
    assert(moves_seen == moved);
    free(canvas);

    /* Rule 6: windows by row, then by x, inside the grid, on kept rows. */
    for (int i = 0; i < o->n_windows; i++) {
        const struct plan_window *w = &o->windows[i];
        assert(w->row >= 0 && w->row <= rows);
        assert(!plan_span_empty(w->x) && w->x.x0 >= 0 && w->x.x1 <= f->grid.width);
        assert(w->row == rows || o->rows[w->row].op != PLAN_PAINT);
        if (i) {
            const struct plan_window *p = &o->windows[i - 1];
            assert(p->row < w->row || (p->row == w->row && p->x.x1 < w->x.x0));
        }
    }
    assert(o->n_windows <= PLAN_WINDOWS_MAX(rows));

    /* Rule 5: regions in the canvas and aligned to the rows. */
    struct rect2i canvas_rect = { .size = f->canvas };
    assert(o->ui.n >= 0 && o->ui.n <= UI_DAMAGE_MAX_RECTS);
    int grid_end = f->grid.y + f->grid.height;
    int rows_end = f->grid.y + rows * f->ch;
    for (int i = 0; i < o->ui.n; i++) {
        struct rect2i q = o->ui.rects[i];
        assert(!rect2i_is_empty(q) && contains(canvas_rect, q));
        int edges[2] = { q.y, q.y + q.height };
        for (int e = 0; e < 2; e++) {
            int y = edges[e];
            if (y > f->grid.y && y < grid_end)
                assert(y == rows_end || (y < rows_end && (y - f->grid.y) % f->ch == 0));
        }
    }
    if (o->ui.all)
        assert(o->ui.n == 1 && rect2i_eq(o->ui.rects[0], canvas_rect));

    /* Rules 5 and 7: painted and moved bands are in a region and damaged. */
    for (int r = 0; r < rows; r++) {
        if (o->rows[r].op == PLAN_KEEP)
            continue;
        struct rect2i b = band(f, r, r + 1);
        bool in_region = o->ui.all;
        for (int i = 0; i < o->ui.n; i++)
            in_region |= contains(o->ui.rects[i], b);
        assert(in_region);
        for (int y = b.y; y < b.y + b.height; y++)
            for (int x = b.x; x < b.x + b.width; x++)
                assert(damage_has_pixel(&o->damage, x, y));
    }
    assert(o->damage.n >= 0 && o->damage.n <= DAMAGE_RECTS_MAX);
    for (int i = 0; i < o->damage.n; i++)
        assert(contains(canvas_rect, o->damage.rects[i]));
}

static void plan_run(struct fixture *f)
{
    memset(f->out.rows, 0xA5, (f->rows > 0 ? (size_t)f->rows : 1) * sizeof(*f->out.rows));
    memset(f->out.moves, 0xA5, (f->rows > 0 ? (size_t)f->rows : 1) * sizeof(*f->out.moves));
    memset(&f->out.ui, 0xA5, sizeof(f->out.ui));
    memset(&f->out.damage, 0xA5, sizeof(f->out.damage));
    f->out.n_moves = f->out.n_windows = f->out.shift = -7;
    f->out.all = true;
    plan_frame(&f->in, &f->out);
    if (!f->in.invalid)
        check_valid_plan(f);
}

/* --- The pixel model --- */

struct model_op {
    struct rect2i rect;
    struct rect2i clip;
    bool clipped;
    bool invisible;
    uint32_t id;
};

#define MODEL_OPS 8
struct model_scene {
    struct model_op ops[MODEL_OPS];
    int n;
};

struct px {
    uint64_t content;
    uint32_t ui;
    uint32_t layers;
};

static struct rect2i op_ink(const struct model_op *op)
{
    return op->clipped ? rect2i_intersect(op->rect, op->clip) : op->rect;
}

static uint32_t scene_ink(const struct model_scene *sc, int x, int y)
{
    for (int i = sc->n - 1; i >= 0; i--) {
        const struct model_op *op = &sc->ops[i];
        if (!op->invisible && rect2i_contains(op_ink(op), vec(x, y)))
            return op->id;
    }
    return 0;
}

static void scene_build(const struct model_scene *sc, struct ui_scene *s)
{
    ui_scene_init(s);
    for (int i = 0; i < sc->n; i++) {
        const struct model_op *op = &sc->ops[i];
        if (op->clipped)
            assert(ui_scene_push_clip(s, op->clip));
        if (op->invisible)
            assert(ui_scene_blend_rect(s, op->rect, 0xFF000000u | op->id, 0));
        else
            assert(ui_scene_rect(s, op->rect, 0xFF000000u | op->id));
        if (op->clipped)
            assert(ui_scene_pop_clip(s));
    }
    assert(ui_scene_is_valid(s));
}

/* What a fresh paint of one frame shows. */
static void model_fresh(const struct fixture *f, const uint64_t *content,
                        const struct model_scene *sc, struct px *out)
{
    for (int y = 0; y < f->canvas.y; y++) {
        for (int x = 0; x < f->canvas.x; x++) {
            struct px p = { .content = BASE_ID };
            if (rect2i_contains(f->grid, vec(x, y))) {
                int r = row_of(f, y);
                p.content = r < f->rows ? content[r] : MARGIN_ID;
            }
            p.ui = scene_ink(sc, x, y);
            p.layers = p.ui ? 1 : 0;
            out[(size_t)y * f->canvas.x + x] = p;
        }
    }
}

static void model_fill(const struct fixture *f, struct px *canvas, struct rect2i r, struct px p)
{
    if (!rect2i_clip(&r, f->canvas))
        return;
    for (int y = r.y; y < r.y + r.height; y++)
        for (int x = r.x; x < r.x + r.width; x++)
            canvas[(size_t)y * f->canvas.x + x] = p;
}

/* The executor of render_draw(), on the model canvas. */
static void model_execute(const struct fixture *f, const uint64_t *content,
                          const struct model_scene *sc, const struct ui_scene *scene,
                          struct px *canvas)
{
    const struct plan_output *o = &f->out;
    int w = f->canvas.x;

    struct px *tmp = malloc(((size_t)f->grid.width * (size_t)f->grid.height + 1) * sizeof(*tmp));
    assert(tmp);
    for (int i = 0; i < o->n_moves; i++) {
        const struct plan_move *m = &o->moves[i];
        int lines = m->count * f->ch;
        int sy = f->grid.y + m->src * f->ch, dy = f->grid.y + m->dst * f->ch;
        for (int l = 0; l < lines; l++)
            memcpy(tmp + (size_t)l * f->grid.width, canvas + (size_t)(sy + l) * w + f->grid.x,
                   (size_t)f->grid.width * sizeof(*tmp));
        for (int l = 0; l < lines; l++)
            memcpy(canvas + (size_t)(dy + l) * w + f->grid.x, tmp + (size_t)l * f->grid.width,
                   (size_t)f->grid.width * sizeof(*tmp));
    }
    free(tmp);

    for (int r = 0; r < f->rows; r++)
        if (o->rows[r].op == PLAN_PAINT)
            model_fill(f, canvas, band(f, r, r + 1), (struct px){ .content = content[r] });
    if (o->all)
        model_fill(f, canvas, band(f, f->rows, f->rows + 1), (struct px){ .content = MARGIN_ID });
    for (int i = 0; i < o->n_windows; i++) {
        const struct plan_window *win = &o->windows[i];
        struct rect2i b = band(f, win->row, win->row + 1);
        b.x = f->grid.x + win->x.x0;
        b.width = win->x.x1 - win->x.x0;
        uint64_t c = win->row < f->rows ? content[win->row] : MARGIN_ID;
        model_fill(f, canvas, b, (struct px){ .content = c });
    }

    struct rect2i canvas_rect = { .size = f->canvas };
    struct rect2i bounds = ui_scene_bounds(scene);
    int n = o->ui.all ? 1 : o->ui.n;
    for (int i = 0; i < n; i++) {
        struct rect2i q = o->ui.all ? canvas_rect : o->ui.rects[i];
        if (!rect2i_clip(&q, f->canvas))
            continue;
        for (int y = q.y; y < q.y + q.height; y++) {
            for (int x = q.x; x < q.x + q.width; x++) {
                struct px *p = &canvas[(size_t)y * w + x];
                if (!rect2i_contains(f->grid, vec(x, y)))
                    *p = (struct px){ .content = BASE_ID };
                uint32_t id = rect2i_contains(bounds, vec(x, y)) ? scene_ink(sc, x, y) : 0;
                if (id) {
                    p->ui = id;
                    p->layers++;
                }
            }
        }
    }
}

/* The content that a row shows: its key, or a unique value for key 0. */
static void row_content(const uint64_t *keys, int rows, uint64_t tag, uint64_t *content)
{
    for (int r = 0; r < rows; r++)
        content[r] = keys[r] ? keys[r] : tag + (uint64_t)r;
}

/*
 * Plans the step from (old keys, old scene) to (new keys, new scene) and
 * checks the executor model against a fresh paint and the damage.
 */
static void model_check(struct fixture *f, const struct model_scene *old_sc,
                        const struct model_scene *new_sc)
{
    struct ui_scene old_scene = {0}, new_scene = {0};
    scene_build(old_sc, &old_scene);
    scene_build(new_sc, &new_scene);
    plan_ui_ink(&old_scene, f->grid, f->ch, f->rows, f->old_ui);
    plan_run(f);

    size_t rows = f->rows > 0 ? (size_t)f->rows : 1;
    uint64_t *old_content = malloc(rows * sizeof(*old_content));
    uint64_t *new_content = malloc(rows * sizeof(*new_content));
    size_t n = (size_t)f->canvas.x * (size_t)f->canvas.y;
    struct px *before = malloc(n * sizeof(*before));
    struct px *canvas = malloc(n * sizeof(*canvas));
    struct px *fresh = malloc(n * sizeof(*fresh));
    assert(old_content && new_content && before && canvas && fresh);
    row_content(f->old_keys, f->rows, UINT64_C(1) << 40, old_content);
    row_content(f->new_keys, f->rows, UINT64_C(1) << 41, new_content);

    model_fresh(f, old_content, old_sc, before);
    memcpy(canvas, before, n * sizeof(*canvas));
    model_execute(f, new_content, new_sc, &new_scene, canvas);
    model_fresh(f, new_content, new_sc, fresh);
    for (int y = 0; y < f->canvas.y; y++) {
        for (int x = 0; x < f->canvas.x; x++) {
            size_t i = (size_t)y * f->canvas.x + x;
            if (canvas[i].content != fresh[i].content || canvas[i].ui != fresh[i].ui ||
                canvas[i].layers != fresh[i].layers) {
                fprintf(stderr, "pixel (%d, %d): got %llx/%u/%u, want %llx/%u/%u\n", x, y,
                        (unsigned long long)canvas[i].content, canvas[i].ui, canvas[i].layers,
                        (unsigned long long)fresh[i].content, fresh[i].ui, fresh[i].layers);
                assert(!"incremental plan differs from a fresh paint");
            }
            if (memcmp(&before[i], &fresh[i], sizeof(fresh[i])) != 0 &&
                !damage_has_pixel(&f->out.damage, x, y)) {
                fprintf(stderr, "pixel (%d, %d) changed outside the damage\n", x, y);
                assert(!"damage misses a changed pixel");
            }
        }
    }
    free(old_content);
    free(new_content);
    free(before);
    free(canvas);
    free(fresh);
    ui_scene_destroy(&old_scene);
    ui_scene_destroy(&new_scene);
}

static bool op_eq(const struct model_op *a, const struct model_op *b)
{
    return rect2i_eq(a->rect, b->rect) && rect2i_eq(a->clip, b->clip) &&
           a->clipped == b->clipped && a->invisible == b->invisible && a->id == b->id;
}

static void damage_scene_change(struct ui_damage *d, const struct model_scene *a,
                                const struct model_scene *b)
{
    *d = (struct ui_damage){0};
    int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        const struct model_op *oa = i < a->n ? &a->ops[i] : NULL;
        const struct model_op *ob = i < b->n ? &b->ops[i] : NULL;
        if (oa && ob && op_eq(oa, ob))
            continue;
        if (oa)
            ui_damage_add(d, op_ink(oa));
        if (ob)
            ui_damage_add(d, op_ink(ob));
    }
}

static const struct model_scene no_scene;

/* --- Planner cases --- */

static void idle_keeps_every_row_test(void)
{
    struct fixture *f = standard(10);
    keys_idle(f);
    f->old_ui[3] = span(5, 30);
    plan_run(f);
    for (int r = 0; r < 10; r++)
        assert(f->out.rows[r].op == PLAN_KEEP);
    assert(f->out.n_moves == 0 && f->out.n_windows == 0 && f->out.shift == 0);
    assert(f->out.ui.n == 0 && !f->out.ui.all);
    assert(!f->out.damage.any && f->out.damage.n == 0);
    fixture_free(f);
}

static void typing_paints_one_row_test(void)
{
    struct fixture *f = standard(10);
    keys_idle(f);
    f->new_keys[4] = 99;
    plan_run(f);
    for (int r = 0; r < 10; r++)
        assert(f->out.rows[r].op == (r == 4 ? PLAN_PAINT : PLAN_KEEP));
    assert(f->out.n_moves == 0 && f->out.shift == 0);
    assert(f->out.damage.n == 1 && rect2i_eq(f->out.damage.rects[0], band(f, 4, 5)));
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], band(f, 4, 5)));
    model_check(f, &no_scene, &no_scene);
    fixture_free(f);
}

/* Rows with key 0 are unknown: every frame paints them, even when both frames
 * hold 0, and a scroll never moves one onto another. */
static void unknown_key_paints_test(void)
{
    struct fixture *f = standard(10);
    keys_idle(f);
    f->old_keys[2] = f->new_keys[2] = 0;
    f->old_keys[3] = f->new_keys[3] = 0;
    plan_run(f);
    for (int r = 0; r < 10; r++)
        assert(f->out.rows[r].op == (r == 2 || r == 3 ? PLAN_PAINT : PLAN_KEEP));
    model_check(f, &no_scene, &no_scene);
    fixture_free(f);

    f = standard(10);
    keys_idle(f);
    for (int r = 0; r < 10; r++)
        f->new_keys[r] = r + 1 < 10 ? f->old_keys[r + 1] : 0;
    f->old_keys[4] = 0;
    f->new_keys[3] = 0; /* shows old row 4, which was unknown */
    plan_run(f);
    assert(f->out.rows[3].op == PLAN_PAINT && f->out.rows[9].op == PLAN_PAINT);
    model_check(f, &no_scene, &no_scene);
    fixture_free(f);
}

static void scroll_test(int k)
{
    int rows = 10;
    for (int dir = -1; dir <= 1; dir += 2) {
        struct fixture *f = standard(rows);
        keys_idle(f);
        int shift = dir * k;
        for (int r = 0; r < rows; r++) {
            int src = r - shift;
            f->new_keys[r] = src >= 0 && src < rows ? f->old_keys[src] : 100 + (uint64_t)r;
        }
        plan_run(f);
        assert(f->out.shift == shift);
        assert(f->out.n_moves == 1);
        struct plan_move m = f->out.moves[0];
        assert(m.count == rows - k);
        assert(dir < 0 ? (m.dst == 0 && m.src == k) : (m.dst == k && m.src == 0));
        for (int r = 0; r < rows; r++) {
            bool fresh = dir < 0 ? r >= rows - k : r < k;
            assert(f->out.rows[r].op == (fresh ? PLAN_PAINT : PLAN_MOVE));
        }
        assert(f->out.damage.n == 1 && rect2i_eq(f->out.damage.rects[0], band(f, 0, rows)));
        model_check(f, &no_scene, &no_scene);
        fixture_free(f);
    }
}

static void scroll_by_one_and_by_k_test(void)
{
    scroll_test(1);
    scroll_test(3);
    scroll_test(9);
}

static void scroll_region_test(void)
{
    /* Rows 2..7 scroll; rows 0, 1, 8, and 9 stay. */
    for (int dir = -1; dir <= 1; dir += 2) {
        struct fixture *f = standard(10);
        keys_idle(f);
        for (int r = 2; r < 8; r++) {
            int src = r - dir;
            f->new_keys[r] = src >= 2 && src < 8 ? f->old_keys[src] : 77;
        }
        plan_run(f);
        assert(f->out.shift == dir);
        for (int r = 0; r < 10; r++) {
            enum plan_row_op want = r < 2 || r >= 8 ? PLAN_KEEP
                : (dir < 0 ? r == 7 : r == 2) ? PLAN_PAINT : PLAN_MOVE;
            assert(f->out.rows[r].op == want);
        }
        assert(f->out.n_moves == 1 && f->out.moves[0].count == 5);
        assert(f->out.moves[0].dst == (dir < 0 ? 2 : 3));
        assert(f->out.damage.n == 1 && rect2i_eq(f->out.damage.rects[0], band(f, 2, 8)));
        model_check(f, &no_scene, &no_scene);
        fixture_free(f);
    }
}

/*
 * Rows 4 and 9 (or 6 and 11) repaint inside a scroll by 2, so the moves come
 * in three blocks. One canvas and memmove in plan order give the target.
 * The reverse order breaks it: so the case really tests the order.
 */
static void move_order_test(void)
{
    int rows = 16;
    for (int dir = -1; dir <= 1; dir += 2) {
        struct fixture *f = standard(rows);
        keys_idle(f);
        int shift = 2 * dir;
        for (int r = 0; r < rows; r++) {
            int src = r - shift;
            f->new_keys[r] = src >= 0 && src < rows ? f->old_keys[src] : 200 + (uint64_t)r;
        }
        int a = dir < 0 ? 4 : 6, b = dir < 0 ? 9 : 11;
        f->new_keys[a] = 300;
        f->new_keys[b] = 301;
        plan_run(f);
        assert(f->out.shift == shift && f->out.n_moves == 3);
        for (int i = 1; i < 3; i++)
            assert(dir < 0 ? f->out.moves[i].dst > f->out.moves[i - 1].dst
                           : f->out.moves[i].dst < f->out.moves[i - 1].dst);

        int canvas[16][CELLS], reversed[16][CELLS];
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < CELLS; c++)
                canvas[r][c] = reversed[r][c] = r * 10 + c;
        run_moves(rows, f->out.moves, 3, canvas);
        struct plan_move back[3] = { f->out.moves[2], f->out.moves[1], f->out.moves[0] };
        run_moves(rows, back, 3, reversed);
        bool reverse_differs = false;
        for (int r = 0; r < rows; r++) {
            if (f->out.rows[r].op != PLAN_MOVE)
                continue;
            for (int c = 0; c < CELLS; c++) {
                assert(canvas[r][c] == f->out.rows[r].src * 10 + c);
                reverse_differs |= reversed[r][c] != canvas[r][c];
            }
        }
        assert(reverse_differs);
        model_check(f, &no_scene, &no_scene);
        fixture_free(f);
    }
}

/* Sets new rows [r0, r0 + n) to the old rows [r0 - shift, ...). */
static void shift_block(struct fixture *f, int r0, int n, int shift)
{
    for (int r = r0; r < r0 + n; r++)
        f->new_keys[r] = f->old_keys[r - shift];
}

static struct fixture *fresh_rows(int rows)
{
    struct fixture *f = standard(rows);
    for (int r = 0; r < rows; r++) {
        f->old_keys[r] = (uint64_t)r + 1;
        f->new_keys[r] = 1000 + (uint64_t)r;
    }
    return f;
}

static void competing_shifts_test(void)
{
    /* The majority wins: 5 votes for -1 against 3 votes for -2. */
    struct fixture *f = fresh_rows(12);
    shift_block(f, 0, 5, -1);
    shift_block(f, 7, 3, -2);
    plan_run(f);
    assert(f->out.shift == -1);
    for (int r = 0; r < 12; r++)
        assert(f->out.rows[r].op == (r < 5 ? PLAN_MOVE : PLAN_PAINT));
    model_check(f, &no_scene, &no_scene);
    fixture_free(f);

    /* A tie goes to the smaller |shift|: +1 beats -2. */
    f = fresh_rows(12);
    shift_block(f, 1, 3, -2);
    shift_block(f, 8, 3, 1);
    plan_run(f);
    assert(f->out.shift == 1);
    for (int r = 0; r < 12; r++)
        assert(f->out.rows[r].op == (r >= 8 && r < 11 ? PLAN_MOVE : PLAN_PAINT));
    fixture_free(f);

    /* Then to the negative shift: -1 beats +1. */
    f = fresh_rows(12);
    shift_block(f, 8, 3, 1);
    shift_block(f, 0, 3, -1);
    plan_run(f);
    assert(f->out.shift == -1);
    for (int r = 0; r < 12; r++)
        assert(f->out.rows[r].op == (r < 3 ? PLAN_MOVE : PLAN_PAINT));
    model_check(f, &no_scene, &no_scene);
    fixture_free(f);

    /* One more vote beats the sign rule: +1 with 4 votes beats -1 with 3. */
    f = fresh_rows(12);
    shift_block(f, 0, 3, -1);
    shift_block(f, 7, 4, 1);
    plan_run(f);
    assert(f->out.shift == 1);
    fixture_free(f);
}

/* The terminal scrollbar: an overlay over every row that stays during a scroll. */
static void moved_row_under_overlay_test(void)
{
    struct fixture *f = standard(10);
    keys_idle(f);
    for (int r = 0; r < 10; r++)
        f->new_keys[r] = r < 9 ? f->old_keys[r + 1] : 50;
    struct model_scene bar = { .n = 1, .ops = { { .rect = rect(84, 20, 4, 100), .id = 7 } } };
    f->ui_damage = (struct ui_damage){0};
    model_check(f, &bar, &bar);
    assert(f->out.shift == -1);
    assert(f->out.n_windows == 9);
    for (int i = 0; i < 9; i++) {
        assert(f->out.windows[i].row == i);
        assert(f->out.windows[i].x.x0 == 74 && f->out.windows[i].x.x1 == 78);
    }
    fixture_free(f);

    /* A moved row repaints the old ink of its source row, not its own. */
    f = standard(10);
    keys_idle(f);
    for (int r = 0; r < 10; r++)
        f->new_keys[r] = r < 9 ? f->old_keys[r + 1] : 50;
    struct model_scene tip = { .n = 1, .ops = { { .rect = rect(30, 50, 10, 30), .id = 9 } } };
    model_check(f, &tip, &tip);
    assert(f->out.n_windows == 3);
    for (int i = 0; i < 3; i++)
        assert(f->out.windows[i].row == 2 + i && f->out.windows[i].x.x0 == 20 &&
               f->out.windows[i].x.x1 == 30);
    fixture_free(f);
}

static void vanished_overlay_test(void)
{
    struct fixture *f = standard(10);
    keys_idle(f);
    struct model_scene popup = { .n = 1, .ops = { { .rect = rect(30, 45, 20, 30), .id = 3 } } };
    damage_scene_change(&f->ui_damage, &popup, &no_scene);
    model_check(f, &popup, &no_scene);
    for (int r = 0; r < 10; r++)
        assert(f->out.rows[r].op == PLAN_KEEP);
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], rect(30, 40, 20, 40)));
    assert(f->out.n_windows == 4);
    for (int i = 0; i < 4; i++)
        assert(f->out.windows[i].row == 2 + i && f->out.windows[i].x.x0 == 20 &&
               f->out.windows[i].x.x1 == 40);
    assert(f->out.damage.n == 1 && rect2i_eq(f->out.damage.rects[0], rect(30, 45, 20, 30)));
    fixture_free(f);
}

static void new_overlay_needs_no_restore_test(void)
{
    struct fixture *f = standard(10);
    keys_idle(f);
    struct model_scene popup = { .n = 1, .ops = { { .rect = rect(30, 45, 20, 30), .id = 3 } } };
    damage_scene_change(&f->ui_damage, &no_scene, &popup);
    model_check(f, &no_scene, &popup);
    assert(f->out.n_windows == 0);
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], rect(30, 40, 20, 40)));
    fixture_free(f);

    /* A popup that moves restores only where the old one was. */
    f = standard(10);
    keys_idle(f);
    struct model_scene moved = { .n = 1, .ops = { { .rect = rect(60, 45, 20, 30), .id = 3 } } };
    damage_scene_change(&f->ui_damage, &popup, &moved);
    model_check(f, &popup, &moved);
    assert(f->out.n_windows == 4);
    for (int i = 0; i < 4; i++)
        assert(f->out.windows[i].x.x0 == 20 && f->out.windows[i].x.x1 == 40);
    fixture_free(f);
}

static void bottom_margin_test(void)
{
    /* Grid rows end at y 120. The margin is [120, 126). */
    struct fixture *f = standard(10);
    keys_idle(f);
    struct model_scene over = { .n = 1, .ops = { { .rect = rect(20, 121, 20, 4), .id = 5 } } };
    damage_scene_change(&f->ui_damage, &over, &no_scene);
    model_check(f, &over, &no_scene);
    assert(f->old_ui[10].x0 == 10 && f->old_ui[10].x1 == 30);
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], rect(20, 120, 20, 6)));
    assert(f->out.n_windows == 1 && f->out.windows[0].row == 10);
    assert(f->out.windows[0].x.x0 == 10 && f->out.windows[0].x.x1 == 30);
    fixture_free(f);

    /* A toast over the last row and the margin, gone. */
    f = standard(10);
    keys_idle(f);
    struct model_scene toast = { .n = 1, .ops = { { .rect = rect(40, 115, 30, 20), .id = 6 } } };
    damage_scene_change(&f->ui_damage, &toast, &no_scene);
    model_check(f, &toast, &no_scene);
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], rect(40, 110, 30, 25)));
    assert(f->out.n_windows == 2);
    assert(f->out.windows[0].row == 9 && f->out.windows[1].row == 10);
    fixture_free(f);

    /* A grid smaller than one cell is all margin. */
    f = fixture_new(vec(60, 40), rect(5, 10, 50, 6), 0, 10);
    struct model_scene dot = { .n = 1, .ops = { { .rect = rect(8, 12, 4, 2), .id = 2 } } };
    damage_scene_change(&f->ui_damage, &dot, &no_scene);
    model_check(f, &dot, &no_scene);
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], rect(8, 10, 4, 6)));
    assert(f->out.n_windows == 1 && f->out.windows[0].row == 0);
    fixture_free(f);
}

static void ui_regions_snap_to_rows_test(void)
{
    struct fixture *f = standard(10);
    keys_idle(f);
    f->ui_damage = (struct ui_damage){0};
    ui_damage_add(&f->ui_damage, rect(60, 2, 30, 5));   /* above the grid */
    ui_damage_add(&f->ui_damage, rect(0, 25, 5, 10));   /* left of the grid, rows 0 and 1 */
    ui_damage_add(&f->ui_damage, rect(20, 55, 10, 4));  /* inside row 3 */
    ui_damage_add(&f->ui_damage, rect(50, 10, 10, 15)); /* from above into row 0 */
    ui_damage_add(&f->ui_damage, rect(70, 123, 5, 12)); /* from the margin to below */
    plan_run(f);
    assert(f->out.ui.n == 5);
    assert(ui_has_rect(&f->out.ui, rect(60, 2, 30, 5)));
    assert(ui_has_rect(&f->out.ui, rect(0, 20, 5, 20)));
    assert(ui_has_rect(&f->out.ui, rect(20, 50, 10, 10)));
    assert(ui_has_rect(&f->out.ui, rect(50, 10, 10, 20)));
    assert(ui_has_rect(&f->out.ui, rect(70, 120, 5, 15)));
    /* The damage is the rects as given. */
    assert(f->out.damage.bounds.y == 2 && f->out.damage.bounds.height == 133);
    fixture_free(f);

    /* Two rects that touch after the snap merge into one aligned rect. */
    f = standard(10);
    keys_idle(f);
    f->ui_damage = (struct ui_damage){0};
    ui_damage_add(&f->ui_damage, rect(20, 33, 10, 2));
    ui_damage_add(&f->ui_damage, rect(25, 41, 10, 2));
    plan_run(f);
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], rect(20, 30, 15, 20)));
    fixture_free(f);

    /*
     * A window repaints the whole height of its row. The region grows to
     * the row, so the UI also repaints the op that stays in that row.
     */
    f = standard(10);
    keys_idle(f);
    struct model_scene before = { .n = 2, .ops = {
        { .rect = rect(20, 40, 20, 3), .id = 1 },
        { .rect = rect(20, 45, 20, 5), .id = 2 },
    } };
    struct model_scene after = { .n = 1, .ops = { before.ops[0] } };
    damage_scene_change(&f->ui_damage, &before, &after);
    model_check(f, &before, &after);
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], rect(20, 40, 20, 10)));
    assert(f->out.n_windows == 1 && f->out.windows[0].row == 2);
    fixture_free(f);
}

static void null_ui_damage_test(void)
{
    struct fixture *f = standard(10);
    keys_idle(f);
    f->new_keys[6] = 66;
    struct model_scene old_sc = { .n = 2, .ops = {
        { .rect = rect(84, 20, 4, 100), .id = 7 },
        { .rect = rect(15, 118, 40, 8), .id = 8 },
    } };
    struct model_scene new_sc = { .n = 1, .ops = { { .rect = rect(30, 40, 20, 20), .id = 9 } } };
    f->in.ui_damage = NULL;
    model_check(f, &old_sc, &new_sc);
    struct rect2i canvas = { .size = f->canvas };
    assert(f->out.ui.all && f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], canvas));
    assert(f->out.damage.all ||
           (f->out.damage.n == 1 && rect2i_eq(f->out.damage.rects[0], canvas)));
    /* Every kept row with old ink and the margin get a window; row 6 paints. */
    assert(f->out.n_windows == 10);
    for (int i = 0; i < f->out.n_windows; i++)
        assert(f->out.windows[i].row != 6);
    fixture_free(f);
}

static void damage_overflow_test(void)
{
    struct fixture *f = standard(20);
    keys_idle(f);
    for (int r = 1; r < 20; r += 2)
        f->new_keys[r] = 500 + (uint64_t)r;
    plan_run(f);
    assert(f->out.damage.any && !f->out.damage.all);
    assert(f->out.damage.n == 1);
    assert(rect2i_eq(f->out.damage.rects[0], f->out.damage.bounds));
    assert(rect2i_eq(f->out.damage.bounds, band(f, 1, 20)));
    assert(f->out.ui.n == 10);
    model_check(f, &no_scene, &no_scene);
    fixture_free(f);
}

static void no_grid_test(void)
{
    struct fixture *f = fixture_new(vec(50, 30), (struct rect2i){0}, 0, 10);
    struct model_scene menu = { .n = 1, .ops = { { .rect = rect(5, 5, 20, 13), .id = 4 } } };
    damage_scene_change(&f->ui_damage, &no_scene, &menu);
    model_check(f, &no_scene, &menu);
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], rect(5, 5, 20, 13)));
    assert(f->out.n_windows == 0);
    assert(f->out.damage.n == 1 && rect2i_eq(f->out.damage.rects[0], rect(5, 5, 20, 13)));
    fixture_free(f);
}

/* --- Invalidation --- */

static bool needs_full_paint(const struct render_surface *s, const struct render_surface_key *key,
                             bool fresh, bool repaint)
{
    return !s->valid || fresh || repaint || !render_surface_key_eq(&s->key, key);
}

static struct render_surface_key base_key(void)
{
    return (struct render_surface_key){
        .canvas = vec(100, 140),
        .grid = rect(10, 20, 80, 106),
        .cells = vec(10, 10),
        .cell = vec(8, 10),
        .font_version = 3,
        .palette_gen = 4,
        .default_fg = 0xFFEEEEEE,
        .default_bg = 0xFF101010,
        .cursor_color = 0xFFFF8800,
        .selection_fg = 0xFFFFFFFF,
        .selection_bg = 0xFF585B70,
        .base = 0xFF202020,
    };
}

static void check_full_plan(struct fixture *f)
{
    f->in.invalid = true;
    plan_run(f);
    struct rect2i canvas = { .size = f->canvas };
    assert(f->out.all && f->out.damage.all && f->out.damage.any && f->out.ui.all);
    assert(f->out.ui.n == 1 && rect2i_eq(f->out.ui.rects[0], canvas));
    assert(f->out.damage.n == 1 && rect2i_eq(f->out.damage.rects[0], canvas));
    assert(f->out.n_moves == 0 && f->out.n_windows == 0 && f->out.shift == 0);
    for (int r = 0; r < f->rows; r++)
        assert(f->out.rows[r].op == PLAN_PAINT);
}

static void invalidation_test(void)
{
    struct framebuffer_history history = {0};
    struct render_surface *s = render_surface_of(&history);
    assert(s && !s->valid);
    struct render_surface_key key = base_key();
    struct fixture *f = standard(10);
    keys_idle(f);
    f->old_ui[2] = span(0, 10);

    /* No state yet. */
    assert(needs_full_paint(s, &key, false, false));
    check_full_plan(f);
    model_check(f, &no_scene, &no_scene);

    assert(render_surface_reserve(s, 10));
    render_surface_commit(s, &key, 10, f->new_keys, f->old_ui, NULL);
    assert(s->valid && !needs_full_paint(s, &key, false, false));

    /* A fresh target and the repaint flag. */
    assert(needs_full_paint(s, &key, true, false));
    assert(needs_full_paint(s, &key, false, true));

    /* Each key field. */
    struct render_surface_key k;
#define CHANGED(stmt) \
    do { \
        k = base_key(); \
        stmt; \
        assert(!render_surface_key_eq(&key, &k) && !render_surface_key_eq(&k, &key)); \
        assert(needs_full_paint(s, &k, false, false)); \
    } while (0)
    CHANGED(k.canvas.x++);
    CHANGED(k.canvas.y++);
    CHANGED(k.grid.x++);
    CHANGED(k.grid.y++);
    CHANGED(k.grid.width--);
    CHANGED(k.grid.height--);
    CHANGED(k.cells.x--);
    CHANGED(k.cells.y--);
    CHANGED(k.cell.x++);
    CHANGED(k.cell.y++);
    CHANGED(k.font_version++);
    CHANGED(k.palette_gen++);
    CHANGED(k.default_fg ^= 1);
    CHANGED(k.default_bg ^= 1);
    CHANGED(k.cursor_color ^= 1);
    CHANGED(k.selection_fg ^= 1);
    CHANGED(k.selection_bg ^= 1);
    CHANGED(k.base ^= 1);
    CHANGED(k.reverse_video = true);
#undef CHANGED
    k = base_key();
    assert(render_surface_key_eq(&key, &k));

    /* A forgotten canvas. */
    render_surface_invalidate(s);
    assert(needs_full_paint(s, &key, false, false));

    /* Every reason gives the same plan: everything paints. */
    check_full_plan(f);
    for (int r = 0; r < 10; r++)
        f->new_keys[r] = f->old_keys[(r + 1) % 10];
    check_full_plan(f);
    f->in.ui_damage = NULL;
    check_full_plan(f);

    framebuffer_history_release(&history);
    assert(!history.data && !history.destroy);
    fixture_free(f);
}

/* --- plan_ui_ink --- */

static void ui_ink_test(void)
{
    struct rect2i grid = rect(10, 20, 80, 106);
    struct plan_span ink[11];
    struct ui_scene s = {0};

    for (int r = 0; r <= 10; r++)
        ink[r] = span(1, 2);
    plan_ui_ink(NULL, grid, 10, 10, ink);
    for (int r = 0; r <= 10; r++)
        assert(plan_span_empty(ink[r]));

    ui_scene_init(&s);
    assert(ui_scene_rect(&s, rect(0, 35, 30, 10), 0xFF0000FF));    /* rows 1, 2; left clip */
    assert(ui_scene_rect(&s, rect(60, 41, 10, 1), 0x80FFFFFF));    /* row 2 */
    assert(ui_scene_rect(&s, rect(40, 70, 10, 10), 0x00FFFFFF));   /* alpha 0 */
    assert(ui_scene_blend_rect(&s, rect(40, 80, 10, 10), 0xFFFFFFFF, 0));
    assert(ui_scene_circle(&s, vec(45, 95), 3, 0xFFFFFFFF, 0));
    assert(ui_scene_blend_rect(&s, rect(41, 100, 3, 3), 0xFFFFFFFF, 1)); /* row 8 */
    assert(ui_scene_rect(&s, rect(0, 0, 100, 15), 0xFFFFFFFF));    /* above the grid */
    assert(ui_scene_push_clip(&s, rect(20, 60, 30, 100)));
    assert(ui_scene_push_clip(&s, rect(0, 0, 25, 65)));
    assert(ui_scene_rect(&s, rect(0, 50, 100, 30), 0xFFFFFFFF));   /* row 4, x [20, 25) */
    assert(ui_scene_pop_clip(&s));
    assert(ui_scene_rect(&s, rect(0, 118, 100, 30), 0xFFFFFFFF));  /* row 9, margin; x [20, 50) */
    assert(ui_scene_pop_clip(&s));
    assert(ui_scene_is_valid(&s));
    plan_ui_ink(&s, grid, 10, 10, ink);
    assert(plan_span_empty(ink[0]));
    assert(ink[1].x0 == 0 && ink[1].x1 == 20);
    assert(ink[2].x0 == 0 && ink[2].x1 == 60);
    assert(plan_span_empty(ink[3]));
    assert(ink[4].x0 == 10 && ink[4].x1 == 15);
    assert(plan_span_empty(ink[5]) && plan_span_empty(ink[6]) && plan_span_empty(ink[7]));
    assert(ink[8].x0 == 31 && ink[8].x1 == 34);
    assert(ink[9].x0 == 10 && ink[9].x1 == 40);
    assert(ink[10].x0 == 10 && ink[10].x1 == 40);

    /* Text ink is one advance wider on each side than its cells. */
    ui_scene_init(&s);
    assert(ui_scene_text_run(&s, rect(30, 30, 0, 10),
                             (struct ui_text_style){ .advance = 6, .cell_h = 10 },
                             0xFFFFFFFF, "abc", 3));
    assert(ui_scene_text_run(&s, rect(30, 60, 0, 10),
                             (struct ui_text_style){ .advance = 6, .cell_h = 10 },
                             0x00FFFFFF, "abc", 3));
    plan_ui_ink(&s, grid, 10, 10, ink);
    assert(ink[1].x0 == 14 && ink[1].x1 == 44);
    assert(plan_span_empty(ink[4]));

    /* An invalid scene gives no ink. */
    ui_scene_init(&s);
    assert(ui_scene_rect(&s, rect(10, 20, 10, 10), 0xFFFFFFFF));
    assert(ui_scene_push_clip(&s, rect(0, 0, 100, 100)));
    assert(!ui_scene_is_valid(&s));
    plan_ui_ink(&s, grid, 10, 10, ink);
    for (int r = 0; r <= 10; r++)
        assert(plan_span_empty(ink[r]));

    /* No grid rows: all ink is margin ink. */
    ui_scene_init(&s);
    assert(ui_scene_rect(&s, rect(12, 21, 3, 3), 0xFFFFFFFF));
    plan_ui_ink(&s, rect(10, 20, 80, 6), 10, 0, ink);
    assert(ink[0].x0 == 2 && ink[0].x1 == 5);
    ui_scene_destroy(&s);
}

/* --- Random steps through the pixel model --- */

static uint64_t rng = 0x2545F4914F6CDD1Dull;

static int rnd(int n)
{
    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
    return n > 0 ? (int)((rng >> 33) % (uint64_t)n) : 0;
}

static struct rect2i random_rect(struct vector2i canvas)
{
    int x = rnd(canvas.x + 8) - 4, y = rnd(canvas.y + 8) - 4;
    return rect(x, y, 1 + rnd(canvas.x / 2 + 2), 1 + rnd(canvas.y / 2 + 2));
}

static struct model_op random_op(struct vector2i canvas, uint32_t id)
{
    struct model_op op = { .rect = random_rect(canvas), .id = id };
    op.clipped = rnd(4) == 0;
    if (op.clipped)
        op.clip = random_rect(canvas);
    op.invisible = rnd(8) == 0;
    return op;
}

static void random_scenes(const struct fixture *f, struct model_scene *a, struct model_scene *b)
{
    static uint32_t next_id = 1;
    *a = (struct model_scene){ .n = rnd(4) };
    for (int i = 0; i < a->n; i++)
        a->ops[i] = random_op(f->canvas, next_id++);
    if (rnd(3) == 0 && a->n < MODEL_OPS) {
        /* A scrollbar over the right edge of the grid. */
        a->ops[a->n++] = (struct model_op){
            .rect = rect(f->grid.x + f->grid.width - 3, f->grid.y, 3, f->grid.height),
            .id = next_id++,
        };
    }
    *b = *a;
    for (int i = 0; i < b->n; i++) {
        switch (rnd(5)) {
        case 0:
            b->ops[i].rect = rect2i_translate(b->ops[i].rect, vec(rnd(9) - 4, rnd(9) - 4));
            break;
        case 1:
            b->ops[i].id = next_id++;
            break;
        case 2:
            b->ops[i] = b->ops[--b->n];
            break;
        default:
            break;
        }
    }
    while (b->n < MODEL_OPS && rnd(3) == 0)
        b->ops[b->n++] = random_op(f->canvas, next_id++);
}

static void random_keys(struct fixture *f)
{
    static uint64_t next_key = 1;
    int rows = f->rows;
    for (int r = 0; r < rows; r++)
        f->old_keys[r] = rnd(12) ? next_key++ : 0;
    if (rows > 1 && rnd(10) == 0)
        f->old_keys[rnd(rows)] = f->old_keys[rnd(rows)];
    for (int r = 0; r < rows; r++)
        f->new_keys[r] = f->old_keys[r];
    switch (rows ? rnd(6) : 0) {
    case 1: /* typing */
        for (int i = 1 + rnd(3); i > 0; i--)
            f->new_keys[rnd(rows)] = next_key++;
        break;
    case 2:
    case 3: { /* scroll regions */
        for (int n = 1 + rnd(2); n > 0; n--) {
            int top = rnd(rows), bot = top + 1 + rnd(rows - top);
            int k = 1 + rnd(bot - top), shift = rnd(2) ? k : -k;
            uint64_t tmp[64];
            for (int r = top; r < bot; r++) {
                int src = r - shift;
                tmp[r] = src >= top && src < bot ? f->new_keys[src] : next_key++;
            }
            for (int r = top; r < bot; r++)
                f->new_keys[r] = tmp[r];
        }
        break;
    }
    case 4: /* tab switch */
        for (int r = 0; r < rows; r++)
            f->new_keys[r] = next_key++;
        break;
    case 5: /* rows taken from anywhere, some twice */
        for (int r = 0; r < rows; r++)
            f->new_keys[r] = rnd(3) ? f->old_keys[rnd(rows)] : next_key++;
        break;
    default:
        break;
    }
    for (int r = 0; r < rows; r++)
        if (rnd(16) == 0)
            f->new_keys[r] = rnd(2) ? 0 : next_key++;
}

static void random_model_test(void)
{
    for (int iter = 0; iter < 3000; iter++) {
        int rows = rnd(15), ch = 1 + rnd(6);
        int grid_h = rows * ch + rnd(ch + 3);
        struct rect2i grid = rect(rnd(10), rnd(10), 1 + rnd(40), grid_h);
        if (grid_h == 0)
            grid = (struct rect2i){0};
        struct vector2i canvas = vec(grid.x + grid.width + rnd(10) + 1,
                                     grid.y + grid.height + rnd(10) + 1);
        struct fixture *f = fixture_new(canvas, grid, rows, ch);
        random_keys(f);
        struct model_scene a, b;
        random_scenes(f, &a, &b);
        damage_scene_change(&f->ui_damage, &a, &b);
        if (rnd(4) == 0)
            ui_damage_add(&f->ui_damage, random_rect(canvas));
        if (rnd(10) == 0)
            f->in.ui_damage = NULL;
        f->in.invalid = rnd(20) == 0;
        model_check(f, &a, &b);
        fixture_free(f);
    }
}

/* --- Surface state --- */

static void surface_state_test(void)
{
    struct framebuffer_history history = {0};
    struct render_surface *s = render_surface_of(&history);
    assert(s && history.data == s && history.destroy == render_surface_destroy);
    assert(render_surface_of(&history) == s);
    assert(!s->valid && s->rows == 0);

    assert(render_surface_reserve(s, 5) && s->cap >= 5);
    struct render_surface_key key = base_key();
    uint64_t keys[5] = { 11, 12, 13, 14, 15 };
    struct plan_span ui[6] = { span(1, 2), {0}, {0}, span(3, 9), {0}, span(0, 80) };
    uint16_t reach[5] = { 0, 2, 0, 0, 7 };
    render_surface_commit(s, &key, 5, keys, ui, reach);
    assert(s->valid && s->rows == 5 && render_surface_key_eq(&s->key, &key));
    assert(!memcmp(s->keys, keys, sizeof(keys)));
    assert(!memcmp(s->ui, ui, sizeof(ui)));
    assert(!memcmp(s->reach, reach, sizeof(reach)));

    /* Steady state: the same or fewer rows keep the memory. */
    uint64_t *k0 = s->keys;
    struct plan_span *u0 = s->ui;
    uint16_t *r0 = s->reach;
    assert(render_surface_reserve(s, 5) && render_surface_reserve(s, 2));
    assert(s->keys == k0 && s->ui == u0 && s->reach == r0 && s->valid);
    render_surface_commit(s, &key, 2, keys, ui, reach);
    assert(s->rows == 2 && s->ui[2].x0 == 0 && s->ui[2].x1 == 0);

    /* Growing keeps the contents. */
    render_surface_commit(s, &key, 5, keys, ui, reach);
    assert(render_surface_reserve(s, 40) && s->cap >= 40);
    assert(!memcmp(s->keys, keys, sizeof(keys)) && !memcmp(s->ui, ui, sizeof(ui)) &&
           !memcmp(s->reach, reach, sizeof(reach)));

    render_surface_invalidate(s);
    assert(!s->valid && s->cap >= 40 && s->keys);

    /* Zero rows still record the margin. */
    render_surface_commit(s, &key, 0, NULL, ui, NULL);
    assert(s->valid && s->rows == 0 && s->ui[0].x0 == 1 && s->ui[0].x1 == 2);

    framebuffer_history_release(&history);
    assert(!history.data);

    /* An embedded surface, such as the scratch surface. */
    struct render_surface scratch;
    render_surface_init(&scratch);
    assert(!scratch.valid && !scratch.keys);
    assert(render_surface_reserve(&scratch, 0) && scratch.ui);
    render_surface_commit(&scratch, &key, 0, NULL, NULL, NULL);
    assert(scratch.valid && plan_span_empty(scratch.ui[0]));
    render_surface_release(&scratch);
    assert(!scratch.valid && !scratch.keys && !scratch.ui && !scratch.reach && !scratch.cap);

    /* History data of another owner is not a surface. */
    int other = 0;
    struct framebuffer_history foreign = { .data = &other };
    assert(!render_surface_of(&foreign));
    assert(!render_surface_of(NULL));
}

static void row_key_test(void)
{
    struct terminal_frame f = { .rows = STUB_ROWS, .cols = 20, .cursor_y = -1 };
    memset(stub_selection, 0, sizeof(stub_selection));
    for (int r = 0; r < STUB_ROWS; r++)
        stub_content[r] = 0x1000 + (uint64_t)r;
    stub_content[5] = 0;

    uint64_t plain[STUB_ROWS];
    for (int r = 0; r < STUB_ROWS; r++) {
        plain[r] = render_surface_row_key(&f, r);
        /* A row with an unknown content key keeps key 0: it never matches. */
        assert((plain[r] == 0) == (r == 5));
        assert(render_surface_row_key(&f, r) == plain[r]);
        for (int q = 0; q < r; q++)
            assert(plain[q] != plain[r] || plain[r] == 0);
    }

    /* The selection span of a row is part of its key. */
    stub_selection[2] = (struct wsap_vtcore_span){ .begin = 3, .end = 9 };
    assert(render_surface_row_key(&f, 2) == plain[2]); /* has_selection is false */
    f.has_selection = true;
    uint64_t sel = render_surface_row_key(&f, 2);
    assert(sel != plain[2] && sel != 0);
    assert(render_surface_row_key(&f, 1) == plain[1]);
    stub_selection[2].end = 10;
    assert(render_surface_row_key(&f, 2) != sel);
    stub_selection[2] = (struct wsap_vtcore_span){0};
    assert(render_surface_row_key(&f, 2) == plain[2]);

    /* The cursor x and style are part of the key of the cursor row only. */
    f.cursor_y = 4;
    f.cursor_x = 7;
    uint64_t cur = render_surface_row_key(&f, 4);
    assert(cur != plain[4] && cur != 0);
    f.cursor_y = 5;
    assert(render_surface_row_key(&f, 5) == 0); /* the cursor does not make it known */
    f.cursor_y = 4;
    assert(render_surface_row_key(&f, 3) == plain[3]);
    f.cursor_x = 8;
    assert(render_surface_row_key(&f, 4) != cur);
    f.cursor_x = 7;
    f.cursor_style = WSAP_VTCORE_CURSOR_BAR;
    assert(render_surface_row_key(&f, 4) != cur);
    f.cursor_y = -1;
    assert(render_surface_row_key(&f, 4) == plain[4]);

    /* The key follows the content when the content moves. */
    uint64_t moved = stub_content[6];
    stub_content[1] = moved;
    assert(render_surface_row_key(&f, 1) == plain[6]);
}

int main(void)
{
    idle_keeps_every_row_test();
    typing_paints_one_row_test();
    unknown_key_paints_test();
    scroll_by_one_and_by_k_test();
    scroll_region_test();
    move_order_test();
    competing_shifts_test();
    moved_row_under_overlay_test();
    vanished_overlay_test();
    new_overlay_needs_no_restore_test();
    bottom_margin_test();
    ui_regions_snap_to_rows_test();
    null_ui_damage_test();
    damage_overflow_test();
    no_grid_test();
    invalidation_test();
    ui_ink_test();
    random_model_test();
    surface_state_test();
    row_key_test();
    printf("render-plan-test: ok\n");
    return 0;
}
