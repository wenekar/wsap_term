#include "render/plan.h"

#include <stddef.h>

#include "paint/scene.h"
#include "util/hash.h"

/*
 * The clip depth that plan_ui_ink() follows. A deeper clip does not narrow
 * the ink. The ink can then be too wide, but never too narrow.
 */
#define PLAN_CLIP_DEPTH 32

/*
 * The geometry of one plan, in canvas pixels. Row `rows` is the bottom
 * margin. Row rows + 1 starts at the bottom edge of the grid rect.
 */
struct geom {
    struct rect2i canvas;
    struct rect2i grid;
    int rows;
    int ch;
    int rows_end;
    int grid_end;
};

/* Returns false when `rows` rows of height `ch` do not fit in the grid. */
static bool geom_init(struct geom *g, struct rect2i grid, int rows, int ch)
{
    if (rect2i_is_empty(grid))
        grid = (struct rect2i){0};
    int64_t height = rows > 0 && ch > 0 ? (int64_t)rows * ch : 0;
    bool fits = rows == 0 || (rows > 0 && ch > 0 && height <= grid.height);
    if (height > grid.height)
        height = grid.height;
    *g = (struct geom){
        .grid = grid,
        .rows = rows > 0 ? rows : 0,
        .ch = ch,
        .rows_end = grid.y + (int)height,
        .grid_end = grid.y + grid.height,
    };
    return fits;
}

static int row_top(const struct geom *g, int r)
{
    if (r < g->rows)
        return g->grid.y + r * g->ch;
    return r == g->rows ? g->rows_end : g->grid_end;
}

/* The row that holds canvas line y. y must be inside the grid rect. */
static int row_at(const struct geom *g, int y)
{
    return y >= g->rows_end ? g->rows : (y - g->grid.y) / g->ch;
}

/* The band of rows [r0, r1) across the grid rect. */
static struct rect2i row_band(const struct geom *g, int r0, int r1)
{
    int y0 = row_top(g, r0);
    return (struct rect2i){
        .pos = { .x = g->grid.x, .y = y0 },
        .size = { .x = g->grid.width, .y = row_top(g, r1) - y0 },
    };
}

/*
 * Grows q to whole row bands where an edge of q is inside the grid rect.
 * Only y changes. The union of two snapped rects is also snapped, so the
 * merge in ui_damage_add() keeps the rects row-aligned.
 */
static struct rect2i snap_rows(const struct geom *g, struct rect2i q)
{
    int y0 = q.y, y1 = q.y + q.height;
    if (y0 > g->grid.y && y0 < g->grid_end)
        y0 = row_top(g, row_at(g, y0));
    if (y1 > g->grid.y && y1 < g->grid_end)
        y1 = row_top(g, row_at(g, y1 - 1) + 1);
    q.y = y0;
    q.height = y1 - y0;
    return q;
}

static struct plan_span span_clip(struct plan_span s, int x0, int x1)
{
    return (struct plan_span){
        .x0 = s.x0 > x0 ? s.x0 : x0,
        .x1 = s.x1 < x1 ? s.x1 : x1,
    };
}

static struct plan_span span_union(struct plan_span a, struct plan_span b)
{
    if (plan_span_empty(a))
        return b;
    if (plan_span_empty(b))
        return a;
    return (struct plan_span){
        .x0 = a.x0 < b.x0 ? a.x0 : b.x0,
        .x1 = a.x1 > b.x1 ? a.x1 : b.x1,
    };
}

/* Rule 1. */
static void plan_all(const struct geom *g, int rows, struct plan_output *out)
{
    for (int r = 0; r < rows; r++)
        out->rows[r] = (struct plan_row){ .op = PLAN_PAINT };
    out->all = true;
    out->ui.all = true;
    damage_add_all(&out->damage);
    if (rect2i_is_empty(g->canvas))
        return;
    out->ui.rects[out->ui.n++] = g->canvas;
    damage_add(&out->damage, g->canvas);
}

static unsigned key_slot(uint64_t key, unsigned mask)
{
    return (unsigned)hash64_finish(key) & mask;
}

/*
 * Rule 3. The window array is not in use before the window pass, so the
 * vote borrows it. windows[i].row is a hash table of the old rows (row + 1;
 * 0 is empty). windows[i].x.x0 counts the votes for shift i - (rows - 1).
 * The table has fewer than 4 * rows slots and the counts take 2 * rows - 1.
 * Both fit in PLAN_WINDOWS_MAX(rows).
 */
static int vote_shift(int rows, const uint64_t *old_keys, const uint64_t *new_keys,
                      const struct plan_row *ops, struct plan_window *scratch)
{
    unsigned size = 1;
    while (size < 2u * (unsigned)rows)
        size <<= 1;
    unsigned mask = size - 1;
    for (unsigned i = 0; i < size; i++)
        scratch[i].row = 0;
    for (int r = 0; r < rows; r++) {
        if (!old_keys[r])
            continue;
        unsigned i = key_slot(old_keys[r], mask);
        while (scratch[i].row)
            i = (i + 1) & mask;
        scratch[i].row = r + 1;
    }

    for (int i = 0; i < 2 * rows - 1; i++)
        scratch[i].x.x0 = 0;
    for (int r = 0; r < rows; r++) {
        uint64_t k = new_keys[r];
        if (ops[r].op == PLAN_KEEP || !k)
            continue;
        for (unsigned i = key_slot(k, mask); scratch[i].row; i = (i + 1) & mask) {
            int src = scratch[i].row - 1;
            if (old_keys[src] == k)
                scratch[r - src + rows - 1].x.x0++;
        }
    }

    /* Smaller |shift| first, the negative one first: ties go to the first. */
    int best = 0, votes = 0;
    for (int d = 1; d < rows; d++) {
        int up = scratch[rows - 1 - d].x.x0;
        int down = scratch[rows - 1 + d].x.x0;
        if (up > votes) {
            best = -d;
            votes = up;
        }
        if (down > votes) {
            best = d;
            votes = down;
        }
    }
    return best;
}

/* Rules 2 to 4: the op of each row, the dominant shift, and the moves. */
static void plan_rows(const struct plan_input *in, int rows, struct plan_output *out)
{
    const uint64_t *old_keys = in->old_keys;
    const uint64_t *new_keys = in->new_keys;
    bool changed = false;
    for (int r = 0; r < rows; r++) {
        uint64_t k = new_keys[r];
        bool keep = k && old_keys[r] == k;
        out->rows[r] = (struct plan_row){ .op = keep ? PLAN_KEEP : PLAN_PAINT };
        changed |= !keep && k;
    }
    int shift = changed && rows > 1
        ? vote_shift(rows, old_keys, new_keys, out->rows, out->windows) : 0;
    if (!shift)
        return;

    for (int r = 0; r < rows; r++) {
        int src = r - shift;
        if (out->rows[r].op == PLAN_PAINT && new_keys[r] && src >= 0 && src < rows &&
            old_keys[src] == new_keys[r])
            out->rows[r] = (struct plan_row){ .op = PLAN_MOVE, .src = src };
    }

    int n = 0;
    for (int r = 0; r < rows;) {
        if (out->rows[r].op != PLAN_MOVE) {
            r++;
            continue;
        }
        int end = r + 1;
        while (end < rows && out->rows[end].op == PLAN_MOVE)
            end++;
        out->moves[n++] = (struct plan_move){ .dst = r, .src = r - shift, .count = end - r };
        r = end;
    }
    /*
     * Content that moves up reads below its target, so the blocks go top
     * to bottom. Content that moves down goes bottom to top. No block then
     * reads a row that an earlier block wrote.
     */
    if (shift > 0) {
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            struct plan_move t = out->moves[i];
            out->moves[i] = out->moves[j];
            out->moves[j] = t;
        }
    }
    out->n_moves = n;
    out->shift = n ? shift : 0;
}

static void add_region(const struct geom *g, struct ui_damage *ui, struct rect2i q)
{
    if (rect2i_clip(&q, g->canvas.size))
        ui_damage_add(ui, snap_rows(g, q));
}

static void add_damage(const struct geom *g, struct damage_list *d, struct rect2i r)
{
    if (rect2i_clip(&r, g->canvas.size))
        damage_add(d, r);
}

/* Rules 5 and 7: the UI regions and the output damage. */
static void plan_regions(const struct geom *g, const struct plan_input *in,
                         struct plan_output *out)
{
    const struct ui_damage *ud = in->ui_damage;
    if (!ud || ud->all) {
        /* The canvas holds every band, so it is the only region and damage. */
        out->ui.all = true;
        if (!rect2i_is_empty(g->canvas))
            out->ui.rects[out->ui.n++] = g->canvas;
        add_damage(g, &out->damage, g->canvas);
        return;
    }

    for (int r = 0; r < g->rows;) {
        if (out->rows[r].op == PLAN_KEEP) {
            r++;
            continue;
        }
        int end = r + 1;
        while (end < g->rows && out->rows[end].op != PLAN_KEEP)
            end++;
        struct rect2i band = row_band(g, r, end);
        add_region(g, &out->ui, band);
        add_damage(g, &out->damage, band);
        r = end;
    }
    int n = ud->n < 0 ? 0 : ud->n > UI_DAMAGE_MAX_RECTS ? UI_DAMAGE_MAX_RECTS : ud->n;
    for (int i = 0; i < n; i++) {
        add_region(g, &out->ui, ud->rects[i]);
        add_damage(g, &out->damage, ud->rects[i]);
    }
    if (out->damage.n < 0) {
        out->damage.n = 1;
        out->damage.rects[0] = out->damage.bounds;
    }
}

/* Sorts the windows [first, n) of one row by x and joins the ones that touch. */
static int join_row_windows(struct plan_window *w, int first, int n)
{
    for (int i = first + 1; i < n; i++) {
        struct plan_window v = w[i];
        int j = i;
        for (; j > first && w[j - 1].x.x0 > v.x.x0; j--)
            w[j] = w[j - 1];
        w[j] = v;
    }
    int end = first;
    for (int i = first; i < n; i++) {
        if (end > first && w[i].x.x0 <= w[end - 1].x.x1) {
            if (w[i].x.x1 > w[end - 1].x.x1)
                w[end - 1].x.x1 = w[i].x.x1;
        } else {
            w[end++] = w[i];
        }
    }
    return end;
}

/*
 * Rule 6. A kept or moved row holds the old ink of the row that it came
 * from. Each region that crosses the row repaints the grid where the
 * region and that old ink meet.
 */
static void plan_windows(const struct geom *g, const struct plan_input *in,
                         struct plan_output *out)
{
    int n = 0;
    for (int r = 0; r <= g->rows && out->ui.n; r++) {
        int src = r;
        if (r < g->rows) {
            if (out->rows[r].op == PLAN_PAINT)
                continue;
            if (out->rows[r].op == PLAN_MOVE)
                src = out->rows[r].src;
        }
        struct plan_span ink = span_clip(in->old_ui[src], 0, g->grid.width);
        int y0 = row_top(g, r), y1 = row_top(g, r + 1);
        if (plan_span_empty(ink) || y1 <= y0)
            continue;
        int first = n;
        for (int i = 0; i < out->ui.n; i++) {
            struct rect2i q = out->ui.rects[i];
            if (q.y >= y1 || q.y + q.height <= y0)
                continue;
            struct plan_span x = span_clip(ink, q.x - g->grid.x, q.x + q.width - g->grid.x);
            if (!plan_span_empty(x))
                out->windows[n++] = (struct plan_window){ .row = r, .x = x };
        }
        n = join_row_windows(out->windows, first, n);
    }
    out->n_windows = n;
}

void plan_frame(const struct plan_input *in, struct plan_output *out)
{
    if (!in || !out)
        return;
    out->n_moves = 0;
    out->n_windows = 0;
    out->ui = (struct ui_damage){0};
    out->damage = (struct damage_list){0};
    out->shift = 0;
    out->all = false;

    struct rect2i canvas = { .size = vector2i_max(in->canvas, vector2i_splat(0)) };
    struct geom g;
    bool fits = geom_init(&g, rect2i_intersect(in->grid, canvas), in->rows, in->ch);
    g.canvas = canvas;
    bool keys = g.rows == 0 || (in->old_keys && in->new_keys);
    if (in->invalid || !fits || !keys || !in->old_ui) {
        plan_all(&g, in->rows, out);
        return;
    }
    plan_rows(in, g.rows, out);
    plan_regions(&g, in, out);
    plan_windows(&g, in, out);
}

static bool op_draws(const struct ui_scene_op *op)
{
    switch (op->kind) {
    case UI_SCENE_OP_PUSH_CLIP:
    case UI_SCENE_OP_POP_CLIP:
        return false;
    case UI_SCENE_OP_BLEND_RECT:
    case UI_SCENE_OP_CIRCLE:
        return op->alpha != 0;
    case UI_SCENE_OP_IMAGE:
        return true;
    case UI_SCENE_OP_RECT:
    case UI_SCENE_OP_ROUNDED_RECT:
    case UI_SCENE_OP_SHADOW:
    case UI_SCENE_OP_TEXT:
        break;
    }
    return (op->color >> 24) != 0;
}

void plan_ui_ink(const struct ui_scene *scene, struct rect2i grid, int ch, int rows,
                 struct plan_span *out)
{
    if (!out || rows < 0)
        return;
    for (int r = 0; r <= rows; r++)
        out[r] = (struct plan_span){0};
    struct geom g;
    geom_init(&g, grid, rows, ch);
    if (!ui_scene_is_valid(scene) || rect2i_is_empty(g.grid))
        return;

    struct rect2i stack[PLAN_CLIP_DEPTH];
    int depth = 0;
    struct rect2i clip = g.grid;
    for (int i = 0; i < scene->n_ops; i++) {
        const struct ui_scene_op *op = &scene->ops[i];
        if (op->kind == UI_SCENE_OP_PUSH_CLIP) {
            if (depth < PLAN_CLIP_DEPTH) {
                stack[depth] = clip;
                clip = rect2i_intersect(clip, op->rect);
            }
            depth++;
            continue;
        }
        if (op->kind == UI_SCENE_OP_POP_CLIP) {
            if (depth > 0 && --depth < PLAN_CLIP_DEPTH)
                clip = stack[depth];
            continue;
        }
        if (!op_draws(op))
            continue;
        struct rect2i ink = op->kind == UI_SCENE_OP_TEXT ? ui_scene_text_ink(scene, op) : op->rect;
        ink = rect2i_intersect(ink, clip);
        if (rect2i_is_empty(ink))
            continue;
        struct plan_span x = { .x0 = ink.x - g.grid.x, .x1 = ink.x + ink.width - g.grid.x };
        int last = row_at(&g, ink.y + ink.height - 1);
        for (int r = row_at(&g, ink.y); r <= last; r++)
            out[r] = span_union(out[r], x);
    }
}
