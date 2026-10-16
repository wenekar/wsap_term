#ifndef RENDER_H
#define RENDER_H

#include <stdint.h>
#include <stdbool.h>

#include "paint/damage.h"
#include "paint/framebuffer.h"
#include "util/geom.h"
#include "render/term_frame.h"

struct font_instance;
struct renderer;
struct ui_scene;

/* All layer data for one output frame. Pointers are borrowed until
 * render_draw returns. */
struct render_frame {
    struct font_instance *font;
    struct font_instance *ui_font;
    struct rect2i terminal;
    uint32_t base;
    struct terminal_frame grid;
    bool has_grid;
    const struct ui_scene *paint;
    const struct ui_damage *damage;
    bool repaint;
    bool gpu_off;
};

struct damage_list render_draw(struct renderer *r, struct framebuffer *target,
                               const struct render_frame *frame);

struct renderer *renderer_create(void);
void renderer_destroy(struct renderer *r);
/* The native graphics device borrowed from the backend, or NULL. */
void *renderer_device(struct renderer *r);

/* Loads the fonts. Returns false when the font system fails to start.
 * The calls are counted: each successful render_init() needs one
 * render_shutdown(), and the last one stops the font system. */
bool render_init(void);
void render_shutdown(void);
void render_warm_caches(void);

void render_paint_full_terminal_frame(struct font_instance *fi, uint32_t *px,
                                      int width, int height,
                                      const struct terminal_frame *frame);

#endif
