#ifndef WSAP_UI_DAMAGE_H
#define WSAP_UI_DAMAGE_H

#include "paint/damage.h"
#include "paint/scene.h"
#include <stdint.h>

struct ui_damage_record {
    uint64_t key;
    uint64_t hash;
    struct rect2i ink;
    bool used;
};

struct ui_damage_tracker {
    struct ui_damage_record *records;
    /* Open-addressed map from a record key to the record index plus one.
       Zero marks an empty slot, so record index 0 stays representable.
       `util_grow` doubles the capacity, so it is a power of two. The
       capacity holds at least two slots for each record, so a probe always
       finds an empty slot. */
    int *slots;
    int n, cap;
    int slot_cap;
    bool valid;
};

void ui_damage_track(struct ui_damage_tracker *t, const struct ui_scene *s,
                     struct ui_damage *out);
void ui_damage_tracker_reset(struct ui_damage_tracker *t);
void ui_damage_tracker_destroy(struct ui_damage_tracker *t);

#endif
