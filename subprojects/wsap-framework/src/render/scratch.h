#ifndef RENDER_SCRATCH_H
#define RENDER_SCRATCH_H

/*
 * The word scratch of a thread. The row walk uses it for the segment code
 * points. Each thread owns its own scratch, so a walk on one thread never
 * sees the walk of another thread.
 *
 * The renderer gives every pool thread room before a parallel phase. A pool
 * thread releases its own scratch before it exits. render_shutdown()
 * releases the scratch of the main thread.
 */

#include <stdbool.h>
#include <stdint.h>

/*
 * Returns a thread-local array of at least `words` words. It keeps the room
 * of an earlier call. Returns NULL when the memory is missing.
 */
uint32_t *render_thread_scratch(int words);
/* Makes room for `words` words. True if the room exists. */
bool render_thread_scratch_reserve(int words);
/* Frees the scratch of the calling thread. */
void render_thread_scratch_release(void);

#endif
