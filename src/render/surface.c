#include "render/surface.h"

#include <stdlib.h>
#include <string.h>

#include "render/gpu/gpu.h"
#include "util/hash.h"

/* Tags that keep the selection part and the cursor part of a row key apart. */
#define ROW_KEY_SELECTION UINT64_C(0x51E5E1EC7105E1EC)
#define ROW_KEY_CURSOR UINT64_C(0xC0125012C0125012)

bool render_surface_key_eq(const struct render_surface_key *a,
                           const struct render_surface_key *b)
{
    return vector2i_eq(a->canvas, b->canvas) &&
           rect2i_eq(a->grid, b->grid) &&
           vector2i_eq(a->cells, b->cells) &&
           vector2i_eq(a->cell, b->cell) &&
           a->font_version == b->font_version &&
           a->palette_gen == b->palette_gen &&
           a->default_fg == b->default_fg &&
           a->default_bg == b->default_bg &&
           a->cursor_color == b->cursor_color &&
           a->selection_fg == b->selection_fg &&
           a->selection_bg == b->selection_bg &&
           a->base == b->base &&
           a->reverse_video == b->reverse_video;
}

struct render_surface *render_surface_of(struct framebuffer_history *history)
{
    if (!history)
        return NULL;
    /* Data that another owner put there is not a surface. */
    if (history->data)
        return history->destroy == render_surface_destroy ? history->data : NULL;
    struct render_surface *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    history->data = s;
    history->destroy = render_surface_destroy;
    return s;
}

void render_surface_init(struct render_surface *s)
{
    if (s)
        *s = (struct render_surface){0};
}

void render_surface_release(struct render_surface *s)
{
    if (!s)
        return;
    free(s->keys);
    free(s->ui);
    free(s->reach);
    render_gpu_surface_free(s->gpu);
    *s = (struct render_surface){0};
}

void render_surface_destroy(void *data)
{
    if (!data)
        return;
    render_surface_release(data);
    free(data);
}

bool render_surface_reserve(struct render_surface *s, int rows)
{
    if (!s || rows < 0)
        return false;
    /* The first call also makes the bottom margin entry of `ui`. */
    if (rows <= s->cap && s->ui)
        return true;
    int cap = rows > s->cap ? rows : s->cap;
    size_t n = cap > 0 ? (size_t)cap : 1;
    /* Each array that grows is at least cap long, so a failure keeps cap true. */
    uint64_t *keys = realloc(s->keys, n * sizeof(*keys));
    if (!keys)
        goto fail;
    s->keys = keys;
    struct plan_span *ui = realloc(s->ui, ((size_t)cap + 1) * sizeof(*ui));
    if (!ui)
        goto fail;
    s->ui = ui;
    uint16_t *reach = realloc(s->reach, n * sizeof(*reach));
    if (!reach)
        goto fail;
    s->reach = reach;
    s->cap = cap;
    return true;
fail:
    s->valid = false;
    return false;
}

void render_surface_invalidate(struct render_surface *s)
{
    if (s)
        s->valid = false;
}

uint64_t render_surface_row_key(const struct terminal_frame *f, int row)
{
    uint64_t tk = terminal_frame_row_key(f, row);
    /* An unknown row keeps key 0. The planner never matches or keeps it. */
    if (!tk)
        return 0;
    uint64_t h = hash64_finish(tk ^ HASH64_BASIS);
    if (f && f->has_selection) {
        struct wsap_vtcore_span sel = terminal_frame_row_selection(f, row);
        if (sel.end > sel.begin)
            h = hash64_finish(h ^ ROW_KEY_SELECTION ^
                              ((uint64_t)(uint32_t)sel.begin << 32 | (uint32_t)sel.end));
    }
    if (f && f->cursor_y >= 0 && f->cursor_y == row)
        h = hash64_finish(h ^ ROW_KEY_CURSOR ^
                          ((uint64_t)(uint32_t)f->cursor_x << 32 | (uint32_t)f->cursor_style));
    return h ? h : 1;
}

void render_surface_commit(struct render_surface *s, const struct render_surface_key *key,
                           int rows, const uint64_t *keys, const struct plan_span *ui,
                           const uint16_t *reach)
{
    if (!s)
        return;
    if (!key || rows < 0 || rows > s->cap || !s->ui) {
        s->valid = false;
        return;
    }
    /* A NULL array commits zeros: unknown keys, no ink, no reach. */
    size_t n = (size_t)rows;
    if (keys)
        memmove(s->keys, keys, n * sizeof(*keys));
    else if (n)
        memset(s->keys, 0, n * sizeof(*s->keys));
    if (ui)
        memmove(s->ui, ui, (n + 1) * sizeof(*ui));
    else
        memset(s->ui, 0, (n + 1) * sizeof(*s->ui));
    if (reach)
        memmove(s->reach, reach, n * sizeof(*reach));
    else if (n)
        memset(s->reach, 0, n * sizeof(*s->reach));
    s->key = *key;
    s->rows = rows;
    s->valid = true;
}
