#include "style.h"
#include "internal.h"
#include "screen.h"
#include <stdlib.h>
#include <string.h>

/* Ids go from 1 to CELL_EXT_ID. Entry zero stays unused. */
#define STYLE_MAX (CELL_EXT_ID + 1u)

static inline uint32_t style_hash(uint16_t link, uint32_t ul)
{
    return (uint32_t)hash64_finish(hash64_step(hash64_step(HASH64_BASIS, link), ul));
}

/* The slot table has twice as many slots as there are ids, so a free slot
 * always stops the probe. */
static void style_slot_insert(struct wsap_vtcore_styles *s, uint16_t id)
{
    const struct style_entry *e = &s->entry[id];
    uint32_t mask = s->slot_cap - 1;
    uint32_t i = style_hash(e->link, e->ul) & mask;
    while (s->slot[i])
        i = (i + 1) & mask;
    s->slot[i] = id;
}

static uint16_t style_find(const struct wsap_vtcore_styles *s, uint16_t link, uint32_t ul)
{
    if (!s->slot_cap)
        return 0;
    uint32_t mask = s->slot_cap - 1;
    for (uint32_t i = style_hash(link, ul) & mask;; i = (i + 1) & mask) {
        uint16_t id = s->slot[i];
        if (!id)
            return 0;
        if (s->entry[id].link == link && s->entry[id].ul == ul)
            return id;
    }
}

static bool style_grow(struct wsap_vtcore_styles *s)
{
    uint32_t cap = s->cap ? (uint32_t)s->cap * 2 : 64;
    if (cap > STYLE_MAX)
        cap = STYLE_MAX;
    if (cap <= s->cap)
        return false;
    struct style_entry *entry = realloc(s->entry, cap * sizeof(*entry));
    if (!entry)
        return false;
    s->entry = entry;
    memset(entry + s->cap, 0, (cap - s->cap) * sizeof(*entry));
    uint16_t *free_ids = realloc(s->free_ids, cap * sizeof(*free_ids));
    if (!free_ids)
        return false;
    s->free_ids = free_ids;
    uint16_t *slot = calloc((size_t)cap * 2, sizeof(*slot));
    if (!slot)
        return false;
    free(s->slot);
    s->slot = slot;
    s->slot_cap = cap * 2;
    s->cap = (uint16_t)cap;
    for (uint16_t id = 1; id < s->next; id++) {
        if (s->entry[id].live)
            style_slot_insert(s, id);
    }
    return true;
}

static uint16_t style_alloc(struct wsap_vtcore *t, struct wsap_vtcore_styles *s)
{
    if (s->free_count)
        return s->free_ids[--s->free_count];
    if (s->next < s->cap || style_grow(s))
        return s->next++;
    wsap_vtcore_style_gc(t, NULL, 0);
    if (s->free_count)
        return s->free_ids[--s->free_count];
    return 0;
}

static uint16_t style_intern(struct wsap_vtcore *t, uint16_t link, uint32_t ul)
{
    struct wsap_vtcore_styles *s = t->styles;
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s)
            return 0;
        s->next = 1;
        t->styles = s;
    }
    uint16_t id = style_find(s, link, ul);
    if (id)
        return id;
    id = style_alloc(t, s);
    if (!id)
        return 0;
    s->entry[id] = (struct style_entry){ .ul = ul, .link = link, .live = 1 };
    style_slot_insert(s, id);
    return id;
}

void wsap_vtcore_style_update(struct wsap_vtcore *t)
{
    uint16_t base = 0;
    if (t->cur_link) {
        base = style_intern(t, t->cur_link, 0);
        if (!base)
            t->cur_link = 0;
    }
    uint16_t id = base;
    if (t->cur_ul) {
        id = style_intern(t, t->cur_link, t->cur_ul);
        if (!id)
            id = base;
    }
    uint16_t flags = t->cur_pad & CELL_EXT_FLAGS;
    t->cur_pad = (uint16_t)(flags | id);
    t->base_pad = (uint16_t)((flags & CELL_EXT_PROTECTED) | base);
    pen_sync_ext(t);
}

static void style_mark(void *ud, uint16_t pad)
{
    uint64_t *bits = ud;
    uint16_t id = pad & CELL_EXT_ID;
    bits[id >> 6] |= 1ull << (id & 63);
}

static void style_mark_screen(const struct screen *scr, uint64_t *bits)
{
    for (int y = 0; y < scr->rows; y++) {
        const struct srow *row = screen_get_row(scr, y);
        if (row->len == 0)
            continue;
        if (row->kind == ROW_CELLS) {
            for (int x = 0; x < row->len; x++)
                style_mark(bits, row->cells[x].pad);
        } else {
            wsap_vtcore_hist_text_mark_links(&row->text, style_mark, bits);
        }
    }
}

void wsap_vtcore_style_gc(struct wsap_vtcore *t, uint64_t *link_mark, uint16_t link_cap)
{
    struct wsap_vtcore_styles *s = t->styles;
    if (!s || !s->slot)
        return;
    uint64_t bits[STYLE_MAX / 64] = { 0 };
    style_mark(bits, t->cur_pad);
    style_mark(bits, t->base_pad);
    style_mark_screen(&t->scr, bits);
    if (t->alternate && t->alt_allocated)
        style_mark_screen(&t->alt_scr, bits);
    wsap_vtcore_hist_mark_links(&t->hist, style_mark, bits);

    s->free_count = 0;
    memset(s->slot, 0, s->slot_cap * sizeof(*s->slot));
    for (uint16_t id = 1; id < s->next; id++) {
        struct style_entry *e = &s->entry[id];
        if (e->live && !(bits[id >> 6] & (1ull << (id & 63))))
            *e = (struct style_entry){ 0 };
        if (!e->live) {
            s->free_ids[s->free_count++] = id;
            continue;
        }
        style_slot_insert(s, id);
        if (link_mark && e->link && e->link <= link_cap)
            link_mark[(e->link - 1) >> 6] |= 1ull << ((e->link - 1) & 63);
    }
}

void wsap_vtcore_style_free(struct wsap_vtcore *t)
{
    struct wsap_vtcore_styles *s = t->styles;
    if (!s)
        return;
    free(s->entry);
    free(s->slot);
    free(s->free_ids);
    free(s);
    t->styles = NULL;
}
