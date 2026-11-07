#include "compat.h"
#include "screen.h"
#include "unicode/grapheme.h"
#include <stdlib.h>
#include <string.h>

struct wsap_vtcore_snapshot {
    struct wsap_vtcore_view view;
    uint32_t palette[WSAP_VTCORE_PALETTE_SIZE];
    uint64_t source;

    // The row store is a ring. Logical row y is physical row (top + y) % view.size.y.
    // A scroll moves top. The kept rows do not move.
    struct wsap_vtcore_cell *cells;
    uint64_t *keys;
    uint32_t *row_clusters;
    // For each physical row: the arena offset of its underline colors, or ROW_NO_UL.
    uint32_t *row_ul;
    int top;
    // Scratch rows for the staged path. NULL, or cell_cap cells.
    struct wsap_vtcore_cell *stage;
    // One span for each logical row.
    struct wsap_vtcore_span *selection;
    size_t cell_cap;
    int row_cap;

    uint32_t *arena;
    uint32_t arena_len, arena_cap, arena_dead;
    uint32_t *spare;
    uint32_t spare_cap;
};

struct wsap_vtcore_snapshot *wsap_vtcore_snapshot_new(void)
{
    struct wsap_vtcore_snapshot *s = calloc(1, sizeof(*s));
    if (s)
        s->view.palette = s->palette;
    return s;
}

void wsap_vtcore_snapshot_free(struct wsap_vtcore_snapshot *s)
{
    if (!s)
        return;
    wsap_vtcore_aligned_free(s->cells);
    wsap_vtcore_aligned_free(s->stage);
    free(s->keys);
    free(s->row_clusters);
    free(s->row_ul);
    free(s->selection);
    free(s->arena);
    free(s->spare);
    free(s);
}

static bool reserve(struct wsap_vtcore_snapshot *s, int cols, int rows)
{
    size_t cells = (size_t)cols * (size_t)rows;
    if (cells > s->cell_cap) {
        struct wsap_vtcore_cell *c = wsap_vtcore_aligned_alloc(64, cells * sizeof(*c));
        if (!c)
            return false;
        // Keep the old rows. The old view must stay valid if this update fails.
        if (s->cells)
            memcpy(c, s->cells, s->cell_cap * sizeof(*c));
        wsap_vtcore_aligned_free(s->cells);
        wsap_vtcore_aligned_free(s->stage);
        s->cells = c;
        s->stage = NULL;
        s->cell_cap = cells;
    }
    if (rows > s->row_cap) {
        uint64_t *k = realloc(s->keys, (size_t)rows * sizeof(*k));
        if (k)
            s->keys = k;
        uint32_t *rc = realloc(s->row_clusters, (size_t)rows * sizeof(*rc));
        if (rc)
            s->row_clusters = rc;
        struct wsap_vtcore_span *sel = realloc(s->selection, (size_t)rows * sizeof(*sel));
        if (sel)
            s->selection = sel;
        uint32_t *ul = realloc(s->row_ul, (size_t)rows * sizeof(*ul));
        if (ul)
            s->row_ul = ul;
        if (!k || !rc || !sel || !ul)
            return false;
        s->row_cap = rows;
    }
    return true;
}

static inline int ring_row(int top, int y, int rows)
{
    int p = top + y;
    return p >= rows ? p - rows : p;
}

struct arena_out {
    uint32_t **buf;
    uint32_t *cap;
    uint32_t len;
};

static bool arena_reserve(struct arena_out *a, uint64_t need)
{
    if (need > WSAP_VTCORE_TEXT_CLUSTER)
        return false;
    if (need <= *a->cap)
        return true;
    uint32_t cap = wsap_vtcore_grow_cap32(*a->cap ? *a->cap * 2 : 256, (uint32_t)need);
    uint32_t *grown = realloc(*a->buf, cap * sizeof(*grown));
    if (!grown)
        return false;
    *a->buf = grown;
    *a->cap = cap;
    return true;
}

static bool arena_add(struct arena_out *a, const struct gstore *g, uint32_t handle,
                      uint32_t *text)
{
    uint32_t cps[WSAP_VTCORE_GRAPHEME_MAX_LEN];
    int n = wsap_vtcore_gstore_expand(g, handle, cps, WSAP_VTCORE_GRAPHEME_MAX_LEN);
    if (n <= 0) {
        *text = ' ';
        return true;
    }
    uint64_t need = (uint64_t)a->len + (uint32_t)n + 1;
    if (!arena_reserve(a, need))
        return false;
    uint32_t off = a->len;
    (*a->buf)[off] = (uint32_t)n;
    memcpy(&(*a->buf)[off + 1], cps, (size_t)n * sizeof(*cps));
    a->len = (uint32_t)need;
    *text = WSAP_VTCORE_TEXT_CLUSTER | off;
    return true;
}

#define ROW_NO_UL UINT32_MAX

/* Add the underline colors of a row to the arena: one word for each column.
 * Set *ul_off to ROW_NO_UL when no cell has an underline color. */
static WSAP_VTCORE_NOINLINE bool convert_underline(struct arena_out *a, const struct wsap_vtcore_styles *st,
                                                   const struct cell *src, int cols, uint32_t *ul_off)
{
    uint64_t need = (uint64_t)a->len + (uint32_t)cols;
    if (!arena_reserve(a, need))
        return false;
    uint32_t *out = *a->buf + a->len;
    uint32_t any = 0;
    for (int x = 0; x < cols; x++) {
        const struct style_entry *e = wsap_vtcore_style_entry(st, src[x].pad);
        out[x] = e ? e->ul : WSAP_VTCORE_COLOR_NONE;
        any |= out[x];
    }
    if (any) {
        *ul_off = a->len;
        a->len = (uint32_t)need;
    }
    return true;
}

#if (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) || defined(_MSC_VER)
#define CONVERT_WORDS 1
/* The private and the public cell share the text, fg, and bg words. Two
 * 64-bit moves then convert a cell. The second word holds bg, flags, and pad
 * in, and bg, attrs, width, and reserved out. */
_Static_assert(offsetof(struct cell, fg) == offsetof(struct wsap_vtcore_cell, fg) &&
               offsetof(struct cell, bg) == offsetof(struct wsap_vtcore_cell, bg) &&
               offsetof(struct cell, flags) == 12 && offsetof(struct cell, pad) == 14 &&
               offsetof(struct wsap_vtcore_cell, attrs) == 12 &&
               offsetof(struct wsap_vtcore_cell, width) == 14 &&
               offsetof(struct wsap_vtcore_cell, reserved) == 15,
               "the cell words must line up");
_Static_assert(CELL_WIDE == 1u << 7 && CELL_TRAIL == 1u << 8, "width bits moved");

static inline WSAP_VTCORE_ALWAYS_INLINE uint64_t convert_word(uint64_t in)
{
    const uint64_t keep = 0xFFFFFFFFu | ((uint64_t)(uint16_t)~(CELL_WIDE | CELL_TRAIL | CELL_WRAP_PAD) << 32);
    const uint64_t ext = (uint64_t)(WSAP_VTCORE_ATTR_BLINK | WSAP_VTCORE_ATTR_CONCEAL) << 32;
    uint64_t wide = (in >> 39) & 1, trail = (in >> 40) & 1;
    uint64_t width = (1 + wide) >> (trail << 1);
    return (in & keep) | ((in >> (16 + CELL_EXT_ATTR_SHIFT)) & ext) | (width << 48);
}
#else
#define CONVERT_WORDS 0
#endif

/* Map a control code to a space or a blank, and a cluster handle of the
 * terminal to a handle of the snapshot. */
static inline WSAP_VTCORE_ALWAYS_INLINE bool convert_text(struct arena_out *a, const struct gstore *g,
                                                          uint32_t *text)
{
    if (*text < 0x20) {
        *text = *text ? ' ' : 0;
        return true;
    }
    uint32_t handle;
    if (!arena_add(a, g, *text, &handle))
        return false;
    *text = handle;
    return true;
}

/* Keep this inline. Out of line, the loop kept text in memory, and full
 * updates measured 20% slower. */
static inline WSAP_VTCORE_ALWAYS_INLINE bool convert_row(struct arena_out *a, const struct wsap_vtcore *t,
                                                         struct wsap_vtcore_cell *dst, const struct cell *src,
                                                         int cols, uint32_t *ul_off)
{
    // One unsigned compare finds both rare cases: a control code and a cluster handle.
    const uint32_t plain_span = WSAP_VTCORE_GRAPHEME_HANDLE_BASE - 0x20;
    // The pad fields of the row, in bits 48 to 63.
    uint64_t ext = 0;
    for (int x = 0; x < cols; x++) {
#if CONVERT_WORDS
        /* Read the cell in one load. The row is often fresh from 16-byte
         * stores, and smaller loads of it measured slower. */
        uint64_t w[2];
        memcpy(w, &src[x], sizeof(w));
        uint32_t text = (uint32_t)w[0];
        if (WSAP_VTCORE_EXPECT(text - 0x20 >= plain_span, 0)) {
            if (!convert_text(a, t->gstore, &text))
                return false;
            w[0] = (w[0] & ~(uint64_t)0xFFFFFFFFu) | text;
        }
        ext |= w[1];
        w[1] = convert_word(w[1]);
        memcpy(&dst[x], w, sizeof(w));
#else
        const struct cell *c = &src[x];
        uint16_t f = c->flags;
        uint32_t text = c->c;
        if (WSAP_VTCORE_EXPECT(text - 0x20 >= plain_span, 0) && !convert_text(a, t->gstore, &text))
            return false;
        ext |= (uint64_t)c->pad << 48;
        dst[x] = (struct wsap_vtcore_cell){
            .text = text,
            .fg = c->fg,
            .bg = c->bg,
            .attrs = (uint16_t)((f & ~(CELL_WIDE | CELL_TRAIL | CELL_WRAP_PAD)) |
                                ((c->pad >> CELL_EXT_ATTR_SHIFT) &
                                 (WSAP_VTCORE_ATTR_BLINK | WSAP_VTCORE_ATTR_CONCEAL))),
            .width = (f & CELL_TRAIL) ? 0 : (f & CELL_WIDE) ? 2 : 1,
        };
#endif
    }
    *ul_off = ROW_NO_UL;
    if (WSAP_VTCORE_EXPECT((ext >> 48) & CELL_EXT_ID, 0))
        return convert_underline(a, t->styles, src, cols, ul_off);
    return true;
}

#define ROW_KEPT UINT32_MAX
#define DIRECT_MAX_WORDS (1u << 16)

struct plan {
    bool all;
    int scrolled;
    uint32_t arena_len;
    const uint64_t *keys;
    // For each logical row: ROW_KEPT, or the arena words of a new row.
    uint32_t *words;
    // For each new staged row: the arena offset of its underline colors.
    uint32_t *ul;
    int staged;
    bool direct;
    const int *c0, *c1;
};

/* Write the plan to the snapshot. This step cannot fail. The direct path
 * converts rows here into arena room that the plan reserved. */
static void apply(struct wsap_vtcore_snapshot *s, const struct wsap_vtcore *t, const struct plan *p)
{
    int cols = t->cols, rows = t->rows;
    int k = p->scrolled;
    if (p->all) {
        uint32_t *arena = s->arena, cap = s->arena_cap;
        s->arena = s->spare;
        s->arena_cap = s->spare_cap;
        s->spare = arena;
        s->spare_cap = cap;
        s->arena_dead = 0;
        s->top = 0;
    } else if (k > 0) {
        // The rows that scroll off become the new bottom rows. No row moves.
        s->top = ring_row(s->top, k, rows);
    }
    s->arena_len = p->arena_len;

    bool changed = p->all;
    size_t row_bytes = (size_t)cols * sizeof(struct wsap_vtcore_cell);
    if (!p->direct && p->staged * 2 > rows) {
        // Most rows are new and are in the stage already. Copy the kept rows
        // to the stage and swap the buffers. The rows are then in logical order.
        for (int y = 0; y < rows; y++) {
            int phys = ring_row(s->top, y, rows);
            if (p->words[y] == ROW_KEPT) {
                memcpy(s->stage + (size_t)y * cols, s->cells + (size_t)phys * cols, row_bytes);
                p->words[y] = s->row_clusters[phys];
                p->ul[y] = s->row_ul[phys];
            } else if (!p->all) {
                s->arena_dead += s->row_clusters[phys];
            }
        }
        struct wsap_vtcore_cell *cells = s->cells;
        s->cells = s->stage;
        s->stage = cells;
        // A kept row has its old key, and the old key is equal to the new key.
        memcpy(s->keys, p->keys, (size_t)rows * sizeof(*s->keys));
        memcpy(s->row_clusters, p->words, (size_t)rows * sizeof(*s->row_clusters));
        memcpy(s->row_ul, p->ul, (size_t)rows * sizeof(*s->row_ul));
        s->top = 0;
        changed = true;
    } else {
        for (int y = 0; y < rows; y++) {
            if (p->words[y] == ROW_KEPT)
                continue;
            int phys = ring_row(s->top, y, rows);
            struct wsap_vtcore_cell *dst = s->cells + (size_t)phys * cols;
            if (!p->all)
                s->arena_dead += s->row_clusters[phys];
            if (p->direct) {
                // The plan reserved the cluster and underline words for each
                // cell of each new row, so convert_row cannot grow the arena or fail.
                struct arena_out a = { &s->arena, &s->arena_cap, s->arena_len };
                (void)convert_row(&a, t, dst, wsap_vtcore_screen_view_row(t, y), cols,
                                  &s->row_ul[phys]);
                s->row_clusters[phys] = a.len - s->arena_len;
                s->arena_len = a.len;
            } else {
                memcpy(dst, s->stage + (size_t)y * cols, row_bytes);
                s->row_clusters[phys] = p->words[y];
                s->row_ul[phys] = p->ul[y];
            }
            s->keys[phys] = p->keys[y];
            changed = true;
        }
    }

    for (int y = 0; y < rows; y++) {
        struct wsap_vtcore_span span = p->c1[y] >= p->c0[y] ? (struct wsap_vtcore_span){ p->c0[y], p->c1[y] + 1 }
                                                     : (struct wsap_vtcore_span){ 0 };
        if (p->all || span.begin != s->selection[y].begin || span.end != s->selection[y].end)
            changed = true;
        s->selection[y] = span;
    }

    struct wsap_vtcore_cursor cursor = wsap_vtcore_cursor(t);
    int view_offset = wsap_vtcore_view_offset(t);
    cursor.pos.y += view_offset;
    if (cursor.pos.y >= rows)
        cursor.visible = false;
    const struct wsap_vtcore_cursor *old = &s->view.cursor;
    if (cursor.pos.x != old->pos.x || cursor.pos.y != old->pos.y ||
        cursor.style != old->style || cursor.visible != old->visible ||
        cursor.blink != old->blink ||
        s->view.scroll_offset != view_offset ||
        s->view.reverse_video != t->reverse_video)
        changed = true;
    if (p->all || s->view.palette_version != t->palette.version) {
        memcpy(s->palette, t->palette.colors, sizeof(s->palette));
        s->view.palette_version = t->palette.version;
        changed = true;
    }

    s->source = t->serial;
    s->view.size = (struct wsap_vtcore_vector2i){ .x = cols, .y = rows };
    s->view.cursor = cursor;
    s->view.scroll_offset = view_offset;
    s->view.scrolled = k;
    s->view.reverse_video = t->reverse_video;
    if (changed || k)
        s->view.version++;
}

static bool wsap_vtcore_snapshot_update_inner(struct wsap_vtcore_snapshot *s, struct wsap_vtcore *t)
{
    if (!s || !t)
        return false;
    int cols = t->cols, rows = t->rows;
    bool ok = false;
    bool resized = s->view.size.x != cols || s->view.size.y != rows;
    bool all = resized || s->source != t->serial;
    int scrolled = 0;

    if (!reserve(s, cols, rows))
        return false;
    uint64_t stack_keys[256];
    uint32_t stack_words[2][256];
    int stack[2][256];
    uint64_t *new_keys = stack_keys;
    uint32_t *words = stack_words[0], *ul = stack_words[1];
    int *c0 = stack[0], *c1 = stack[1];
    void *heap = NULL;
    if (rows > 256) {
        heap = malloc((size_t)rows * (sizeof(*new_keys) + 2 * sizeof(*words) + 2 * sizeof(*c0)));
        if (!heap)
            return false;
        new_keys = heap;
        words = (uint32_t *)(new_keys + rows);
        ul = words + rows;
        c0 = (int *)(ul + rows);
        c1 = c0 + rows;
    }
    struct plan p = { .keys = new_keys, .words = words, .ul = ul, .c0 = c0, .c1 = c1 };

    if (s->arena_dead > 4096 && s->arena_dead * 2 > s->arena_len)
        all = true;

    if (!t->alternate) {
        wsap_vtcore_hist_prepare_rows(&t->hist, cols);
        if (t->view.anchor.active || t->view.indexed) {
            if (!wsap_vtcore_hist_view_sync(&t->view, &t->hist, cols))
                goto done;
            wsap_vtcore_hist_view_refine(&t->view, &t->hist, cols, rows);
        } else {
            uint64_t est = wsap_vtcore_hist_estimated_rows(&t->hist, cols);
            // This estimate does not build or update the row lookup arrays.
            t->view.indexed = false;
            t->view.total_rows = est;
            t->view.index_cols = cols;
            t->view.epoch = t->hist.index_epoch;
            t->view.first_id = hist_first_id(&t->hist);
            t->view.end_id = hist_end_id(&t->hist);
            t->view.head = t->hist.head;
            t->view.tail = t->hist.tail;
            t->view.last_cells = t->hist.lines ? wsap_vtcore_hist_line_cells(&t->hist, hist_end_id(&t->hist) - 1) : 0;
        }
    }
    wsap_vtcore_screen_view_row_keys(t, new_keys);

    // Find a scroll. Compare each new key with the old key k rows lower.
    int top = s->top;
    if (!all && rows > 1) {
        int cand_k = 0;
        uint64_t key0 = new_keys[0];
        if (key0) {
            for (int k = 1; k < rows; k++) {
                if (s->keys[ring_row(top, k, rows)] == key0) {
                    cand_k = k;
                    break;
                }
            }
        }
        if (!cand_k && rows > 2) {
            uint64_t key1 = new_keys[1];
            if (key1) {
                for (int k = 1; 1 + k < rows; k++) {
                    if (s->keys[ring_row(top, 1 + k, rows)] == key1) {
                        cand_k = k;
                        break;
                    }
                }
            }
        }

        if (cand_k > 0) {
            int total = rows - cand_k;
            int threshold = total / 2;
            int matches = 0, mismatches = 0;
            for (int y = 0; y < total; y++) {
                if (new_keys[y] == s->keys[ring_row(top, y + cand_k, rows)]) {
                    if (++matches > threshold) {
                        scrolled = cand_k;
                        break;
                    }
                } else {
                    if (++mismatches > threshold)
                        break;
                }
            }
        }
    }

    for (int y = 0; y < rows; y++) {
        if (!all && y + scrolled < rows && s->keys[ring_row(top, y + scrolled, rows)] == new_keys[y]) {
            words[y] = ROW_KEPT;
        } else {
            words[y] = 0;
            p.staged++;
        }
    }

    struct arena_out a = all ? (struct arena_out){ &s->spare, &s->spare_cap, 0 }
                             : (struct arena_out){ &s->arena, &s->arena_cap, s->arena_len };
    // A cluster can come only from the cluster store or from a history row.
    // Each cell of a new row then needs at most 1 + WSAP_VTCORE_GRAPHEME_MAX_LEN words.
    // An underline color needs one more word for each cell. It needs a style table.
    // The direct path converts in place, so it must reserve all of them first.
    uint64_t cell_words = 0;
    if (wsap_vtcore_gstore_count(t->gstore) > 0 || t->hist.lines > 0)
        cell_words += 1 + WSAP_VTCORE_GRAPHEME_MAX_LEN;
    if (t->styles)
        cell_words += 1;
    uint64_t worst = (uint64_t)p.staged * (uint64_t)cols * cell_words;
    // A full update takes the staged path. The direct path was not faster for it.
    if (!all && worst <= DIRECT_MAX_WORDS && arena_reserve(&a, a.len + worst)) {
        p.direct = true;
    } else {
        if (!s->stage) {
            s->stage = wsap_vtcore_aligned_alloc(64, s->cell_cap * sizeof(*s->stage));
            if (!s->stage)
                goto done;
        }
        for (int y = 0; y < rows; y++) {
            if (words[y] == ROW_KEPT)
                continue;
            uint32_t before = a.len;
            if (!convert_row(&a, t, s->stage + (size_t)y * cols,
                             wsap_vtcore_screen_view_row(t, y), cols, &ul[y]))
                goto done;
            words[y] = a.len - before;
        }
    }

    if (!wsap_vtcore_selection_view_spans(t, c0, c1))
        goto done;
    p.all = all;
    p.scrolled = scrolled;
    p.arena_len = a.len;
    apply(s, t, &p);
    ok = true;

done:
    free(heap);
    return ok;
}

bool wsap_vtcore_snapshot_update(struct wsap_vtcore_snapshot *s, struct wsap_vtcore *t)
{
    if (!s || !t)
        return false;
    wsap_vtcore_write_begin(t);
    bool ok = wsap_vtcore_snapshot_update_inner(s, t);
    wsap_vtcore_write_end(t);
    return ok;
}

const struct wsap_vtcore_view *wsap_vtcore_snapshot_view(const struct wsap_vtcore_snapshot *s)
{
    return s ? &s->view : NULL;
}

const struct wsap_vtcore_cell *wsap_vtcore_snapshot_row(const struct wsap_vtcore_snapshot *s, int y)
{
    if (!s || y < 0 || y >= s->view.size.y)
        return NULL;
    return s->cells + (size_t)ring_row(s->top, y, s->view.size.y) * s->view.size.x;
}

uint64_t wsap_vtcore_snapshot_row_key(const struct wsap_vtcore_snapshot *s, int y)
{
    if (!s || y < 0 || y >= s->view.size.y)
        return 0;
    return s->keys[ring_row(s->top, y, s->view.size.y)];
}

const wsap_vtcore_color *wsap_vtcore_snapshot_row_underline_colors(const struct wsap_vtcore_snapshot *s, int y)
{
    if (!s || y < 0 || y >= s->view.size.y)
        return NULL;
    uint32_t off = s->row_ul[ring_row(s->top, y, s->view.size.y)];
    if (off == ROW_NO_UL || off > s->arena_len || s->arena_len - off < (uint32_t)s->view.size.x)
        return NULL;
    return s->arena + off;
}

struct wsap_vtcore_span wsap_vtcore_snapshot_row_selection(const struct wsap_vtcore_snapshot *s, int y)
{
    if (!s || y < 0 || y >= s->view.size.y)
        return (struct wsap_vtcore_span){ 0 };
    return s->selection[y];
}

struct wsap_vtcore_snapshot_rows wsap_vtcore_snapshot_rows(const struct wsap_vtcore_snapshot *s)
{
    if (!s)
        return (struct wsap_vtcore_snapshot_rows){ 0 };
    return (struct wsap_vtcore_snapshot_rows){ .cells = s->cells, .keys = s->keys, .top = s->top };
}

const struct wsap_vtcore_span *wsap_vtcore_snapshot_selections(const struct wsap_vtcore_snapshot *s)
{
    return s ? s->selection : NULL;
}

int wsap_vtcore_snapshot_cluster(const struct wsap_vtcore_snapshot *s, uint32_t text, uint32_t *cps, int cap)
{
    if (!s)
        return 0;
    if (!(text & WSAP_VTCORE_TEXT_CLUSTER)) {
        if (cps && cap > 0)
            cps[0] = text;
        return 1;
    }
    uint32_t off = text & ~WSAP_VTCORE_TEXT_CLUSTER;
    if (off >= s->arena_len)
        return 0;
    uint32_t n = s->arena[off];
    if (n > WSAP_VTCORE_GRAPHEME_MAX_LEN || n > s->arena_len - off - 1)
        return 0;
    int copy = (int)n < cap ? (int)n : cap;
    if (cps && copy > 0)
        memcpy(cps, &s->arena[off + 1], (size_t)copy * sizeof(*cps));
    return (int)n;
}

const uint32_t *wsap_vtcore_snapshot_cluster_direct(const struct wsap_vtcore_snapshot *s, uint32_t text, int *out_len)
{
    if (!s || !(text & WSAP_VTCORE_TEXT_CLUSTER)) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    uint32_t off = text & ~WSAP_VTCORE_TEXT_CLUSTER;
    if (off >= s->arena_len) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    uint32_t n = s->arena[off];
    if (n > WSAP_VTCORE_GRAPHEME_MAX_LEN || n > s->arena_len - off - 1) {
        if (out_len)
            *out_len = 0;
        return NULL;
    }
    if (out_len)
        *out_len = (int)n;
    return &s->arena[off + 1];
}
