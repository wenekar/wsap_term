#ifndef WSAP_TEST_RENDER_FRAME_HELPERS_H
#define WSAP_TEST_RENDER_FRAME_HELPERS_H

#include <assert.h>
#include <stdlib.h>

#include "font/font.h"
#include "render/render.h"
#include "render/term_frame.h"
#include <wsap_vtcore/wsap_vtcore.h>

static struct framebuffer_history test_frame_history;
static struct renderer *test_frame_history_owner;
static bool test_frame_history_cleanup_registered;

static inline void test_frame_history_cleanup(void)
{
    framebuffer_history_release(&test_frame_history);
}

static struct font_instance *test_font_override;

static inline struct font_instance *test_font(void)
{
    return test_font_override ? test_font_override : font_instance_default();
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

static inline void test_frame_init(struct terminal_frame *frame, struct wsap_vtcore *term,
                                   bool cursor_visible)
{
    static struct wsap_vtcore_snapshot *snap;
    wsap_vtcore_snapshot_free(snap);
    snap = test_snapshot_of(term);
    terminal_frame_init(frame, snap, cursor_visible);
}

static inline void test_render_paint_full(uint32_t *px, int width, int height,
                                          bool cursor_visible,
                                          struct wsap_vtcore *term)
{
    struct wsap_vtcore_snapshot *snap = test_snapshot_of(term);
    struct terminal_frame frame;
    terminal_frame_init(&frame, snap, cursor_visible);
    render_paint_full_terminal_frame(test_font(), px, width, height, &frame);
    wsap_vtcore_snapshot_free(snap);
}

static inline void
test_renderer_paint(struct renderer *r, uint32_t *px, int width, int height,
                    int buf, struct wsap_vtcore *term, bool cursor_visible)
{
    if (!test_frame_history_cleanup_registered) {
        atexit(test_frame_history_cleanup);
        test_frame_history_cleanup_registered = true;
    }
    if (test_frame_history_owner != r) {
        framebuffer_history_release(&test_frame_history);
        test_frame_history_owner = r;
    }
    struct wsap_vtcore_snapshot *snap = test_snapshot_of(term);
    struct terminal_frame frame;
    terminal_frame_init(&frame, snap, cursor_visible);
    struct damage_list damage = {0};
    struct framebuffer target = {
        .pixels = px,
        .size = { .x = width, .y = height },
        .buffer = buf,
        .damage = &damage,
        .history = &test_frame_history,
    };
    struct render_frame input = {
        .font = test_font(),
        .has_grid = true,
        .grid = frame,
    };
    (void)render_draw(r, &target, &input);
    wsap_vtcore_snapshot_free(snap);
}

#endif
