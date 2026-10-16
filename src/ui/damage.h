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

/* This tracker remembers the scopes from the last scene. */
struct ui_damage_tracker {
    struct ui_damage_record *records;
    int n, cap;
    bool valid;
};

void ui_damage_track(struct ui_damage_tracker *t, const struct ui_scene *s,
                     struct ui_damage *out);
void ui_damage_tracker_reset(struct ui_damage_tracker *t);
void ui_damage_tracker_destroy(struct ui_damage_tracker *t);

#endif
