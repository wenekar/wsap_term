#ifndef WSAP_PAINT_FRAMEBUFFER_H
#define WSAP_PAINT_FRAMEBUFFER_H

#include <stdbool.h>
#include <stdint.h>

#include "util/damage.h"
#include "util/geom.h"

#define FRAMEBUFFER_BUFFERS 4

/* Render defines the GPU type. Paint keeps it opaque. */
struct framebuffer_gpu;

/* Persistent render state owned by one surface. A framebuffer copy keeps
 * the pointer to this state. */
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

/* One frame to draw into. */
struct framebuffer {
    uint32_t *pixels;
    struct vector2i size;
    /* The index of this surface buffer. */
    int buffer;
    /* The pixels start transparent and hold nothing to keep. */
    bool cleared;
    /* The pixels do not hold the last frame painted into them. */
    bool stale;
    /* Where painting records damage. NULL: render_draw() keeps its own. */
    struct damage_list *damage;
    /* Surface lends the native drawable until the frame callback ends. */
    void *drawable;
    bool transaction;
    bool presented;
    struct framebuffer_history *history;
    /* NULL draws into the pixels only. A caller that needs the pixels,
     * such as a screenshot, sets it to NULL before drawing. */
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
