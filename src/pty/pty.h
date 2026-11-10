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

/* The largest time that one pass may spend on PTY output while the output keeps
 * streaming. A pass that the base budget stopped with data still to read was
 * limited by the budget, so the next pass doubles it up to this value
 * (pty_drain_budget_next). A flood then gives the loop back less often, and the
 * drain keeps the kernel PTY buffer empty. Input waits up to this time between
 * two passes of a flood, and a stream that runs dry returns to the base
 * budget. */
#define PTY_DRAIN_BUDGET_MAX_NS 10000000ull

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

/* The budget of the next pass, after the last one ended with `more` output to
 * read. A pass that the budget stopped doubles the budget, so a stream at speed
 * gives the loop back less often. A pass that ran dry returns to the base
 * budget, so input and a frame get the loop back in the base time. */
static inline uint64_t pty_drain_budget_next(uint64_t budget_ns, bool more)
{
    if (!more || !budget_ns)
        return PTY_DRAIN_BUDGET_NS;
    return budget_ns < PTY_DRAIN_BUDGET_MAX_NS / 2 ? budget_ns * 2
                                                   : PTY_DRAIN_BUDGET_MAX_NS;
}

/* The size of the next chunk, after the parse of the last one took `took_ns` of
 * the `budget_ns` of the pass. A chunk that took longer than half the budget
 * halves, so a slow stream reads less and its overrun stays short. A chunk that
 * took less than a quarter of the budget doubles, so a fast stream reads the
 * maximum. A chunk between those times keeps its size, so a stream at speed
 * that read the maximum keeps it, and a short delay does not shrink it. */
static inline size_t pty_chunk_next(size_t chunk, uint64_t took_ns,
                                    uint64_t budget_ns)
{
    if (!chunk)
        return PTY_CHUNK_START;
    if (took_ns > budget_ns / 2)
        chunk = chunk > PTY_CHUNK_MIN ? chunk / 2 : PTY_CHUNK_MIN;
    else if (took_ns < budget_ns / 4)
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
