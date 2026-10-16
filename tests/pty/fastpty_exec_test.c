#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#include <spawn.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

extern char **environ;

static const char *output_type(int fd)
{
    struct stat st;
    assert(fstat(fd, &st) == 0);
    return S_ISCHR(st.st_mode) ? "tty" : S_ISFIFO(st.st_mode) ? "pipe" : "file";
}

static const char *probe_for(const char *mode)
{
    if (strstr(mode, "-stderr"))
        return "probe-isatty-stderr";
    if (strstr(mode, "-isatty"))
        return "probe-isatty";
    if (strstr(mode, "-env"))
        return "probe-env";
    return "probe";
}

int main(int argc, char **argv)
{
    assert(argc >= 2);
    if (!strcmp(argv[1], "probe")) {
        dprintf(1, "OUT:%s\n", output_type(1));
        dprintf(2, "ERR:%s\n", output_type(2));
        return 0;
    }
    if (!strcmp(argv[1], "probe-isatty-stderr")) {
        dprintf(2, "ISATTY:%d\n", !!isatty(2));
        return 0;
    }
    if (!strcmp(argv[1], "probe-isatty")) {
        dprintf(1, "ISATTY:%d\n", !!isatty(1));
        return 0;
    }
    if (!strcmp(argv[1], "probe-env")) {
        const char *preload = getenv("LD_PRELOAD");
        dprintf(1, "PRELOAD:%s\n", preload ? preload : "");
        return 0;
    }
    if (!strcmp(argv[1], "probe-termios")) {
        struct termios tio;
        assert(tcgetattr(0, &tio) == 0);
        unsigned long first = tio.c_oflag;
        assert(tcsetattr(0, TCSANOW, &tio) == 0);
        tio.c_oflag &= ~OPOST;
        unsigned long second = tio.c_oflag;
        assert(tcsetattr(1, TCSANOW, &tio) == 0);
        tio.c_oflag |= OPOST;
        unsigned long third = tio.c_oflag;
        assert(ioctl(1, TCSETS, &tio) == 0);
        assert(tcgetattr(1, &tio) == 0);
        assert((unsigned long)tio.c_oflag == third);
        dprintf(1, "OFLAG:%lu:%lu:%lu\n", first, second, third);
        return 0;
    }
    if (!strcmp(argv[1], "probe-other-termios")) {
        int master, slave;
        assert(openpty(&master, &slave, NULL, NULL, NULL) == 0);
        struct termios tio;
        assert(tcgetattr(slave, &tio) == 0);
        tio.c_oflag = 0;
        assert(tcsetattr(slave, TCSANOW, &tio) == 0);
        assert(ioctl(slave, TCSETS, &tio) == 0);
        close(slave);
        close(master);
        dprintf(1, "OTHER:ok\n");
        return 0;
    }
    if (!strcmp(argv[1], "probe-dup")) {
        int fd = dup(1);
        assert(fd >= 0);
        assert(isatty(fd));
        struct termios tio;
        assert(tcgetattr(fd, &tio) == 0);
        assert(ttyname(fd));
        assert(write(fd, "DUP:ok\n", 7) == 7);
        close(fd);
        return 0;
    }
    if (!strcmp(argv[1], "probe-route")) {
        /* A burst in cooked mode, then a marker in raw mode. The burst
         * keeps the pipe backlogged at each switch, so without a
         * barrier the raw marker would overtake it. */
        assert(argc == 3);
        int rounds = atoi(argv[2]);
        struct termios cooked, raw;
        assert(tcgetattr(0, &cooked) == 0);
        raw = cooked;
        cfmakeraw(&raw);
        static char burst[32768];
        memset(burst, 'a', sizeof(burst));
        for (int i = 0; i < rounds; i++) {
            assert(write(1, burst, sizeof(burst)) == (ssize_t)sizeof(burst));
            dprintf(1, "A%d.", i);
            assert(tcsetattr(0, TCSANOW, &raw) == 0);
            dprintf(1, "B%d.", i);
            assert(tcsetattr(0, TCSANOW, &cooked) == 0);
        }
        dprintf(1, "END");
        return 0;
    }
    if (!strcmp(argv[1], "probe-reuse")) {
        /* The shim keeps a terminal fd of its own. An app that closes
         * that number and reuses it must not have its new file closed
         * by the shim's next lookup. */
        struct termios tio;
        assert(tcgetattr(1, &tio) == 0);
        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        assert(null_fd >= 0);
        int reused[64], nreused = 0;
        for (int fd = 3; fd < 256 && nreused < 64; fd++) {
            struct stat st;
            if (fd != null_fd && fstat(fd, &st) == 0 && S_ISCHR(st.st_mode) &&
                isatty(fd)) {
                assert(dup2(null_fd, fd) == fd);
                reused[nreused++] = fd;
            }
        }
        assert(tcgetattr(1, &tio) == 0);
        for (int i = 0; i < nreused; i++)
            assert(fcntl(reused[i], F_GETFD) >= 0);
        dprintf(1, "REUSE:ok\n");
        return 0;
    }
    if (!strcmp(argv[1], "probe-ttyname")) {
        const char *name = ttyname(1);
        assert(name && !strncmp(name, "/dev/", 5));
        dprintf(1, "TTYNAME:%s\n", name);
        return 0;
    }
    if (!strcmp(argv[1], "probe-tc")) {
        assert(setsid() >= 0);
        assert(ioctl(0, TIOCSCTTY, 0) == 0);
        assert(tcsetpgrp(1, getpgrp()) == 0);
        assert(tcgetpgrp(1) == getpgrp());
        dprintf(1, "PGRP:ok\n");
        dprintf(1, "DRAIN:%d\n", tcdrain(1));
        dprintf(1, "FLOW:%d\n", tcflow(1, TCOON));
        dprintf(1, "FLUSH:%d\n", tcflush(1, TCIOFLUSH));
        return 0;
    }
    assert(argc == 3);
    if (!strstr(argv[1], "-env"))
        unsetenv("LD_PRELOAD");
    const char *mode = argv[1];
    const char *probe = probe_for(mode);
    char *args[] = {argv[2], (char *)probe, NULL};
    if (!strncmp(mode, "spawnp", 6)) {
        pid_t pid;
        assert(posix_spawnp(&pid, argv[2], NULL, NULL, args, environ) == 0);
        assert(waitpid(pid, NULL, 0) == pid);
        return 0;
    }
    if (!strncmp(mode, "spawn", 5)) {
        pid_t pid;
        assert(posix_spawn(&pid, argv[2], NULL, NULL, args, environ) == 0);
        assert(waitpid(pid, NULL, 0) == pid);
        return 0;
    }
    if (!strncmp(mode, "vp", 2)) {
        execvp(argv[2], args);
        perror("execvp");
        return 1;
    }
    if (!strncmp(mode, "fexec", 5)) {
        int fd = open(argv[2], O_RDONLY);
        assert(fd >= 0);
        fexecve(fd, args, environ);
        perror("fexecve");
        return 1;
    }
    if (!strncmp(mode, "le", 2)) {
        execle(argv[2], argv[2], probe, (char *)NULL, environ);
        perror("execle");
        return 1;
    }
    if (!strncmp(mode, "lp", 2)) {
        execlp(argv[2], argv[2], probe, (char *)NULL);
        perror("execlp");
        return 1;
    }
    if (!strncmp(mode, "l", 1)) {
        execl(argv[2], argv[2], probe, (char *)NULL);
        perror("execl");
        return 1;
    }
    if (!strncmp(mode, "v", 1)) {
        execv(argv[2], args);
        perror("execv");
        return 1;
    }
    execve(argv[2], args, environ);
    assert(!strcmp(argv[1], "fail") && errno == ENOENT);
    dprintf(1, "OUT:restored\n");
    dprintf(2, "ERR:restored\n");
    return 0;
}
