#include "osc.h"
#include "../compat.h"
#include "../internal.h"
#include "../screen.h"
#include <stdlib.h>
#include <string.h>

#define WSAP_VTCORE_LINK_MAX 8192
#define WSAP_VTCORE_LINK_URI_MAX 4096

struct link_entry {
    uint32_t offset;
    uint16_t len;
    bool allocated;
    uint64_t hash;
};

struct link_hash_slot {
    uint64_t hash;
    uint16_t id;
};

struct wsap_vtcore_links {
    char *arena;
    size_t arena_len;
    size_t arena_cap;
    struct link_entry *entries;
    uint16_t *free_ids;
    struct link_hash_slot *hash_table;
    uint32_t hash_cap;
    uint16_t capacity;
    uint16_t free_count;
    uint16_t next;
};

static inline uint64_t hash_uri(const char *s)
{
    uint64_t h = HASH64_BASIS;
    while (*s)
        h = hash64_step(h, (uint8_t)*s++);
    h = hash64_finish(h);
    return h ? h : 1;
}

static void links_insert_hash(struct wsap_vtcore_links *l, uint64_t h, uint16_t id)
{
    if (!l->hash_table || !l->hash_cap)
        return;
    uint32_t mask = l->hash_cap - 1;
    uint32_t idx = (uint32_t)h & mask;
    for (uint32_t probe = 0; probe <= mask; probe++) {
        struct link_hash_slot *slot = &l->hash_table[idx];
        if (slot->hash == 0) {
            slot->hash = h;
            slot->id = id;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

static bool links_grow(struct wsap_vtcore_links *l)
{
    uint16_t new_cap = l->capacity == 0 ? 64 : l->capacity < 4096 ? l->capacity * 2 : WSAP_VTCORE_LINK_MAX;
    if (new_cap <= l->capacity)
        return false;
    struct link_entry *new_entries = realloc(l->entries, sizeof(*new_entries) * new_cap);
    if (!new_entries)
        return false;
    uint16_t *new_free = malloc(sizeof(uint16_t) * new_cap);
    if (!new_free) {
        l->entries = new_entries;
        return false;
    }
    uint32_t new_hash_cap = (uint32_t)new_cap * 2;
    struct link_hash_slot *new_hash = calloc(new_hash_cap, sizeof(*new_hash));
    if (!new_hash) {
        free(new_free);
        l->entries = new_entries;
        return false;
    }
    uint16_t *old_free = l->free_ids;
    l->free_ids = new_free;
    if (l->capacity)
        memcpy(new_free, old_free, sizeof(uint16_t) * l->capacity);
    free(old_free);
    memset(new_entries + l->capacity, 0, sizeof(*new_entries) * (new_cap - l->capacity));
    l->entries = new_entries;
    l->capacity = new_cap;

    free(l->hash_table);
    l->hash_table = new_hash;
    l->hash_cap = new_hash_cap;

    for (int i = 0; i + 1 < l->next; i++) {
        if (l->entries[i].allocated)
            links_insert_hash(l, l->entries[i].hash, (uint16_t)(i + 1));
    }
    return true;
}

static void links_mark_id(uint16_t id, uint64_t *mark, uint16_t cap)
{
    if (id && id <= cap)
        mark[(id - 1) >> 6] |= 1ull << ((id - 1) & 63);
}

/* Cells refer to links through the style table. Collect the unused styles
 * first. Then keep the links of the styles that stay and of the pen. */
static void links_gc(struct wsap_vtcore *t, struct wsap_vtcore_links *l)
{
    uint64_t mark[WSAP_VTCORE_LINK_MAX / 64] = { 0 };
    links_mark_id(t->cur_link, mark, l->capacity);
    wsap_vtcore_style_gc(t, mark, l->capacity);

    uint32_t dead = 0;
    for (int i = 0; i + 1 < l->next; i++) {
        if (l->entries[i].allocated && !(mark[i >> 6] & (1ull << (i & 63))))
            dead++;
    }
    if (dead == 0)
        return;

    uint32_t needed_bytes = 0;
    for (int i = 0; i + 1 < l->next; i++) {
        struct link_entry *e = &l->entries[i];
        if (e->allocated && (mark[i >> 6] & (1ull << (i & 63))))
            needed_bytes += (uint32_t)e->len + 1;
    }

    if (needed_bytes == 0) {
        free(l->arena);
        l->arena = NULL;
        l->arena_len = 0;
        l->arena_cap = 0;
    } else if (needed_bytes < l->arena_len) {
        size_t new_cap = wsap_vtcore_grow_cap(512, needed_bytes);
        char *packed = malloc(new_cap);
        if (packed) {
            uint32_t new_len = 0;
            for (int i = 0; i + 1 < l->next; i++) {
                struct link_entry *e = &l->entries[i];
                if (e->allocated && (mark[i >> 6] & (1ull << (i & 63)))) {
                    memcpy(&packed[new_len], &l->arena[e->offset], (size_t)e->len + 1);
                    e->offset = new_len;
                    new_len += (uint32_t)e->len + 1;
                }
            }
            free(l->arena);
            l->arena = packed;
            l->arena_len = new_len;
            l->arena_cap = new_cap;
        }
    }

    l->free_count = 0;
    if (l->hash_table && l->hash_cap)
        memset(l->hash_table, 0, sizeof(*l->hash_table) * l->hash_cap);

    for (int i = 0; i + 1 < l->next; i++) {
        struct link_entry *e = &l->entries[i];
        if (e->allocated) {
            if (!(mark[i >> 6] & (1ull << (i & 63)))) {
                e->allocated = false;
                e->len = 0;
                e->offset = 0;
                e->hash = 0;
                l->free_ids[l->free_count++] = (uint16_t)(i + 1);
            } else {
                links_insert_hash(l, e->hash, (uint16_t)(i + 1));
            }
        } else {
            l->free_ids[l->free_count++] = (uint16_t)(i + 1);
        }
    }
}

static uint16_t links_find(const struct wsap_vtcore_links *l, const char *uri, size_t len, uint64_t h)
{
    if (!l->hash_table || !l->hash_cap)
        return 0;
    uint32_t mask = l->hash_cap - 1;
    uint32_t idx = (uint32_t)h & mask;
    for (uint32_t probe = 0; probe <= mask; probe++) {
        const struct link_hash_slot *slot = &l->hash_table[idx];
        if (slot->hash == 0)
            return 0;
        if (slot->hash == h) {
            uint16_t id = slot->id;
            if (id && id <= l->capacity) {
                const struct link_entry *e = &l->entries[id - 1];
                if (e->allocated && e->len == (uint16_t)len &&
                    !memcmp(&l->arena[e->offset], uri, len))
                    return id;
            }
        }
        idx = (idx + 1) & mask;
    }
    return 0;
}

static uint16_t links_alloc(struct wsap_vtcore *t, struct wsap_vtcore_links *l)
{
    if (l->free_count)
        return l->free_ids[--l->free_count];
    if (l->next <= l->capacity || links_grow(l))
        return l->next++;
    links_gc(t, l);
    if (l->free_count)
        return l->free_ids[--l->free_count];
    return 0;
}

static uint16_t links_intern(struct wsap_vtcore *t, const char *uri)
{
    size_t len = strlen(uri);
    if (len == 0 || len > WSAP_VTCORE_LINK_URI_MAX)
        return 0;
    struct wsap_vtcore_links *l = t->links;
    if (!l) {
        l = calloc(1, sizeof(*l));
        if (!l)
            return 0;
        l->next = 1;
        t->links = l;
    }
    uint64_t h = hash_uri(uri);
    uint16_t id = links_find(l, uri, len, h);
    if (!id) {
        id = links_alloc(t, l);
        if (!id)
            return 0;
        size_t extra = len + 1;
        if (l->arena_len + extra > l->arena_cap) {
            size_t new_cap = wsap_vtcore_grow_cap(l->arena_cap ? l->arena_cap * 2 : 512,
                                           l->arena_len + extra);
            char *next = realloc(l->arena, new_cap);
            if (!next) {
                l->free_ids[l->free_count++] = id;
                return 0;
            }
            l->arena = next;
            l->arena_cap = new_cap;
        }
        uint32_t off = (uint32_t)l->arena_len;
        memcpy(&l->arena[off], uri, extra);
        l->arena_len += extra;

        struct link_entry *e = &l->entries[id - 1];
        e->offset = off;
        e->len = (uint16_t)len;
        e->hash = h;
        e->allocated = true;

        links_insert_hash(l, h, id);
    }
    return id;
}

void wsap_vtcore_osc8(struct wsap_vtcore *t, const char *params)
{
    const char *sep = strchr(params, ';');
    if (!sep)
        return;
    t->cur_link = links_intern(t, sep + 1);
    wsap_vtcore_style_update(t);
}

const char *wsap_vtcore_osc8_url_at(const struct wsap_vtcore *t, int col, int row)
{
    const struct wsap_vtcore_links *l = t->links;
    if (!l || !l->arena || col < 0 || col >= t->cols)
        return NULL;
    const struct cell *cells = wsap_vtcore_screen_view_row(t, row);
    if (!cells)
        return NULL;
    if (!(cells[col].flags & CELL_LINK))
        return NULL;
    const struct style_entry *e = wsap_vtcore_style_entry(t->styles, cells[col].pad);
    uint16_t id = e ? e->link : 0;
    if (!id || id > l->capacity)
        return NULL;
    const struct link_entry *entry = &l->entries[id - 1];
    if (!entry->allocated)
        return NULL;
    return &l->arena[entry->offset];
}

void wsap_vtcore_osc8_cleanup(struct wsap_vtcore *t)
{
    struct wsap_vtcore_links *l = t->links;
    if (!l)
        return;
    free(l->arena);
    free(l->entries);
    free(l->free_ids);
    free(l->hash_table);
    free(l);
    t->links = NULL;
    t->cur_link = 0;
}
