#define _GNU_SOURCE
#include "bench.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

#include "platform/platform.h"
#include "session/session.h"
#include "yasl/yasl.h"

#define PAYLOAD_SIZE (64u * 1024)
#define TIMEOUT_NS (30ull * 1000000000)
#define DONE_TITLE "wsap-session-ingest-done"

static const char finish[] = "\030\033\\\033]0;" DONE_TITLE "\007";
static unsigned char payload[PAYLOAD_SIZE];
static unsigned char mixed_payload[PAYLOAD_SIZE];

enum workload { MANYLINE, LONGLINE, FGBG, RANDOM, MIXED };

static const struct scenario {
    const char *name;
    enum workload workload;
    bool pipe;
    bool raw;
} scenarios[] = {
    { .name = "pipe/manyline", .workload = MANYLINE, .pipe = true },
    { .name = "pipe/longline", .workload = LONGLINE, .pipe = true },
    { .name = "pipe/fgbg", .workload = FGBG, .pipe = true },
    { .name = "pipe/random", .workload = RANDOM, .pipe = true },
    { .name = "pty/cooked", .workload = MANYLINE },
    { .name = "pty/raw", .workload = MANYLINE, .raw = true },
    { .name = "pipe/mixed", .workload = MIXED, .pipe = true },
};

static void fail(const char *what)
{
    perror(what);
    exit(EXIT_FAILURE);
}

static bool write_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static bool read_byte(int fd, char want)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int ready;
    do { ready = poll(&pfd, 1, 30000); } while (ready < 0 && errno == EINTR);
    char c;
    return ready > 0 && read(fd, &c, 1) == 1 && c == want;
}

static void make_payload(enum workload workload, unsigned char *out)
{
    uint64_t state = 0x9e3779b97f4a7c15ull;
    for (size_t i = 0; i < sizeof(payload); i++) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = workload == RANDOM ? (unsigned char)(state >> 24)
                                       : (unsigned char)('a' + state % 26);
        if (workload == MANYLINE && i % 80 == 79)
            out[i] = '\n';
    }
    if (workload == FGBG) {
        static const char cell[] = "\033[38;2;12;34;56m\033[48;2;78;90;12mx";
        for (size_t i = 0; i + sizeof(cell) - 1 <= sizeof(payload);
             i += sizeof(cell) - 1)
            memcpy(out + i, cell, sizeof(cell) - 1);
    }
}

static void prepare_payload(enum workload workload)
{
    make_payload(workload, payload);
    if (workload == MIXED)
        make_payload(FGBG, mixed_payload);
}

static const unsigned char *payload_at(enum workload workload, uint64_t offset)
{
    return workload == MIXED && (offset / (1024 * 1024) & 1)
         ? mixed_payload : payload;
}

static int writer(const struct scenario *s, uint64_t bytes, const char *path)
{
    const char *cpu = getenv("WSAP_BENCH_WRITER_CPU");
    if (cpu) {
        char *end;
        long index = strtol(cpu, &end, 10);
        if (!*cpu || *end || index < 0 || index >= CPU_SETSIZE)
            return 1;
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(index, &set);
        if (sched_setaffinity(0, sizeof(set), &set) < 0)
            return 1;
    }
    struct termios tio;
    struct stat st;
    if (tcgetattr(STDIN_FILENO, &tio) < 0 || fstat(STDOUT_FILENO, &st) < 0)
        return 1;
    if (s->pipe ? !S_ISFIFO(st.st_mode) : !S_ISCHR(st.st_mode))
        return 1;
    tio.c_oflag = s->raw ? 0 : OPOST | ONLCR;
    tio.c_lflag &= ~ECHO;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) < 0)
        return 1;
    prepare_payload(s->workload);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    if (strlen(path) >= sizeof(addr.sun_path))
        return 1;
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return 1;
    if (!write_all(fd, "R", 1) || !read_byte(fd, 'G'))
        return 1;
    for (uint64_t left = bytes; left;) {
        size_t n = left < sizeof(payload) ? (size_t)left : sizeof(payload);
        if (!write_all(STDOUT_FILENO, payload_at(s->workload, bytes - left), n))
            return 1;
        left -= n;
    }
    /* Keep the writer open until the session parses the last marker. */
    bool ok = write_all(STDOUT_FILENO, finish, sizeof(finish) - 1) &&
              write_all(fd, "W", 1) && read_byte(fd, 'A');
    close(fd);
    return ok ? 0 : 1;
}

struct measurement {
    struct session *session;
    struct yasl_loop *loop;
    uint64_t end_ns;
    uint64_t cpu_ns;
    bool exited;
};

static uint64_t process_cpu_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) < 0)
        fail("clock_gettime");
    return (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
}

static void on_title(void *ud)
{
    struct measurement *m = ud;
    if (!strcmp(session_title(m->session), DONE_TITLE)) {
        m->end_ns = bench_now_ns();
        m->cpu_ns = process_cpu_ns();
        yasl_loop_quit(m->loop);
    }
}

static void on_exited(void *ud)
{
    struct measurement *m = ud;
    m->exited = true;
    yasl_loop_quit(m->loop);
}

static void on_signal(void *ud, int signo)
{
    (void)ud;
    if (signo == SIGCHLD)
        session_reap_exited();
}

static uint64_t on_timeout(void *ud, uint64_t id, uint64_t delay)
{
    (void)id;
    (void)delay;
    yasl_loop_quit(ud);
    return 0;
}

static struct wsap_vtcore *reference(const struct scenario *s, uint64_t bytes)
{
    struct wsap_vtcore *term = wsap_vtcore_new(&(struct wsap_vtcore_options){
        .size = { .x = 120, .y = 40 }, .scrollback = 1000,
        .cursor_steady = true,
    });
    if (!term)
        fail("wsap_vtcore_new");
    prepare_payload(s->workload);
    unsigned flags = s->raw ? 0 : WSAP_VTCORE_FEED_ONLCR;
    for (uint64_t left = bytes; left;) {
        size_t n = left < sizeof(payload) ? (size_t)left : sizeof(payload);
        const char *data = (const char *)payload_at(s->workload, bytes - left);
        if (!wsap_vtcore_feed(term, data, n, flags))
            fail("reference feed");
        left -= n;
    }
    wsap_vtcore_feed(term, finish, sizeof(finish) - 1, flags);
    return term;
}

static bool same_screen(struct wsap_vtcore *actual, struct wsap_vtcore *expected)
{
    struct wsap_vtcore_snapshot *a = wsap_vtcore_snapshot_new();
    struct wsap_vtcore_snapshot *b = wsap_vtcore_snapshot_new();
    if (!a || !b)
        fail("snapshot_new");
    wsap_vtcore_snapshot_update(a, actual);
    wsap_vtcore_snapshot_update(b, expected);
    const struct wsap_vtcore_view *av = wsap_vtcore_snapshot_view(a);
    const struct wsap_vtcore_view *bv = wsap_vtcore_snapshot_view(b);
    bool ok = av->size.x == bv->size.x && av->size.y == bv->size.y &&
              av->cursor.pos.x == bv->cursor.pos.x &&
              av->cursor.pos.y == bv->cursor.pos.y &&
              av->cursor.style == bv->cursor.style &&
              av->cursor.visible == bv->cursor.visible &&
              av->cursor.blink == bv->cursor.blink &&
              av->reverse_video == bv->reverse_video;
    if (!ok)
        fprintf(stderr, "view: cursor %d,%d / %d,%d, history %d / %d\n",
                av->cursor.pos.x, av->cursor.pos.y, bv->cursor.pos.x, bv->cursor.pos.y,
                av->history_lines, bv->history_lines);
    for (int y = 0; ok && y < av->size.y; y++) {
        const struct wsap_vtcore_cell *ar = wsap_vtcore_snapshot_row(a, y);
        const struct wsap_vtcore_cell *br = wsap_vtcore_snapshot_row(b, y);
        for (int x = 0; ok && x < av->size.x; x++) {
            uint32_t ac[128], bc[128];
            int an = wsap_vtcore_snapshot_cluster(a, ar[x].text, ac, 128);
            int bn = wsap_vtcore_snapshot_cluster(b, br[x].text, bc, 128);
            ok = an >= 0 && an <= 128 && an == bn &&
                 !memcmp(ac, bc, (size_t)an * sizeof(*ac)) &&
                 ar[x].attrs == br[x].attrs && ar[x].width == br[x].width &&
                 wsap_vtcore_view_color(av, ar[x].fg) == wsap_vtcore_view_color(bv, br[x].fg) &&
                 wsap_vtcore_view_color(av, ar[x].bg) == wsap_vtcore_view_color(bv, br[x].bg);
            if (!ok)
                fprintf(stderr, "cell %d,%d: text %x / %x, width %u / %u\n",
                        x, y, ar[x].text, br[x].text, ar[x].width, br[x].width);
        }
    }
    char *ah = wsap_vtcore_history_text(actual, 1000);
    char *bh = wsap_vtcore_history_text(expected, 1000);
    bool history_ok = ah && bh;
    if (history_ok) {
        size_t al = strlen(ah), bl = strlen(bh);
        size_t common = al < bl ? al : bl;
        size_t difference = al > bl ? al - bl : bl - al;
        /* The first retained row can be a partial wrapped row. */
        history_ok = difference < (size_t)av->size.x * 4 &&
                     !memcmp(ah + al - common, bh + bl - common, common);
    }
    if (!history_ok)
        fprintf(stderr, "history suffix differs\n");
    ok = ok && history_ok;
    wsap_vtcore_text_free(ah);
    wsap_vtcore_text_free(bh);
    wsap_vtcore_snapshot_free(a);
    wsap_vtcore_snapshot_free(b);
    return ok;
}

static bool run(const struct scenario *s, size_t index, uint64_t bytes,
                const char *exe, struct wsap_vtcore *expected,
                double *wall, double *cpu, int *pipe_size)
{
    char dir[] = "/tmp/wsap-ingest-XXXXXX";
    if (!mkdtemp(dir))
        fail("mkdtemp");
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/control", dir);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(listener, 1) < 0)
        fail("control socket");
    char command[8192];
    snprintf(command, sizeof(command), "\"%s\" --writer %zu %llu %s", exe,
             index, (unsigned long long)bytes, addr.sun_path);
    if (setenv("WSAP_FAST_PIPE", s->pipe ? "1" : "0", 1) < 0)
        fail("setenv");
    struct yasl_loop *loop = yasl_loop_create();
    if (!loop)
        fail("yasl_loop_create");
    if (yasl_loop_watch_signals(loop, (const int[]){SIGCHLD}, 1, on_signal, NULL) < 0)
        fail("yasl_loop_watch_signals");
    struct session session;
    if (!session_init(&session, loop, &(struct session_spec){
            .command = command, .grid = { .x = 120, .y = 40 },
            .scrollback = 1000,
        }))
        fail("session_init");
    struct measurement m = { .session = &session, .loop = loop };
    const struct session_host host = { .title = on_title, .exited = on_exited };
    session_set_host(&session, &host, &m);
    struct pollfd pfd = { .fd = listener, .events = POLLIN };
    int ready;
    do { ready = poll(&pfd, 1, 30000); } while (ready < 0 && errno == EINTR);
    int control = ready > 0 ? accept4(listener, NULL, NULL, SOCK_CLOEXEC) : -1;
    bool ok = control >= 0 && read_byte(control, 'R') && session.ring.active &&
              (pty_pipe_fd(&session.pty) >= 0) == s->pipe;
    *pipe_size = s->pipe ? fcntl(pty_pipe_fd(&session.pty), F_GETPIPE_SZ) : 0;
    uint64_t timer = yasl_loop_add_timer(loop, TIMEOUT_NS, on_timeout, loop);
    if (!timer)
        fail("yasl_loop_add_timer");
    uint64_t cpu_start = process_cpu_ns();
    uint64_t start = bench_now_ns();
    if (ok)
        ok = write_all(control, "G", 1) && yasl_loop_run(loop) == 0 && m.end_ns;
    if (ok) {
        *wall = (double)(m.end_ns - start) / bytes;
        *cpu = (double)(m.cpu_ns - cpu_start) / bytes;
    }
    yasl_loop_del_timer(loop, timer);
    if (ok)
        ok = read_byte(control, 'W') && same_screen(session.term, expected);
    if (ok)
        ok = write_all(control, "A", 1);
    /* Reap the writer before the next timed sample. */
    uint64_t exit_deadline = bench_now_ns() + TIMEOUT_NS;
    while (ok && !m.exited && bench_now_ns() < exit_deadline)
        ok = yasl_loop_step(loop, 1000) >= 0;
    ok = ok && m.exited;
    if (!ok)
        fprintf(stderr, "%s: writer, parse completion, or screen check failed\n", s->name);
    if (control >= 0)
        close(control);
    session_fini(&session);
    yasl_loop_unwatch_signals(loop);
    yasl_loop_destroy(loop);
    close(listener);
    unlink(addr.sun_path);
    rmdir(dir);
    return ok;
}

int main(int argc, char **argv)
{
    if (argc == 5 && !strcmp(argv[1], "--writer")) {
        unsigned long index = strtoul(argv[2], NULL, 10);
        if (index >= sizeof(scenarios) / sizeof(*scenarios))
            return 1;
        return writer(&scenarios[index], strtoull(argv[3], NULL, 10), argv[4]);
    }
    const char *filter = argc > 1 ? argv[1] : "all";
    char *end;
    unsigned long mib = argc > 2 ? strtoul(argv[2], &end, 10) : 64;
    if (argc > 2 && *end)
        mib = 0;
    unsigned long runs = argc > 3 ? strtoul(argv[3], &end, 10) : 5;
    if (argc > 3 && *end)
        runs = 0;
    if (argc > 4 || mib < 1 || mib > 16384 || runs < 1 || runs > 99) {
        fprintf(stderr, "usage: %s [all|case] [MiB:1..16384] [runs:1..99]\n", argv[0]);
        return 1;
    }
    char exe[4096];
    if (platform_exe_path(exe, sizeof(exe)) < 0 || strpbrk(exe, "\"\\$`"))
        fail("executable path");
    bool matched = false;
    printf("Session ingestion: %lu MiB of source bytes, 1 warmup, %lu runs\n", mib, runs);
    puts("CPU time covers the session process and its worker. It excludes the writer.");
    for (size_t i = 0; i < sizeof(scenarios) / sizeof(*scenarios); i++) {
        const struct scenario *s = &scenarios[i];
        if (strcmp(filter, "all") && strcmp(filter, s->name))
            continue;
        matched = true;
        uint64_t bytes = (uint64_t)mib * 1024 * 1024;
        struct wsap_vtcore *expected = reference(s, bytes);
        double wall[100], cpu[100];
        int pipe_size = 0;
        for (unsigned long n = 0; n <= runs; n++) {
            if (!run(s, i, bytes, exe, expected, &wall[n], &cpu[n], &pipe_size))
                return 1;
        }
        struct bench_stats w = bench_stats(wall + 1, (int)runs);
        struct bench_stats c = bench_stats(cpu + 1, (int)runs);
        printf("%-18s %9.1f MiB/s [%.1f, %.1f], wall %.4f ns/B, CPU %.4f ns/B, pipe %d KiB\n",
               s->name, 1e9 / (1024 * 1024 * w.median),
               1e9 / (1024 * 1024 * w.max), 1e9 / (1024 * 1024 * w.min),
               w.median, c.median, pipe_size / 1024);
        fflush(stdout);
        wsap_vtcore_free(expected);
    }
    if (!matched)
        fprintf(stderr, "unknown case: %s\n", filter);
    return matched ? 0 : 1;
}
