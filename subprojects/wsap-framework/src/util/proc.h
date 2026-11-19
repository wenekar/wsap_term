#ifndef UTIL_PROC_H
#define UTIL_PROC_H

#include <stdbool.h>
#include <sys/types.h>

void proc_child_unblock_signals(void);

void proc_spawn_detached(char *const argv[]);

int proc_nonblocking_pipe(int fds[2]);

void proc_eventfd_close(int rd_fd, int wr_fd);

const char *proc_home_dir(void);

/*
 * A child process with one pipe for each standard stream.
 * A zero value means that no child belongs to the record.
 * The descriptors are valid while `pid` is above zero.
 * The parent writes to `in_fd` (the stdin of the child) and reads from
 * `out_fd` and `err_fd`. The pipes are nonblocking and close-on-exec.
 */
struct proc_child {
    pid_t pid;
    int in_fd;
    int out_fd;
    int err_fd;
};

/*
 * Starts argv with a pipe for stdin, stdout, and stderr.
 * The child gets the descriptors of the parent and no other pipe end.
 * Returns 0, or -1 with errno. A failure leaves `child` zero.
 * The caller must reap the child, also when it exits early.
 * A write to a closed pipe raises SIGPIPE, as for any pipe. Ignore the
 * signal in the caller when it can happen.
 */
int proc_spawn(char *const argv[], struct proc_child *child);

/*
 * Waits until the child exits and stores the wait status in `*status`.
 * The status is the waitpid(2) value; NULL is allowed.
 * Returns 0, or -1 with errno. ECHILD means that no child belongs to `child`.
 * It does not close the pipes, so the caller can read the last output first.
 */
int proc_wait(struct proc_child *child, int *status);

/*
 * Reaps the child without a block and stores the wait status in `*status`.
 * Returns 1 when it reaped the child, 0 while the child runs, or -1 with
 * errno. It does not close the pipes.
 */
int proc_reap(struct proc_child *child, int *status);

/*
 * Closes the pipes of the child. The child still runs and still needs a wait.
 * A second call, and a call on a zero record, do nothing.
 */
void proc_child_close(struct proc_child *child);

#endif
