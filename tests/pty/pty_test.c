#define _GNU_SOURCE
#include "pty/pty.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <termios.h>
#include <unistd.h>

static int spawn(struct pty *p, const char *cmd)
{
    return pty_spawn(p, &(struct pty_spec){
        .grid = { .x = 80, .y = 24 },
        .command = cmd,
    });
}

/* Read child output. It arrives on the fast pipe when there is one. */
static size_t drain_child(struct pty *p, unsigned char *out, size_t cap)
{
    int fd = pty_pipe_fd(p) >= 0 ? pty_pipe_fd(p) : pty_master_fd(p);
    size_t used = 0;
    for (int idle = 0; idle < 400 && used < cap;) {
        ssize_t n = read(fd, out + used, cap - used);
        if (n > 0) {
            used += (size_t)n;
            idle = 0;
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            struct pollfd pfd = { .fd = fd, .events = POLLIN };
            poll(&pfd, 1, 10);
            idle++;
            continue;
        }
        break;
    }
    return used;
}

static void test_child_output_is_byte_exact(void)
{
    enum { PAYLOAD = 64 * 1024 };
    static unsigned char want[PAYLOAD];
    uint64_t s = 0x243F6A8885A308D3ull;
    for (size_t i = 0; i < PAYLOAD; i++) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        want[i] = (unsigned char)s;
    }

    char path[] = "/tmp/wsap-pty-fidelity-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    assert(write(fd, want, PAYLOAD) == (ssize_t)PAYLOAD);
    close(fd);

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/bin/sh -c 'stty -opost; cat %s'", path);

    struct pty p;
    memset(&p, 0, sizeof(p));
    assert(spawn(&p, cmd) == 0);

    static unsigned char got[PAYLOAD * 2];
    size_t n = drain_child(&p, got, sizeof(got));
    pty_destroy(&p);
    unlink(path);

    assert(n == PAYLOAD);
    assert(!memcmp(got, want, PAYLOAD));
}

static void expect_env(const char *cmd, const char *needle, const char *alt)
{
    struct pty p;
    memset(&p, 0, sizeof(p));
    assert(spawn(&p, cmd) == 0);

    unsigned char buf[512];
    size_t n = drain_child(&p, buf, sizeof(buf) - 1);
    pty_destroy(&p);

    assert(n > 0);
    buf[n] = '\0';
    assert(strstr((char *)buf, needle) != NULL ||
           (alt && strstr((char *)buf, alt) != NULL));
}

static void test_spawn_locale_env(void)
{
    unsetenv("LANG");
    unsetenv("LC_ALL");
    unsetenv("LC_CTYPE");
    expect_env("printenv LANG", "UTF-8", "utf8");

    setenv("LANG", "test_LOCALE.UTF-8", 1);
    expect_env("printenv LANG", "test_LOCALE.UTF-8", NULL);
}

static void test_spawn_term_env(void)
{
#ifdef WSAP_DEV_TERMINFO_DIR
    expect_env("printenv TERMINFO_DIRS", WSAP_DEV_TERMINFO_DIR, NULL);
    expect_env("printenv TERM", "wsap_term-256color", NULL);
#else
    expect_env("printenv TERMINFO_DIRS", "terminfo", NULL);
    expect_env("printenv TERM", "wsap_term-256color", "xterm-256color");
#endif
}

static void test_spec_env_overrides(void)
{
    static const char *const env[] = {
        "TERM=dumb",
        "WSAP_SPEC_VAR=from_spec",
        NULL,
    };
    struct pty p = {0};
    assert(pty_spawn(&p, &(struct pty_spec){
        .grid = { .x = 80, .y = 24 },
        /* BSD printenv prints only its first name. */
        .command = "/bin/sh -c 'echo $TERM $WSAP_SPEC_VAR'",
        .env = env,
    }) == 0);

    unsigned char buf[512];
    size_t n = drain_child(&p, buf, sizeof(buf) - 1);
    pty_destroy(&p);

    buf[n] = '\0';
    assert(strstr((char *)buf, "dumb") != NULL);
    assert(strstr((char *)buf, "from_spec") != NULL);
}

static void test_master_fd_is_cloexec(void)
{
    struct pty p;
    memset(&p, 0, sizeof(p));
    assert(spawn(&p, "true") == 0);
    int flags = fcntl(pty_master_fd(&p), F_GETFD);
    pty_destroy(&p);
    assert(flags >= 0);
    assert(flags & FD_CLOEXEC);
}

static void test_priv_cmd_spelling_no_fast_pipe(void)
{
    char dir[] = "/tmp/wsap-privcmd-XXXXXX";
    assert(mkdtemp(dir) != NULL);
    char script[256];
    snprintf(script, sizeof(script), "%s/sudo", dir);
    FILE *f = fopen(script, "w");
    assert(f);
    fputs("#!/bin/sh\necho FASTPIPE=${WSAP_FAST_PIPE:-unset}\n", f);
    fclose(f);
    assert(chmod(script, 0755) == 0);

    const char *cmds[] = {
        "  SUDO",
        "env SUDO",
        "env A=1 SUDO",
        "env -u A SUDO",
        "nice -n 5 SUDO",
        NULL,
    };
    for (size_t i = 0; cmds[i]; i++) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "%s", cmds[i]);
        char *ph = strstr(cmd, "SUDO");
        if (ph) {
            char expanded[512];
            snprintf(expanded, sizeof(expanded), "%.*s%s%s",
                     (int)(ph - cmd), cmd, script, ph + 4);
            snprintf(cmd, sizeof(cmd), "%s", expanded);
        }

        struct pty p;
        memset(&p, 0, sizeof(p));
        assert(spawn(&p, cmd) == 0);
        unsigned char buf[256];
        size_t n = drain_child(&p, buf, sizeof(buf) - 1);
        pty_destroy(&p);
        buf[n] = '\0';
        assert(strstr((char *)buf, "FASTPIPE=unset") != NULL);
    }

    unlink(script);
    rmdir(dir);
}

static char *write_script(char *dir, const char *body)
{
    assert(mkdtemp(dir) != NULL);
    static char script[256];
    snprintf(script, sizeof(script), "%s/script", dir);
    FILE *f = fopen(script, "w");
    assert(f);
    fputs(body, f);
    fclose(f);
    assert(chmod(script, 0755) == 0);
    return script;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* A shell saves history on SIGHUP. Closing a tab must give it that
 * chance instead of killing it outright, and must not block the caller
 * while it does. */
static void test_destroy_lets_child_handle_hangup(void)
{
    char dir[] = "/tmp/wsap-hangup-XXXXXX";
    char *script = write_script(dir,
        "#!/bin/sh\n"
        "trap 'sleep 0.2; echo saved > \"$(dirname \"$0\")/marker\"; exit 0' HUP\n"
        "echo ready\n"
        "while :; do sleep 1 & wait; done\n");
    char marker[256];
    snprintf(marker, sizeof(marker), "%s/marker", dir);

    struct pty p;
    memset(&p, 0, sizeof(p));
    assert(spawn(&p, script) == 0);
    unsigned char buf[64];
    assert(drain_child(&p, buf, 5) == 5 && !memcmp(buf, "ready", 5));

    uint64_t t0 = now_ms();
    pty_destroy(&p);
    assert(now_ms() - t0 < 100);

    bool saved = false;
    for (int i = 0; i < 300 && !saved; i++) {
        saved = access(marker, F_OK) == 0;
        if (!saved)
            usleep(10000);
    }
    unlink(marker);
    unlink(script);
    rmdir(dir);
    assert(saved);
}

#ifdef __linux__
static void test_child_inherits_only_stdio(void)
{
    struct pty p;
    memset(&p, 0, sizeof(p));
    assert(spawn(&p, "cat /proc/self/fdinfo/3") == 0);
    unsigned char buf[1024];
    size_t n = drain_child(&p, buf, sizeof(buf) - 1);
    pty_destroy(&p);
    buf[n] = '\0';
    assert(strstr((char *)buf, "No such file") != NULL);
}
#endif

static void test_command_quoting(void)
{
    expect_env("printf [%s] \"a b\" 'c d' e\\ f \"\"", "[a b][c d][e f][]", NULL);
}

static void test_empty_and_fd_zero(void)
{
    for (int failed_spawn = 0; failed_spawn < 2; failed_spawn++) {
        pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            struct pty p = {0};
            int fd = open("/dev/null", O_RDWR);
            assert(fd >= 0);
            assert(dup2(fd, STDIN_FILENO) == STDIN_FILENO);
            if (fd != STDIN_FILENO) close(fd);
            if (failed_spawn) {
                struct rlimit limit;
                assert(getrlimit(RLIMIT_NOFILE, &limit) == 0);
                limit.rlim_cur = 0;
                assert(setrlimit(RLIMIT_NOFILE, &limit) == 0);
                assert(spawn(&p, "true") < 0);
                pty_destroy(&p);
                assert(fcntl(STDIN_FILENO, F_GETFD) >= 0);
            } else {
                close(STDIN_FILENO);
                assert(spawn(&p, "true") == 0);
                assert(pty_master_fd(&p) == STDIN_FILENO);
                assert(isatty(pty_master_fd(&p)));
                pty_destroy(&p);
                assert(fcntl(STDIN_FILENO, F_GETFD) < 0 && errno == EBADF);
                fd = open("/dev/null", O_RDWR);
                assert(fd == STDIN_FILENO);
                pty_destroy(&p);
                assert(fcntl(fd, F_GETFD) >= 0);
                assert(spawn(&p, "true") == 0);
                pty_destroy(&p);
            }
            assert(pty_master_fd(&p) == -1 && pty_pipe_fd(&p) == -1);
            _exit(0);
        }
        int status;
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}

static void output_mode_child(void)
{
    assert(write(STDOUT_FILENO, "cooked\n", 7) == 7);
    struct termios tio;
    assert(tcgetattr(STDIN_FILENO, &tio) == 0);
    tio.c_lflag &= ~(ICANON | ECHO);
    tio.c_oflag &= ~OPOST;
    assert(tcsetattr(STDIN_FILENO, TCSANOW, &tio) == 0);
    assert(write(STDOUT_FILENO, "raw\n", 4) == 4);
    char ack;
    assert(read(STDIN_FILENO, &ack, 1) == 1);
}

/* Read after the child changes modes. Each write keeps its own line ending. */
static void test_output_mode_changes(const char *program)
{
    const char *value = getenv("WSAP_FAST_PIPE");
    char *saved = value ? strdup(value) : NULL;
    setenv("WSAP_FAST_PIPE", "0", 1);
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "\"%s\" --output-mode-child", program);
    struct pty p = {0};
    assert(spawn(&p, cmd) == 0);
    bool raw = false;
    for (int i = 0; i < 1000 && !raw; i++) {
        struct termios tio;
        assert(tcgetattr(pty_master_fd(&p), &tio) == 0);
        raw = !(tio.c_lflag & ICANON);
        if (!raw) usleep(1000);
    }
    assert(raw);
    const char expected[] = "cooked\r\nraw\n";
    unsigned char got[64];
    size_t n = drain_child(&p, got, sizeof(expected) - 1);
    assert(write(pty_master_fd(&p), "x", 1) == 1);
    int status;
    assert(waitpid(p.pid, &status, 0) == p.pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    p.pid = 0;
    pty_destroy(&p);
    if (saved) {
        setenv("WSAP_FAST_PIPE", saved, 1);
        free(saved);
    } else {
        unsetenv("WSAP_FAST_PIPE");
    }
    assert(n == sizeof(expected) - 1);
    assert(!memcmp(got, expected, n));
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--output-mode-child")) {
        output_mode_child();
        return 0;
    }
    test_output_mode_changes(argv[0]);
    test_empty_and_fd_zero();
    test_child_output_is_byte_exact();
    test_spawn_locale_env();
    test_spawn_term_env();
    test_spec_env_overrides();
    test_master_fd_is_cloexec();
    test_priv_cmd_spelling_no_fast_pipe();
    test_command_quoting();
#ifdef __linux__
    test_child_inherits_only_stdio();
#endif
    test_destroy_lets_child_handle_hangup();
    puts("PASS: pty fast-pipe transport tests");
    return 0;
}
