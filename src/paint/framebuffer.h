#ifndef WSAP_PAINT_FRAMEBUFFER_H
#define WSAP_PAINT_FRAMEBUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "util/damage.h"
#include "util/geom.h"

#define FRAMEBUFFER_BUFFERS 4

struct framebuffer_gpu;

struct framebuffer_history {
    void *data;
    void (*destroy)(void *);
    void *gpu_data;
    void (*gpu_destroy)(void *);
};

static inline void framebuffer_history_release(struct framebuffer_history *history)
{
    if (!history)
        return;
    if (history->gpu_data && history->gpu_destroy)
        history->gpu_destroy(history->gpu_data);
    if (history->data && history->destroy)
        history->destroy(history->data);
    *history = (struct framebuffer_history){0};
}

struct framebuffer {
    uint32_t *pixels;
    struct vector2i size;
    int buffer;
    bool cleared;
    bool stale;
    struct damage_list *damage;
    void *drawable;
    bool transaction;
    bool presented;
    struct framebuffer_history *history;
    struct framebuffer_gpu *gpu;
};

static inline void framebuffer_damage(struct framebuffer *t, struct rect2i r)
{
    damage_add(t->damage, r);
}

static inline void framebuffer_damage_all(struct framebuffer *t)
{
    damage_add_all(t->damage);
}

#endif
