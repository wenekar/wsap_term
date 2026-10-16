/** @file CPU rendering with explicitly acquired frame buffers. */
#ifndef YASL_CPU_H
#define YASL_CPU_H
#include "yasl/yasl.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Borrowed premultiplied 0xAARRGGBB pixels; stride_bytes describes each row.
 * Only pixels may be edited. Preserve the remaining fields for present/discard.
 * The buffer expires on successful present/discard, the next wait, or destruction.
 * Do not retain or access pixels after expiry. */
struct yasl_cpu_buffer {
    uint32_t *pixels;
    int width, height;
    size_t stride_bytes;
    uint64_t token;
};
/** One outstanding acquisition per surface (-EBUSY otherwise). Starts with the
 * last submitted pixels; resize preserves their overlap and clears new pixels.
 * Clears out on failure. Acquisition does not change the visible frame. */
YASL_API int yasl_cpu_acquire(yasl_surface *surface, struct yasl_cpu_buffer *out);
/** Snapshot this buffer for presentation, then consume its token. Failed calls
 * leave a valid acquisition retryable. Stale/consumed tokens return -ESTALE.
 * Success means queued, not confirmed scanout. */
YASL_API int yasl_cpu_present(yasl_surface *surface, const struct yasl_cpu_buffer *buffer);
YASL_API int yasl_cpu_discard(yasl_surface *surface, const struct yasl_cpu_buffer *buffer);
#ifdef __cplusplus
}
#endif
#endif
