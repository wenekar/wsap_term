#ifndef RENDER_BACKEND_H
#define RENDER_BACKEND_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "font/font.h"
#include "font/glyph_cache.h"

struct framebuffer;
struct wsap_vtcore;
struct wsap_vtcore_cell;
struct terminal_frame;
struct glyph;
struct ui_scene;

enum render_backend_result {
    RENDER_BACKEND_SKIP,
    RENDER_BACKEND_DREW,
    RENDER_BACKEND_FAILED,
};

/* A terminal frame for a GPU backend. The glyph calls take font. */
struct render_backend_view {
    struct font_instance *font;
    int cols, rows;
    float cell_w, cell_h;
    size_t cell_size;
    const struct wsap_vtcore_cell *(*row)(const struct terminal_frame *f, int row);
    void (*row_selection)(const struct terminal_frame *f, int row, int cols,
                          int *c0, int *c1);
    struct wsap_vtcore_cell (*resolve_cell)(const struct terminal_frame *f,
                                            const struct wsap_vtcore_cell *cells, int c,
                                            int sel_c0, int sel_c1, int cursor);
    uint64_t (*row_gen)(const struct terminal_frame *f, int row);
    uint64_t (*font_version)(const struct font_instance *fi);
    const struct glyph *(*fetch_glyph)(struct font_instance *fi, uint32_t cp, uint16_t flags);
    void (*shape_segment)(struct font_instance *fi, const uint32_t *cps, int count,
                          uint16_t flags, glyph_span_fn emit, void *ctx);
    bool (*cell_aligned_symbol)(uint32_t cp);
};

struct framebuffer_gpu;

struct framebuffer_gpu *render_gpu_create(void);
void render_gpu_destroy(struct framebuffer_gpu *g);
void *render_gpu_device(struct framebuffer_gpu *g);
bool render_gpu_begin(struct framebuffer_gpu *g, struct framebuffer *fb,
                      uint32_t base);
bool render_gpu_redraw(struct framebuffer_gpu *g);
bool render_gpu_submit(struct framebuffer_gpu *g, struct framebuffer *fb);

/* The GPU backend linked into this build defines these. The renderer
 * calls them only for a target whose gpu is set. */
enum render_backend_result render_gpu_terminal(struct framebuffer_gpu *g,
                                               const struct terminal_frame *frame,
                                               const struct render_backend_view *view);
/* Text ops draw with font. False leaves the scene to the CPU. */
bool render_gpu_ui_scene(struct framebuffer_gpu *g, const struct ui_scene *scene,
                         struct font_instance *font);

#endif
