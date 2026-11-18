#ifndef APP_RUNTIME_H
#define APP_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "paint/scene.h"
#include "util/geom.h"

struct app_runtime;
struct font_instance;
struct renderer;
struct wsap_vtcore;
struct yasl_loop;

/* The runtime owns the renderer and the frame state that must outlive one
 * window. It sits above render and ui_yasl, so the app and the window host
 * share it. */
struct app_runtime *app_runtime_create(struct renderer *renderer,
                                       struct yasl_loop *loop);
void app_runtime_destroy(struct app_runtime *rt);
/* The renderer of the runtime. The runtime keeps its lifetime. */
struct renderer *app_runtime_renderer(struct app_runtime *rt);

/* Starts the idle timer after a GPU frame. The timer frees the GPU memory
 * when the renderer is idle. */
void app_runtime_note_gpu_draw(struct app_runtime *rt, uint64_t now_ms);
/* Reports that the first main frame is done. The runtime trims the heap
 * once after it. */
void app_runtime_note_first_frame(struct app_runtime *rt);
void app_runtime_trim_heap(void);

/* The scene-to-image capture callback of a tab preview. */
bool app_runtime_paint_scene(void *ud, struct font_instance *font,
                             uint32_t *pixels, struct vector2i size,
                             const struct ui_scene *scene);

/* The thumbnail callback of a tab preview. */
bool app_runtime_thumbnail_capture(void *ud, void **state,
                                   struct font_instance *font,
                                   struct wsap_vtcore *term, int width,
                                   struct ui_image *image);
void app_runtime_thumbnail_release(void *state);

#endif
