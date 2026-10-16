#ifndef PTY_PRELOAD_SHIM_H
#define PTY_PRELOAD_SHIM_H

#include <stddef.h>
#include <sys/types.h>
#include <termios.h>

#ifdef __APPLE__
int fp_isatty(int fd);
int fp_tcgetattr(int fd, struct termios *t);
int fp_tcsetattr(int fd, int actions, const struct termios *t);
int fp_tcdrain(int fd);
int fp_tcflush(int fd, int queue);
int fp_tcflow(int fd, int action);
int fp_tcsendbreak(int fd, int duration);
pid_t fp_tcgetpgrp(int fd);
int fp_tcsetpgrp(int fd, pid_t pgrp);
char *fp_ttyname(int fd);
int fp_ttyname_r(int fd, char *buf, size_t n);
int fp_ioctl(int fd, unsigned long request, ...);
#endif

#ifdef __APPLE__
#define FP_SHIM(name) fp_##name
#else
#define FP_SHIM(name) name
#endif

int fp_is_fast_pipe(int fd);
int fp_tty_fd(void);

int fp_stdio_to_tty(int saved[2], int sync);
void fp_stdio_restore(int saved[2]);

void fp_termios_changed(int fd, const struct termios *t);

#ifndef __APPLE__
extern char fp_preload[4096];

char **fp_env_with_preload(char *const envp[], char **entry_out);
char **fp_env_without_fast_preload(char *const envp[], char **entry_out);
int fp_env_has_preload(char *const envp[]);
int fp_env_disables_preload(char *const envp[]);
#endif

#endif
