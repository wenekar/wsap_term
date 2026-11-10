#ifndef YASL_COCOA_SOFTWARE_STAGE_H
#define YASL_COCOA_SOFTWARE_STAGE_H

#include <stdbool.h>
#include <stddef.h>

/*
 * The staging buffers of the CPU pixels of one surface.
 *
 * A blit reads its buffer after the call that commits it returns, so the next
 * frame must not write that buffer until the GPU is done with it. The ring owns
 * three buffers and hands out only a buffer that no started command reads. A
 * buffer comes back when its command completed. Then the GPU completion
 * precedes the CPU reuse, and no frame waits for the GPU.
 *
 * The ring outlives the surface that owns it. A command that completes after
 * the surface is destroyed still returns its buffer to the ring, and the ring
 * frees the buffers when the last command is done.
 *
 * Threads: acquire and cancel on the thread that draws. The completion runs on
 * a thread of the GPU driver.
 */

struct software_stages;
struct software_stage;

struct software_stages *software_stages_new(void *device);
/* Drops the reference of the surface. The buffers live on while a command
 * still reads one of them. */
void software_stages_free(struct software_stages *stages);

/* A buffer of at least `length` bytes that no started command reads, or NULL
 * when every buffer is still in use. */
struct software_stage *software_stage_acquire(struct software_stages *stages, size_t length);
void *software_stage_buffer(const struct software_stage *stage);
/* The frame that took `stage` did not start a command. */
void software_stage_cancel(struct software_stages *stages, struct software_stage *stage);
/* The caller commits `command`, which reads the buffer of `stage`. The buffer
 * comes back when that command completed. */
void software_stage_finish(struct software_stages *stages, struct software_stage *stage,
                           void *command);

#endif
