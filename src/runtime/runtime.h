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

/* The runtime borrows the renderer and the loop for its lifetime and holds
 * the frame state that must outlive one window. It sits above render and
 * ui_yasl, so the app and the window host share it.
 *
 * Returns NULL when `renderer` is NULL or the memory is missing. */
struct app_runtime *app_runtime_create(struct renderer *renderer,
                                       struct yasl_loop *loop);
void app_runtime_destroy(struct app_runtime *rt);
/* Returns the borrowed renderer, or NULL when `rt` is NULL. */
struct renderer *app_runtime_renderer(struct app_runtime *rt);

/* Records a GPU frame at `now_ms`, a monotonic time in milliseconds on the
 * clock of yasl_time_ns(), and starts the idle timer. When 500 ms have passed
 * since the last recorded frame and the renderer is idle, the timer releases
 * the idle GPU queues and trims the heap. */
void app_runtime_note_gpu_draw(struct app_runtime *rt, uint64_t now_ms);
/* Reports that the first main frame is done. The first call trims the heap;
 * later calls do nothing. */
void app_runtime_note_first_frame(struct app_runtime *rt);
void app_runtime_trim_heap(void);

/* Renders `scene` into `pixels` for the drag image of a tab, repainting
 * every pixel. `pixels` holds size.x * size.y words in 0xAARRGGBB order. `ud`
 * is the app_runtime. Returns false only when `ud` is NULL. */
bool app_runtime_paint_scene(void *ud, struct font_instance *font,
                             uint32_t *pixels, struct vector2i size,
                             const struct ui_scene *scene);

/*
 * Captures a thumbnail of `term` into `*state` and `*image`, at most `width`
 * device pixels wide. The height follows the aspect ratio of the grid.
 *
 * `*state` starts NULL. The first call allocates it, and the caller frees it
 * with app_runtime_thumbnail_release() when the preview ends, including after
 * a failed capture. The pixels of `*image` stay valid until the next capture
 * with the same `state` or until that release.
 */
bool app_runtime_thumbnail_capture(void *ud, void **state,
                                   struct font_instance *font,
                                   struct wsap_vtcore *term, int width,
                                   struct ui_image *image);
/* Frees a state that app_runtime_thumbnail_capture() allocated. NULL does
 * nothing. */
void app_runtime_thumbnail_release(void *state);

#endif
