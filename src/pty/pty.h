#ifndef PTY_PTY_H
#define PTY_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#include "util/geom.h"

#define PTY_PIPE_SIZE (1024 * 1024)

/* The time that one pass may spend on PTY output before it gives the main
 * thread back to the loop. It is shorter than one frame of a 240 Hz display
 * (4.17 ms), so input and a frame share the frame period with the output. A
 * pass that still has data asks for the next one at once, so the budget does
 * not lower the throughput. The budget is not a hard bound on a pass: the
 * parse of one chunk can overrun it. */
#define PTY_DRAIN_BUDGET_NS 2000000ull

/* The bounds of one chunk of PTY output. A chunk ends the pass, so the parse
 * of one chunk of hard bytes, such as random data, is the overrun of the
 * budget. The chunk follows the measured parse time of the last one
 * (pty_chunk_next): a slow stream reads less, and a fast stream reads the
 * maximum. */
#define PTY_CHUNK_MAX (256u * 1024u)
#define PTY_CHUNK_MIN (8u * 1024u)
/* A stream starts small, so the first chunk of hard bytes cannot overrun the
 * budget by the parse of a whole chunk. A fast stream grows to the maximum in a
 * few reads. */
#define PTY_CHUNK_START (32u * 1024u)

/* The size of the next chunk, after the parse of the last one took `took_ns`.
 * A chunk that took longer than a quarter of the budget halves, and a chunk
 * that took less than an eighth doubles. A slow stream then reads less, so its
 * overrun stays short, and a fast stream reads the maximum, so the count of
 * reads stays low. */
static inline size_t pty_chunk_next(size_t chunk, uint64_t took_ns)
{
    if (!chunk)
        return PTY_CHUNK_START;
    if (took_ns > PTY_DRAIN_BUDGET_NS / 4)
        chunk = chunk > PTY_CHUNK_MIN ? chunk / 2 : PTY_CHUNK_MIN;
    else if (took_ns < PTY_DRAIN_BUDGET_NS / 8)
        chunk = chunk < PTY_CHUNK_MAX ? chunk * 2 : PTY_CHUNK_MAX;
    return chunk;
}

struct pty {
    unsigned master_handle;
    unsigned pipe_read_handle;
    unsigned sync_handle;
    pid_t pid;
};

struct pty_spec {
    struct vector2i grid;
    const char *command;
    const char *cwd;
    const char *const *env;
};

static inline int pty_master_fd(const struct pty *p)
{
    return p && p->master_handle ? (int)(p->master_handle - 1) : -1;
}

static inline int pty_pipe_fd(const struct pty *p)
{
    return p && p->pipe_read_handle ? (int)(p->pipe_read_handle - 1) : -1;
}

static inline int pty_sync_fd(const struct pty *p)
{
    return p && p->sync_handle ? (int)(p->sync_handle - 1) : -1;
}

int pty_spawn(struct pty *p, const struct pty_spec *spec);
void pty_destroy(struct pty *p);
void pty_resize(struct pty *p, struct vector2i grid);

#endif
