#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static ssize_t (*real_read)(int, void *, size_t);

__attribute__((constructor)) static void init(void)
{
    real_read = dlsym(RTLD_NEXT, "read");
}

ssize_t read(int fd, void *buf, size_t len)
{
    ssize_t n = real_read(fd, buf, len);
    int error = errno;
    unsigned int number;
    if (n > 0 && syscall(SYS_gettid) != getpid() &&
        ioctl(fd, TIOCGPTN, &number) == 0) {
        struct timespec delay = {.tv_nsec = 2000000};
        nanosleep(&delay, NULL);
    }
    errno = error;
    return n;
}
