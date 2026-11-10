#include "ui/damage.h"
#include "util/grow.h"

static uint32_t key_hash(uint64_t key)
{
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdULL;
    key ^= key >> 33;
    return (uint32_t)key;
}

static int slot_find(const struct ui_damage_tracker *t, uint64_t key)
{
    if (!t->slot_cap)
        return -1;
    uint32_t mask = (uint32_t)t->slot_cap - 1;
    uint32_t i = key_hash(key) & mask;
    for (;;) {
        int slot = t->slots[i];
        if (!slot)
            return -1;
        int j = slot - 1;
        if (!t->records[j].used && t->records[j].key == key)
            return j;
        i = (i + 1) & mask;
    }
}

static void slot_insert(struct ui_damage_tracker *t, int index)
{
    uint32_t mask = (uint32_t)t->slot_cap - 1;
    uint32_t i = key_hash(t->records[index].key) & mask;
    while (t->slots[i])
        i = (i + 1) & mask;
    t->slots[i] = index + 1;
}

static int find_record(const struct ui_damage_tracker *t, uint64_t key, int hint)
{
    if (hint < t->n && !t->records[hint].used && t->records[hint].key == key)
        return hint;
    return slot_find(t, key);
}

static void remember(struct ui_damage_tracker *t, const struct ui_scene *s)
{
    if (!UTIL_GROW(t->records, t->cap, s->n_scopes) ||
        !UTIL_GROW(t->slots, t->slot_cap, s->n_scopes * 2)) {
        ui_damage_tracker_reset(t);
        return;
    }
    t->n = s->n_scopes;
    memset(t->slots, 0, (size_t)t->slot_cap * sizeof(*t->slots));
    for (int i = 0; i < s->n_scopes; i++) {
        t->records[i] = (struct ui_damage_record){
            .key = s->scopes[i].key,
            .hash = s->scopes[i].hash,
            .ink = s->scopes[i].ink,
        };
        slot_insert(t, i);
    }
    t->valid = true;
}

void ui_damage_track(struct ui_damage_tracker *t, const struct ui_scene *s,
                     struct ui_damage *out)
{
    *out = (struct ui_damage){0};
    if (!t || !ui_scene_is_valid(s)) {
        out->all = true;
        if (t)
            ui_damage_tracker_reset(t);
        return;
    }
    if (!t->valid) {
        out->all = true;
        remember(t, s);
        return;
    }
    for (int i = 0; i < s->n_scopes; i++) {
        const struct ui_scene_scope *sc = &s->scopes[i];
        int j = find_record(t, sc->key, i);
        if (j < 0) {
            ui_damage_add(out, sc->ink);
            continue;
        }
        t->records[j].used = true;
        if (t->records[j].hash != sc->hash) {
            ui_damage_add(out, t->records[j].ink);
            ui_damage_add(out, sc->ink);
        }
    }
    for (int j = 0; j < t->n; j++)
        if (!t->records[j].used)
            ui_damage_add(out, t->records[j].ink);
    remember(t, s);
}

void ui_damage_tracker_reset(struct ui_damage_tracker *t)
{
    if (t) {
        t->n = 0;
        t->valid = false;
    }
}

void ui_damage_tracker_destroy(struct ui_damage_tracker *t)
{
    if (!t)
        return;
    free(t->records);
    free(t->slots);
    *t = (struct ui_damage_tracker){0};
}
