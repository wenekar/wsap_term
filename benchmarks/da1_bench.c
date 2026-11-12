#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static volatile bool flood_running = true;
static int g_tty_out = -1;

static void *flood_thread_fn(void *arg)
{
    (void)arg;
    char buf[65536];
    memset(buf, 'x', sizeof(buf));
    for (size_t i = 79; i < sizeof(buf); i += 80)
        buf[i] = '\n';
    while (flood_running) {
        ssize_t n = write(g_tty_out, buf, sizeof(buf));
        if (n <= 0 && errno != EINTR && errno != EAGAIN)
            break;
    }
    return NULL;
}

static int cmp_double(const void *a, const void *b)
{
    double da = *(const double *)a;
    double db = *(const double *)b;
    return (da > db) - (da < db);
}

int main(int argc, char **argv)
{
    bool flood = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--flood") == 0)
            flood = true;
    }

    int tty_in = open("/dev/tty", O_RDWR);
    if (tty_in < 0) {
        perror("open /dev/tty");
        return 1;
    }
    g_tty_out = tty_in;

    int samples = 50;
    struct termios orig_tio, raw_tio;
    if (tcgetattr(tty_in, &orig_tio) < 0) {
        perror("tcgetattr");
        close(tty_in);
        return 1;
    }
    raw_tio = orig_tio;
    cfmakeraw(&raw_tio);
    raw_tio.c_cc[VMIN] = 1;
    raw_tio.c_cc[VTIME] = 0;
    if (tcsetattr(tty_in, TCSANOW, &raw_tio) < 0) {
        perror("tcsetattr");
        close(tty_in);
        return 1;
    }

    pthread_t flood_thread;
    if (flood) {
        pthread_create(&flood_thread, NULL, flood_thread_fn, NULL);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };
        nanosleep(&ts, NULL); /* 50 ms warmup */
    }

    double *lat_us = malloc(samples * sizeof(double));
    int valid = 0;

    for (int i = 0; i < samples; i++) {
        /* Discard any pending flooded input before query */
        struct pollfd pfd = { .fd = tty_in, .events = POLLIN };
        char drain[4096];
        while (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            if (read(tty_in, drain, sizeof(drain)) <= 0)
                break;
        }

        uint64_t t0 = now_ns();
        if (write(g_tty_out, "\033[c", 3) != 3)
            break;

        /* Wait for DA1 response: ESC [ ? ... c */
        bool got_esc = false;
        bool in_resp = false;
        bool done = false;
        while (!done) {
            char rbuf[256];
            ssize_t nr = read(tty_in, rbuf, sizeof(rbuf));
            if (nr <= 0) {
                if (nr < 0 && errno == EINTR)
                    continue;
                break;
            }
            for (ssize_t k = 0; k < nr; k++) {
                char ch = rbuf[k];
                if (ch == 0x1b) {
                    got_esc = true;
                    in_resp = false;
                } else if (got_esc && ch == '[') {
                    in_resp = true;
                    got_esc = false;
                } else if (in_resp) {
                    if (ch == 'c') {
                        done = true;
                        break;
                    }
                } else {
                    got_esc = false;
                }
            }
        }
        uint64_t dt = now_ns() - t0;
        if (done)
            lat_us[valid++] = (double)dt / 1000.0;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10000000 };
        nanosleep(&ts, NULL); /* 10 ms interval between queries */
    }

    if (flood) {
        flood_running = false;
        pthread_join(flood_thread, NULL);
    }
    tcsetattr(tty_in, TCSANOW, &orig_tio);
    close(tty_in);

    if (valid == 0) {
        fprintf(stderr, "error: no responses received\n");
        free(lat_us);
        return 1;
    }

    qsort(lat_us, valid, sizeof(double), cmp_double);
    double sum = 0, min = lat_us[0], max = lat_us[valid - 1];
    for (int i = 0; i < valid; i++)
        sum += lat_us[i];
    double avg = sum / valid;
    double p50 = lat_us[(int)(valid * 0.50)];
    double p95 = lat_us[(int)(valid * 0.95)];
    double p99 = lat_us[(int)(valid * 0.99)];

    const char *rep_path = getenv("DA1_REPORT");
    FILE *out = rep_path ? fopen(rep_path, "w") : stdout;
    if (!out) out = stdout;

    fprintf(out, "DA1 %s (%d samples): avg=%.1fus min=%.1fus p50=%.1fus p95=%.1fus p99=%.1fus max=%.1fus\n",
            flood ? "under-flood" : "idle", valid, avg, min, p50, p95, p99, max);

    if (out != stdout)
        fclose(out);

    free(lat_us);
    return 0;
}
