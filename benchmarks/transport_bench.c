#define _GNU_SOURCE

#include <sys/types.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define FAST_PIPE_SIZE (1u << 20)
#define READ_SIZE      (1u << 20)
#define PATTERN_SIZE   (1u << 20)
#define TOTAL_BYTES    (256ull << 20)
#define SENTINEL       "### clernen transport bench done ###"
#define SENT_LEN       (sizeof(SENTINEL) - 1)
#define POLL_TIMEOUT_MS 30000

static unsigned char pattern[PATTERN_SIZE];
static size_t pattern_len;

static void build_pattern(void)
{
    size_t n = 0;
    for (unsigned line = 0;; line++) {
        size_t len = (line % 16 == 15) ? 4096 : 79;
        if (n + len + 1 > sizeof(pattern))
            break;
        for (size_t i = 0; i < len; i++)
            pattern[n + i] =
                (unsigned char)(' ' + (line * 31 + i * 7) % 95);
        pattern[n + len] = '\n';
        n += len + 1;
    }
    pattern_len = n;
}

/* High-entropy stand-in for `cat /dev/random`, but seeded so two runs of the
 * benchmark see identical bytes. Every byte value occurs, so a one-byte magic
 * prefilter matches roughly every 256 bytes. */
static void build_pattern_random(void)
{
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (size_t i = 0; i < sizeof(pattern); i++) {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        pattern[i] = (unsigned char)(s >> 24);
    }
    pattern_len = sizeof(pattern);
}

static void die(const char *msg)
{
    perror(msg);
    exit(1);
}

static void write_all(int fd, const unsigned char *p, size_t len)
{
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            _exit(1);
        }
        p += n;
        len -= (size_t)n;
    }
}

static void child_writer(int fd)
{
    for (uint64_t done = 0; done < TOTAL_BYTES;) {
        size_t chunk = pattern_len;
        if (chunk > TOTAL_BYTES - done)
            chunk = (size_t)(TOTAL_BYTES - done);
        write_all(fd, pattern, chunk);
        done += chunk;
    }
    write_all(fd, (const unsigned char *)SENTINEL "\n",
            sizeof(SENTINEL "\n") - 1);
    _exit(0);
}

struct reader {
    int poll_fd;
};

static ssize_t reader_read(struct reader *r, void *buf, size_t len)
{
    return read(r->poll_fd, buf, len);
}

static int contains_sentinel(const unsigned char *tail, size_t tlen,
        const unsigned char *buf, size_t n)
{
    if (tlen) {
        unsigned char edge[2 * (SENT_LEN - 1)];
        size_t head = n < SENT_LEN - 1 ? n : SENT_LEN - 1;
        memcpy(edge, tail, tlen);
        memcpy(edge + tlen, buf, head);
        if (memmem(edge, tlen + head, SENTINEL, SENT_LEN))
            return 1;
    }
    return memmem(buf, n, SENTINEL, SENT_LEN) != NULL;
}

static void push_tail(unsigned char *tail, size_t *tlen,
        const unsigned char *buf, size_t n)
{
    if (n >= SENT_LEN - 1) {
        memcpy(tail, buf + n - (SENT_LEN - 1), SENT_LEN - 1);
        *tlen = SENT_LEN - 1;
        return;
    }
    if (*tlen + n > SENT_LEN - 1) {
        size_t keep = SENT_LEN - 1 - n;
        memmove(tail, tail + *tlen - keep, keep);
        *tlen = keep;
    }
    memcpy(tail + *tlen, buf, n);
    *tlen += n;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double drain(struct reader *r, uint64_t *bytes_out)
{
    static unsigned char buf[READ_SIZE];
    unsigned char tail[SENT_LEN - 1] = {0};
    size_t tlen = 0;
    uint64_t total = 0;
    double t0 = -1.0;

    for (;;) {
        ssize_t n = reader_read(r, buf, sizeof(buf));
        if (n > 0) {
            double now = now_s();
            if (t0 < 0)
                t0 = now;
            total += (uint64_t)n;
            if (contains_sentinel(tail, tlen, buf, (size_t)n)) {
                *bytes_out = total;
                return now - t0;
            }
            push_tail(tail, &tlen, buf, (size_t)n);
            continue;
        }
        if (n == 0 || errno == EIO)
            return -1.0;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd = { .fd = r->poll_fd, .events = POLLIN };
            int pr;
            do {
                pr = poll(&pfd, 1, POLL_TIMEOUT_MS);
            } while (pr < 0 && errno == EINTR);
            if (pr < 0)
                die("poll");
            if (pr == 0) {
                fprintf(stderr, "drain: poll timed out\n");
                return -1.0;
            }
            continue;
        }
        die("read");
    }
}

struct result {
    double secs;
    uint64_t bytes;
    int pipe_sz;
};

static void reap(pid_t pid)
{
    int status;
    for (int i = 0; i < 2000; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid)
            return;
        usleep(1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
}

struct bench_cfg {
    const char *name;
    int use_fast;
    int raw_oflag;
    int random_data;
};

static struct result run_one(const struct bench_cfg *cfg)
{
    struct result res = { .secs = -1.0, .bytes = 0, .pipe_sz = 0 };
    int master = -1, slave = -1, pfds[2] = { -1, -1 };
    int use_fast = cfg->use_fast;

    if (cfg->random_data)
        build_pattern_random();
    else
        build_pattern();

    if (openpty(&master, &slave, NULL, NULL, NULL) < 0)
        die("openpty");

    if (cfg->raw_oflag) {
        struct termios tio;
        if (tcgetattr(slave, &tio) < 0)
            die("tcgetattr");
        tio.c_oflag &= ~OPOST;
        if (tcsetattr(slave, TCSANOW, &tio) < 0)
            die("tcsetattr");
    }

    if (use_fast) {
        if (pipe2(pfds, O_CLOEXEC) < 0)
            die("pipe2");
        if (fcntl(pfds[1], F_SETPIPE_SZ, FAST_PIPE_SIZE) < 0)
            fprintf(stderr, "warning: F_SETPIPE_SZ: %s\n",
                    strerror(errno));
        res.pipe_sz = fcntl(pfds[0], F_GETPIPE_SZ);
    }

    int wr_fd = use_fast ? pfds[1] : slave;
    int rd_fd = use_fast ? pfds[0] : master;
    int fl = fcntl(rd_fd, F_GETFL, 0);
    if (fl < 0 || fcntl(rd_fd, F_SETFL, fl | O_NONBLOCK) < 0)
        die("fcntl O_NONBLOCK");

    pid_t pid = fork();
    if (pid < 0)
        die("fork");
    if (pid == 0) {
        close(master);
        if (use_fast) {
            close(slave);
            close(pfds[0]);
        }
        child_writer(wr_fd);
    }

    close(wr_fd);

    struct reader r = { .poll_fd = rd_fd };
    res.secs = drain(&r, &res.bytes);
    reap(pid);

    close(master);
    if (use_fast) {
        close(slave);
        close(pfds[0]);
    }
    return res;
}

int main(void)
{
    printf("payload %llu MiB + sentinel\n",
            (unsigned long long)(TOTAL_BYTES >> 20));

    static const struct bench_cfg cfg[] = {
        { "A  real pty, cooked (kernel OPOST|ONLCR)", .use_fast = 0 },
        { "B  fast pipe, text",          .use_fast = 1 },
        { "C  fast pipe, high-entropy",  .use_fast = 1, .random_data = 1 },
        { "D  real pty, raw (OPOST off)", .raw_oflag = 1 },
        { "E  real pty, raw, high-entropy", .raw_oflag = 1, .random_data = 1 },
    };
    enum { NCFG = sizeof(cfg) / sizeof(cfg[0]) };
    double mib_s[NCFG];

    for (unsigned i = 0; i < NCFG; i++) {
        struct result r = run_one(&cfg[i]);
        if (r.secs < 0) {
            fprintf(stderr, "FAIL: %-44s (stream ended early)\n", cfg[i].name);
            mib_s[i] = 0;
            continue;
        }
        mib_s[i] = (double)r.bytes / (double)(1 << 20) / r.secs;
        printf("%-46s %8.1f MiB/s  (%5.2f s, %llu MiB read",
                cfg[i].name, mib_s[i], r.secs,
                (unsigned long long)(r.bytes >> 20));
        if (r.pipe_sz)
            printf(", pipe %d bytes", r.pipe_sz);
        printf(")\n");
        fflush(stdout);
    }

    if (mib_s[0] > 0 && mib_s[1] > 0)
        printf("\nspeedup over cooked pty   B/A = %.2fx", mib_s[1] / mib_s[0]);
    if (mib_s[0] > 0 && mib_s[2] > 0)
        printf("   C/A = %.2fx", mib_s[2] / mib_s[0]);
    printf("\n");
    return 0;
}
