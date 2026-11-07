#include "osc.h"
#include "../internal.h"
#include "../screen.h"
#include <stdlib.h>
#include <string.h>

#define WSAP_VTCORE_LINK_MAX 8192
#define WSAP_VTCORE_LINK_URI_MAX 4096

struct link_hash_slot {
    uint64_t hash;
    uint16_t id;
};

struct wsap_vtcore_links {
    char **uri;
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
    char **new_uri = realloc(l->uri, sizeof(char *) * new_cap);
    if (!new_uri) return false;
    uint16_t *new_free = malloc(sizeof(uint16_t) * new_cap);
    if (!new_free) {
        l->uri = new_uri;
        return false;
    }
    uint32_t new_hash_cap = (uint32_t)new_cap * 2;
    struct link_hash_slot *new_hash = calloc(new_hash_cap, sizeof(*new_hash));
    if (!new_hash) {
        free(new_free);
        l->uri = new_uri;
        return false;
    }
    uint16_t *old_free = l->free_ids;
    l->free_ids = new_free;
    if (l->capacity)
        memcpy(new_free, old_free, sizeof(uint16_t) * l->capacity);
    free(old_free);
    memset(new_uri + l->capacity, 0, sizeof(char *) * (new_cap - l->capacity));
    l->uri = new_uri;
    l->capacity = new_cap;

    free(l->hash_table);
    l->hash_table = new_hash;
    l->hash_cap = new_hash_cap;

    for (int i = 0; i + 1 < l->next; i++) {
        if (l->uri[i])
            links_insert_hash(l, hash_uri(l->uri[i]), (uint16_t)(i + 1));
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
    l->free_count = 0;
    for (int i = 0; i + 1 < l->next; i++) {
        if (l->uri[i] && !(mark[i >> 6] & (1ull << (i & 63)))) {
            free(l->uri[i]);
            l->uri[i] = NULL;
        }
        if (!l->uri[i])
            l->free_ids[l->free_count++] = (uint16_t)(i + 1);
    }
    if (l->hash_table && l->hash_cap) {
        memset(l->hash_table, 0, sizeof(*l->hash_table) * l->hash_cap);
        for (int i = 0; i + 1 < l->next; i++) {
            if (l->uri[i])
                links_insert_hash(l, hash_uri(l->uri[i]), (uint16_t)(i + 1));
        }
    }
}

static uint16_t links_find(const struct wsap_vtcore_links *l, const char *uri, uint64_t h)
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
            if (id && id <= l->capacity && l->uri[id - 1] && !strcmp(l->uri[id - 1], uri))
                return id;
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
    if (!*uri || strlen(uri) > WSAP_VTCORE_LINK_URI_MAX)
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
    uint16_t id = links_find(l, uri, h);
    if (!id) {
        id = links_alloc(t, l);
        if (!id)
            return 0;
        l->uri[id - 1] = wsap_vtcore_strdup(uri);
        if (!l->uri[id - 1]) {
            l->free_ids[l->free_count++] = id;
            return 0;
        }
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
    if (!l || col < 0 || col >= t->cols)
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
    return l->uri[id - 1];
}

void wsap_vtcore_osc8_cleanup(struct wsap_vtcore *t)
{
    struct wsap_vtcore_links *l = t->links;
    if (!l)
        return;
    for (uint16_t i = 0; i < l->capacity; i++)
        free(l->uri[i]);
    free(l->uri);
    free(l->free_ids);
    free(l->hash_table);
    free(l);
    t->links = NULL;
    t->cur_link = 0;
}
