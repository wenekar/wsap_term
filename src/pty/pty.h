#ifndef PTY_PTY_H
#define PTY_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#include "util/geom.h"

#define PTY_PIPE_SIZE (1024 * 1024)

/* The largest read from the pty in one call. The worker thread parses each
 * read into the terminal at once, so this bounds the copy between the kernel
 * and the parser and nothing else. */
#define PTY_CHUNK_MAX (256u * 1024u)

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
    const char *locale;
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
