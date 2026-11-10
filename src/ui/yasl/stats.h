#ifndef FRAME_STATS_H
#define FRAME_STATS_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The frames of one window. Four counts stay apart, because they answer
 * different questions:
 *
 * - `frames_drawn` counts the draw passes. It is the rate of the loop.
 * - `frames_submitted` counts the passes that the GPU or the surface took.
 *   A submitted frame is not on screen yet.
 * - `frames_dropped` counts the passes that did not submit: a busy GPU, no
 *   free drawable, or a failed present. The window asks for the next frame.
 * - `frames_presented` counts the frames that a display reported as shown.
 *   Only a platform with presentation feedback reports it.
 */
struct frame_stats {
    uint64_t frames_drawn;
    uint64_t frames_submitted;
    uint64_t frames_dropped;
    uint64_t frames_presented;
    double frame_min_ms;
    double frame_max_ms;
    double frame_sum_ms;
    uint64_t frame_n;
    double render_sum_us;
    double render_max_us;
    uint64_t render_n;
    uint64_t started_ns;
    uint64_t render_started_ns;
    uint64_t last_frame_ns;
};

/*
 * The presentations of one surface, as the display reports them. The display
 * calls the presentation handler on its own thread, so the count is atomic and
 * this object is reference counted. The frame that waits for the report holds
 * one reference and the window holds another. A window that closes releases
 * its reference, and the last report then frees the object. The handler reads
 * no window, no surface, and no layer.
 */
struct present_watch;

struct present_watch *present_watch_new(void);
/* Adds a reference for a handler that runs later. */
struct present_watch *present_watch_ref(struct present_watch *w);
void present_watch_unref(struct present_watch *w);
/* One frame reached a display. It runs on the thread of the display. */
void present_watch_note_presented(struct present_watch *w);
/* The presentations since the last call. It runs on the window thread. */
uint64_t present_watch_take_presented(struct present_watch *w);

void frame_stats_print(struct frame_stats *st);
void frame_stats_tick(struct frame_stats *st);
void render_begin(struct frame_stats *st);
void render_end_and_count_frame(struct frame_stats *st);

#endif
