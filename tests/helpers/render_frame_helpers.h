#ifndef WSAP_TEST_RENDER_FRAME_HELPERS_H
#define WSAP_TEST_RENDER_FRAME_HELPERS_H

/*
 * Helpers for tests that paint whole terminals through render_draw().
 *
 * - test_font(): the grid font. Set test_font_override to change it.
 * - test_render_paint_full(): the full-paint oracle. A second renderer with
 *   no cell cache and no workers paints the frame with no history.
 * - test_renderer_paint(): paints with `r` into one persistent canvas. The
 *   first frame on a new renderer, pixel buffer, or size is fresh.
 * - test_renderer_destroy(): destroys a renderer and drops its history.
 * - test_frame_helpers_fini(): frees the oracle and the history. Call it
 *   before render_shutdown().
 */

#include <assert.h>
#include <stdlib.h>

#include "font/font.h"
#include "render/render.h"
#include "render/term_frame.h"
#include <wsap_vtcore/wsap_vtcore.h>

static struct font_instance *test_font_override;

static inline struct font_instance *test_font(void)
{
    return test_font_override ? test_font_override : font_instance_default();
}

static struct renderer *test_oracle_renderer;

/* The oracle renderer: no cell cache and no workers. */
static inline struct renderer *test_oracle(void)
{
    if (!test_oracle_renderer)
        test_oracle_renderer = renderer_create(&(struct render_options){
            .threads = RENDER_NONE,
            .cache_bytes = RENDER_NONE,
        });
    assert(test_oracle_renderer);
    return test_oracle_renderer;
}

static inline struct wsap_vtcore_snapshot *test_snapshot_of(struct wsap_vtcore *term)
{
    if (!term)
        return NULL;
    struct wsap_vtcore_snapshot *snap = wsap_vtcore_snapshot_new();
    assert(snap);
    bool ok = wsap_vtcore_snapshot_update(snap, term);
    assert(ok);
    (void)ok;
    return snap;
}

/* Fills `frame` from a new snapshot. It stays valid until the next call. */
static inline void test_frame_init(struct terminal_frame *frame, struct wsap_vtcore *term,
                                   bool cursor_visible)
{
    static struct wsap_vtcore_snapshot *snap;
    wsap_vtcore_snapshot_free(snap);
    snap = test_snapshot_of(term);
    terminal_frame_init(frame, snap, cursor_visible);
}

/* A frame whose grid covers the whole canvas, in the test font. */
static inline struct render_frame test_grid_frame(const struct terminal_frame *grid,
                                                  int width, int height)
{
    return (struct render_frame){
        .grid = {
            .frame = grid,
            .font = test_font(),
            .rect = { .size = { .x = width, .y = height } },
        },
        .ui_font = test_font(),
    };
}

/* Paints `grid` with no history: every pixel, and no state kept. */
static inline struct render_result test_draw_once(struct renderer *r, uint32_t *px,
                                                  int width, int height,
                                                  const struct terminal_frame *grid)
{
    struct framebuffer target = { .pixels = px, .size = { .x = width, .y = height } };
    struct render_frame frame = test_grid_frame(grid, width, height);
    return render_draw(r, &target, &frame);
}

static inline void test_render_paint_full(uint32_t *px, int width, int height,
                                          bool cursor_visible, struct wsap_vtcore *term)
{
    struct wsap_vtcore_snapshot *snap = test_snapshot_of(term);
    struct terminal_frame frame;
    terminal_frame_init(&frame, snap, cursor_visible);
    (void)test_draw_once(test_oracle(), px, width, height, &frame);
    wsap_vtcore_snapshot_free(snap);
}

static struct framebuffer_history test_frame_history;
static const struct renderer *test_frame_history_owner;
static const uint32_t *test_frame_history_pixels;
static struct vector2i test_frame_history_size;

static inline struct render_result test_renderer_paint(struct renderer *r, uint32_t *px,
                                                       int width, int height,
                                                       struct wsap_vtcore *term,
                                                       bool cursor_visible)
{
    struct vector2i size = { .x = width, .y = height };
    if (test_frame_history_owner != r) {
        framebuffer_history_release(&test_frame_history);
        test_frame_history_owner = r;
        test_frame_history_pixels = NULL;
    }
    bool fresh = test_frame_history_pixels != px || !vector2i_eq(test_frame_history_size, size);
    test_frame_history_pixels = px;
    test_frame_history_size = size;
    struct wsap_vtcore_snapshot *snap = test_snapshot_of(term);
    struct terminal_frame grid;
    terminal_frame_init(&grid, snap, cursor_visible);
    struct framebuffer target = {
        .pixels = px,
        .size = size,
        .fresh = fresh,
        .history = &test_frame_history,
    };
    struct render_frame frame = test_grid_frame(&grid, width, height);
    struct render_result result = render_draw(r, &target, &frame);
    wsap_vtcore_snapshot_free(snap);
    return result;
}

/* Destroys `r`. It also drops the canvas history of test_renderer_paint(). */
static inline void test_renderer_destroy(struct renderer *r)
{
    if (test_frame_history_owner == r) {
        framebuffer_history_release(&test_frame_history);
        test_frame_history_owner = NULL;
        test_frame_history_pixels = NULL;
    }
    renderer_destroy(r);
}

static inline void test_frame_helpers_fini(void)
{
    framebuffer_history_release(&test_frame_history);
    test_frame_history_owner = NULL;
    test_frame_history_pixels = NULL;
    renderer_destroy(test_oracle_renderer);
    test_oracle_renderer = NULL;
    test_frame_init(&(struct terminal_frame){0}, NULL, false);
}

#endif
