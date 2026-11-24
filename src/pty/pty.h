#ifndef PTY_PTY_H
#define PTY_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>

#include "util/geom.h"

#define PTY_PIPE_SIZE (1024 * 1024)

/* Maximum number of bytes per read from the pty or the fast pipe. */
#define PTY_CHUNK_MAX (256u * 1024u)

struct pty {
    unsigned master_handle;
    unsigned pipe_read_handle;
    unsigned sync_handle;
    pid_t pid;
};

struct pty_spec {
    struct vector2i grid, cell_pixels;
    const char *command;
    const char *cwd;
    const char *locale;
    const char *const *env;
};

/* Returns the file descriptor of the pseudo-terminal master, or -1 if closed. */
static inline int pty_master_fd(const struct pty *p)
{
    return p && p->master_handle ? (int)(p->master_handle - 1) : -1;
}

/* Returns the read file descriptor of the fast pipe, or -1 if unused. */
static inline int pty_pipe_fd(const struct pty *p)
{
    return p && p->pipe_read_handle ? (int)(p->pipe_read_handle - 1) : -1;
}

/* Returns the file descriptor of the synchronization socket, or -1 if unused. */
static inline int pty_sync_fd(const struct pty *p)
{
    return p && p->sync_handle ? (int)(p->sync_handle - 1) : -1;
}

/*
 * Spawns a child process in a new pseudo-terminal with specification `spec`.
 * Returns 0 on success, or -1 on failure. On failure, `p` holds no
 * descriptors.
 */
int pty_spawn(struct pty *p, const struct pty_spec *spec);

/*
 * Closes pseudo-terminal descriptors and terminates the child process.
 * If the child is still running, a detached thread reaps it and sends SIGKILL
 * after a grace period, so the call normally does not wait for the child to
 * exit.
 */
void pty_destroy(struct pty *p);

/* Sets the grid in columns and rows, and reports grid times cell_pixels as
 * the pixel dimensions. Zero cell size reports unknown dimensions. */
void pty_resize(struct pty *p, struct vector2i grid, struct vector2i cell_pixels);

#endif
