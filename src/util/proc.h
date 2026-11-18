#ifndef UTIL_PROC_H
#define UTIL_PROC_H

void proc_child_unblock_signals(void);

void proc_spawn_detached(char *const argv[]);

int proc_nonblocking_pipe(int fds[2]);

void proc_eventfd_close(int rd_fd, int wr_fd);

const char *proc_home_dir(void);

#endif
