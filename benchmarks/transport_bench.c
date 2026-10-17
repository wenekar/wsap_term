#define _GNU_SOURCE
#include "bench.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define BUFFER_SIZE (1u << 20)
static unsigned char pattern[BUFFER_SIZE], read_buffer[BUFFER_SIZE];

static void die(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}

static void make_pattern(bool random)
{
    if (!random) {
        memset(pattern, 'x', sizeof(pattern));
        for (size_t i = 79; i < sizeof(pattern); i += 80)
            pattern[i] = '\n';
        return;
    }
    uint64_t state = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < sizeof(pattern); i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        pattern[i] = (unsigned char)(state >> 24);
    }
}

static void write_payload(int fd, uint64_t bytes)
{
    size_t pos = 0;
    while (bytes) {
        size_t chunk = sizeof(pattern) - pos;
        if (chunk > bytes) chunk = (size_t)bytes;
        ssize_t n = write(fd, pattern + pos, chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) _exit(1);
        pos = (pos + (size_t)n) % sizeof(pattern);
        bytes -= (uint64_t)n;
    }
    _exit(0);
}

static bool drain(int fd, uint64_t expected)
{
    uint64_t total = 0;
    while (total < expected) {
        size_t chunk = expected - total < sizeof(read_buffer)
            ? (size_t)(expected - total) : sizeof(read_buffer);
        ssize_t n = read(fd, read_buffer, chunk);
        if (n > 0) {
            total += (uint64_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            int ready;
            do { ready = poll(&pfd, 1, 30000); } while (ready < 0 && errno == EINTR);
            if (ready > 0) continue;
        }
        return false;
    }
    return true;
}

static const struct scenario {
    const char *name;
    bool pipe, raw, random;
} scenarios[] = {
    { .name = "pty/cooked" },
    { .name = "pipe/text", .pipe = true },
    { .name = "pipe/random", .pipe = true, .random = true },
    { .name = "pty/raw", .raw = true },
    { .name = "pty/raw-random", .raw = true, .random = true },
};

static bool run(const struct scenario *s, uint64_t bytes)
{
    make_pattern(s->random);
    int reader, writer;
    uint64_t expected = bytes;
    if (s->pipe) {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) die("pipe2");
        reader = fds[0];
        writer = fds[1];
        if (fcntl(writer, F_SETPIPE_SZ, BUFFER_SIZE) < 0)
            perror("F_SETPIPE_SZ");
    } else {
        if (openpty(&reader, &writer, NULL, NULL, NULL) < 0) die("openpty");
        struct termios tio;
        if (tcgetattr(writer, &tio) < 0) die("tcgetattr");
        tio.c_oflag = s->raw ? 0 : OPOST | ONLCR;
        if (tcsetattr(writer, TCSANOW, &tio) < 0) die("tcsetattr");
        if (!s->raw)
            expected += bytes / BUFFER_SIZE * (BUFFER_SIZE / 80) + bytes % BUFFER_SIZE / 80;
    }
    if (fcntl(reader, F_SETFL, O_NONBLOCK) < 0) die("O_NONBLOCK");
    pid_t pid = fork();
    if (pid < 0) die("fork");
    if (!pid) {
        close(reader);
        write_payload(writer, bytes);
    }
    /* Keep the writer open until all bytes arrive. PTY close can discard a tail. */
    uint64_t start = bench_now_ns();
    bool ok = drain(reader, expected);
    double seconds = (double)(bench_now_ns() - start) * 1e-9;
    if (!ok) kill(pid, SIGKILL);
    close(writer);
    close(reader);
    int status;
    pid_t reaped;
    do { reaped = waitpid(pid, &status, 0); } while (reaped < 0 && errno == EINTR);
    ok = ok && reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (ok)
        printf("%-18s %9.1f MiB/s (%5.2f s, %llu bytes read)\n", s->name,
               (double)expected / BUFFER_SIZE / seconds, seconds,
               (unsigned long long)expected);
    else
        fprintf(stderr, "%s: incomplete transfer\n", s->name);
    return ok;
}

int main(int argc, char **argv)
{
    long mib = argc > 1 ? atol(argv[1]) : 256;
    if (argc > 2 || mib < 1 || mib > 16384) {
        fprintf(stderr, "usage: %s [MiB:1..16384]\n", argv[0]);
        return 1;
    }
    bool ok = true;
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++)
        ok &= run(&scenarios[i], (uint64_t)mib * BUFFER_SIZE);
    return ok ? 0 : 1;
}
