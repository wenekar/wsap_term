#include "ui/damage.h"
#include "util/grow.h"

static int find_record(const struct ui_damage_tracker *t, uint64_t key, int hint)
{
    if (hint < t->n && !t->records[hint].used && t->records[hint].key == key)
        return hint;
    for (int j = 0; j < t->n; j++)
        if (!t->records[j].used && t->records[j].key == key)
            return j;
    return -1;
}

static void remember(struct ui_damage_tracker *t, const struct ui_scene *s)
{
    if (!UTIL_GROW(t->records, t->cap, s->n_scopes)) {
        ui_damage_tracker_reset(t);
        return;
    }
    t->n = s->n_scopes;
    for (int i = 0; i < s->n_scopes; i++)
        t->records[i] = (struct ui_damage_record){
            .key = s->scopes[i].key,
            .hash = s->scopes[i].hash,
            .ink = s->scopes[i].ink,
        };
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
    *t = (struct ui_damage_tracker){0};
}
