#include "compat.h"
#include "screen.h"
#include "unicode/grapheme.h"
#include <stdlib.h>
#include <string.h>

struct wsap_vtcore_snapshot {
    struct wsap_vtcore_view view;
    uint32_t palette[WSAP_VTCORE_PALETTE_SIZE];
    uint64_t source;

    struct wsap_vtcore_cell *cells;
    struct wsap_vtcore_cell *stage;
    uint64_t *keys;
    uint32_t *row_clusters;
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
    free(s->cells);
    free(s->stage);
    free(s->keys);
    free(s->row_clusters);
    free(s->selection);
    free(s->arena);
    free(s->spare);
    free(s);
}

static bool reserve(struct wsap_vtcore_snapshot *s, int cols, int rows)
{
    size_t cells = (size_t)cols * (size_t)rows;
    if (cells > s->cell_cap) {
        struct wsap_vtcore_cell *c = realloc(s->cells, cells * sizeof(*c));
        if (!c)
            return false;
        s->cells = c;
        struct wsap_vtcore_cell *st = realloc(s->stage, cells * sizeof(*st));
        if (!st)
            return false;
        s->stage = st;
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
        if (!k || !rc || !sel)
            return false;
        s->row_cap = rows;
    }
    return true;
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

static bool convert_row(struct arena_out *a, const struct gstore *g,
                        struct wsap_vtcore_cell *dst, const struct cell *src, int cols)
{
    for (int x = 0; x < cols; x++) {
        const struct cell *c = &src[x];
        uint16_t f = c->flags;
        uint32_t text = c->c;
        if (WSAP_VTCORE_EXPECT(text < 0x20, 0))
            text = text ? ' ' : 0;
        else if (WSAP_VTCORE_EXPECT(wsap_vtcore_grapheme_is_handle(text), 0) &&
                 !arena_add(a, g, text, &text))
            return false;
        dst[x] = (struct wsap_vtcore_cell){
            .text = text,
            .fg = c->fg,
            .bg = c->bg,
            .attrs = (uint16_t)(f & ~(CELL_WIDE | CELL_TRAIL | CELL_WRAP_PAD)),
            .width = (f & CELL_TRAIL) ? 0 : (f & CELL_WIDE) ? 2 : 1,
        };
    }
    return true;
}

#define ROW_KEPT UINT32_MAX
#define DIRECT_MAX_WORDS (1u << 16)

struct plan {
    bool all;
    int scrolled;
    uint32_t arena_len;
    const uint64_t *keys;
    const uint32_t *words;
    int staged;
    bool direct;
    const int *c0, *c1;
};

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
    }
    s->arena_len = p->arena_len;

    bool changed = p->all;
    size_t row_bytes = (size_t)cols * sizeof(struct wsap_vtcore_cell);
    bool swap = !p->direct && (k > 0 || p->staged * 2 > rows);
    if (p->direct && k > 0)
        memmove(s->cells, s->cells + (size_t)k * cols, (size_t)(rows - k) * row_bytes);
    if (swap) {
        for (int y = 0; y < rows; y++)
            if (p->words[y] == ROW_KEPT)
                memcpy(s->stage + (size_t)y * cols, s->cells + (size_t)(y + k) * cols, row_bytes);
        struct wsap_vtcore_cell *cells = s->cells;
        s->cells = s->stage;
        s->stage = cells;
    }
    if (k > 0) {
        for (int y = 0; y < k; y++)
            s->arena_dead += s->row_clusters[y];
        memmove(s->row_clusters, s->row_clusters + k,
                (size_t)(rows - k) * sizeof(uint32_t));
        memmove(s->keys, s->keys + k,
                (size_t)(rows - k) * sizeof(uint64_t));
        memset(s->keys + (rows - k), 0, (size_t)k * sizeof(uint64_t));
        memset(s->row_clusters + (rows - k), 0, (size_t)k * sizeof(uint32_t));
    }

    for (int y = 0; y < rows; y++) {
        if (p->words[y] == ROW_KEPT)
            continue;
        if (!p->all)
            s->arena_dead += s->row_clusters[y];
        if (p->direct) {
            struct arena_out a = { &s->arena, &s->arena_cap, s->arena_len };
            (void)convert_row(&a, t->gstore, s->cells + (size_t)y * cols,
                              wsap_vtcore_screen_view_row(t, y), cols);
            s->row_clusters[y] = a.len - s->arena_len;
            s->arena_len = a.len;
        } else {
            if (!swap)
                memcpy(s->cells + (size_t)y * cols, s->stage + (size_t)y * cols, row_bytes);
            s->row_clusters[y] = p->words[y];
        }
        s->keys[y] = p->keys[y];
        changed = true;
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
    uint32_t stack_words[256];
    int stack[2][256];
    uint64_t *new_keys = stack_keys;
    uint32_t *words = stack_words;
    int *c0 = stack[0], *c1 = stack[1];
    void *heap = NULL;
    if (rows > 256) {
        heap = malloc((size_t)rows * (sizeof(*new_keys) + sizeof(*words) + 2 * sizeof(*c0)));
        if (!heap)
            return false;
        new_keys = heap;
        words = (uint32_t *)(new_keys + rows);
        c0 = (int *)(words + rows);
        c1 = c0 + rows;
    }
    struct plan p = { .keys = new_keys, .words = words, .c0 = c0, .c1 = c1 };

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
    for (int y = 0; y < rows; y++)
        new_keys[y] = wsap_vtcore_screen_view_row_key(t, y);

    if (!all && rows > 1) {
        int cand_k = 0;
        uint64_t key0 = new_keys[0];
        if (key0) {
            for (int k = 1; k < rows; k++) {
                if (s->keys[k] == key0) {
                    cand_k = k;
                    break;
                }
            }
        }
        if (!cand_k && rows > 2) {
            uint64_t key1 = new_keys[1];
            if (key1) {
                for (int k = 1; 1 + k < rows; k++) {
                    if (s->keys[1 + k] == key1) {
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
                if (new_keys[y] == s->keys[y + cand_k]) {
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
        if (!all && y + scrolled < rows && s->keys[y + scrolled] == new_keys[y]) {
            words[y] = ROW_KEPT;
        } else {
            words[y] = 0;
            p.staged++;
        }
    }

    struct arena_out a = all ? (struct arena_out){ &s->spare, &s->spare_cap, 0 }
                             : (struct arena_out){ &s->arena, &s->arena_cap, s->arena_len };
    uint64_t worst = (uint64_t)p.staged * (uint64_t)cols * (1 + WSAP_VTCORE_GRAPHEME_MAX_LEN);
    if (!all && worst <= DIRECT_MAX_WORDS) {
        // No cluster handle or history row can add arena data in the empty state.
        if ((wsap_vtcore_gstore_count(t->gstore) != 0 || t->hist.lines != 0) &&
            !arena_reserve(&a, a.len + worst))
            goto done;
        p.direct = true;
    } else {
        for (int y = 0; y < rows; y++) {
            if (words[y] == ROW_KEPT)
                continue;
            uint32_t before = a.len;
            if (!convert_row(&a, t->gstore, s->stage + (size_t)y * cols,
                             wsap_vtcore_screen_view_row(t, y), cols))
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
    return s->cells + (size_t)y * s->view.size.x;
}

uint64_t wsap_vtcore_snapshot_row_key(const struct wsap_vtcore_snapshot *s, int y)
{
    if (!s || y < 0 || y >= s->view.size.y)
        return 0;
    return s->keys[y];
}

struct wsap_vtcore_span wsap_vtcore_snapshot_row_selection(const struct wsap_vtcore_snapshot *s, int y)
{
    if (!s || y < 0 || y >= s->view.size.y)
        return (struct wsap_vtcore_span){ 0 };
    return s->selection[y];
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
