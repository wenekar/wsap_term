#ifndef PTY_SHELL_H
#define PTY_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* The shell that a new session runs when the spec names no command. */
const char *pty_default_shell(void);

/* Copies the working directory of `pid` into `out`. Returns false when the
 * directory is unavailable or does not fit. */
bool pty_read_pid_cwd_buf(pid_t pid, char *out, size_t sz);

#endif
