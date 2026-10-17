#ifndef WSAP_PAINT_FRAMEBUFFER_H
#define WSAP_PAINT_FRAMEBUFFER_H

/*
 * The canvas that render_draw() paints, and the renderer state of that
 * canvas. One surface has one canvas and one history.
 */

#include <stdbool.h>
#include <stdint.h>

#include "util/geom.h"

/*
 * Renderer state for one surface.
 *
 * - The window owns this struct. It keeps it while the surface lives.
 * - The renderer owns the memory that `data` points to.
 * - An all-zero history is valid. It holds no state.
 * - render_draw() sets `data` and `destroy` on first use.
 * - One history belongs to one canvas. Do not share it between canvases.
 * - Call framebuffer_history_release() when the surface goes away.
 *   The renderer does not have to be alive at that time.
 */
struct framebuffer_history {
    void *data;
    void (*destroy)(void *);
};

static inline void framebuffer_history_release(struct framebuffer_history *history)
{
    if (!history)
        return;
    if (history->data && history->destroy)
        history->destroy(history->data);
    *history = (struct framebuffer_history){0};
}

/*
 * One canvas for one frame.
 *
 * - `pixels` holds size.x * size.y words in 0xAARRGGBB order, row after
 *   row. The row stride is size.x.
 * - NULL pixels or an empty size paint nothing. render_draw() then reports
 *   full damage and forgets the surface state.
 * - `fresh` means: the pixels are not what render_draw() last drew with
 *   this history. The renderer then paints every pixel. Set it on the first
 *   frame, after a reallocation, and after a failed present. False is the
 *   normal case. A size change also forces a full paint, so a resize does
 *   not need `fresh`.
 * - `history` NULL means: paint everything and keep no state. Use it for
 *   one-shot images, such as thumbnails and drag images.
 * - `drawable` is the GPU drawable of this frame, or NULL. NULL means a CPU
 *   canvas that the window presents itself.
 * - `transaction` means: present the drawable inside the Core Animation
 *   transaction of a live resize.
 */
struct framebuffer {
    uint32_t *pixels;
    struct vector2i size;
    bool fresh;
    struct framebuffer_history *history;
    void *drawable;
    bool transaction;
};

#endif
