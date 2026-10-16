/* Synthetic input to matching frame submission and compositor presentation.
 * See input-latency-bench.md for methodology and limitations. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#include "loop/latency_probe.h"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}
static void sleep_ns(uint64_t ns)
{
    struct timespec ts = { .tv_sec = ns / 1000000000ull, .tv_nsec = ns % 1000000000ull };
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR) {}
}
/* ---------------------------------------------------------------- inject */

#if defined(__linux__)
#include <linux/uinput.h>
#include <sys/ioctl.h>

static int uinput_fd = -1;

static bool uinput_open(void)
{
    uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (uinput_fd < 0) {
        fprintf(stderr, "open /dev/uinput: %s\n"
                "  the invoking user must be able to write it "
                "(group 'input' on most distributions)\n", strerror(errno));
        return false;
    }
    /* libinput only classifies the device as a keyboard if it carries a
     * plausible key range, so claim the usual block rather than one code. */
    bool ok = ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) == 0;
    for (int code = KEY_ESC; ok && code <= KEY_COMPOSE; code++)
        ok = ioctl(uinput_fd, UI_SET_KEYBIT, code) == 0;

    struct uinput_setup setup = {
        .id = { .bustype = BUS_VIRTUAL, .vendor = 0x1d1d, .product = 0x0001 },
        .name = "wsap_term latency bench",
    };
    ok = ok && ioctl(uinput_fd, UI_DEV_SETUP, &setup) == 0 &&
         ioctl(uinput_fd, UI_DEV_CREATE) == 0;
    if (!ok) {
        fprintf(stderr, "uinput setup: %s\n", strerror(errno));
        close(uinput_fd);
        uinput_fd = -1;
    }
    return ok;
}

static bool emit(uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev = { .type = type, .code = code, .value = value };
    return write(uinput_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev);
}

static bool uinput_key(void)
{
    return emit(EV_KEY, KEY_A, 1) && emit(EV_SYN, SYN_REPORT, 0) &&
           emit(EV_KEY, KEY_A, 0) && emit(EV_SYN, SYN_REPORT, 0);
}

static void uinput_close(void)
{
    if (uinput_fd >= 0) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        uinput_fd = -1;
    }
}

#if defined(WSAP_LATENCY_WAYLAND)
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"

/* zwp_virtual_keyboard_v1 types into the compositor named by
 * WAYLAND_DISPLAY. Unlike uinput it reaches only that compositor, so it
 * also works against a private headless one. */
static struct wl_display *wl_dpy;
static struct wl_seat *wl_seat;
static struct zwp_virtual_keyboard_manager_v1 *vk_manager;
static struct zwp_virtual_keyboard_v1 *vk;

static void on_global(void *data, struct wl_registry *reg, uint32_t name,
                      const char *iface, uint32_t version)
{
    (void)data;
    (void)version;
    if (!strcmp(iface, wl_seat_interface.name) && !wl_seat)
        wl_seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
        vk_manager = wl_registry_bind(reg, name,
                                      &zwp_virtual_keyboard_manager_v1_interface, 1);
}

static void on_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = on_global,
    .global_remove = on_global_remove,
};

static bool upload_keymap(void)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names names = { .layout = "us" };
    struct xkb_keymap *map = ctx ? xkb_keymap_new_from_names(ctx, &names, 0) : NULL;
    char *text = map ? xkb_keymap_get_as_string(map, XKB_KEYMAP_FORMAT_TEXT_V1) : NULL;
    bool ok = false;
    if (text) {
        size_t size = strlen(text) + 1;
        int fd = memfd_create("wsap-latency-keymap", MFD_CLOEXEC);
        if (fd >= 0 && write(fd, text, size) == (ssize_t)size) {
            zwp_virtual_keyboard_v1_keymap(vk, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
                                           fd, (uint32_t)size);
            ok = wl_display_roundtrip(wl_dpy) >= 0;
        }
        if (fd >= 0)
            close(fd);
    }
    free(text);
    xkb_keymap_unref(map);
    xkb_context_unref(ctx);
    return ok;
}

static bool wayland_open(void)
{
    wl_dpy = wl_display_connect(NULL);
    if (!wl_dpy) {
        fprintf(stderr, "wayland injector: cannot connect to WAYLAND_DISPLAY\n");
        return false;
    }
    struct wl_registry *reg = wl_display_get_registry(wl_dpy);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    if (wl_display_roundtrip(wl_dpy) < 0 || !wl_seat || !vk_manager) {
        fprintf(stderr, "wayland injector: compositor has no "
                        "zwp_virtual_keyboard_manager_v1 or wl_seat\n");
        return false;
    }
    vk = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(vk_manager, wl_seat);
    if (!upload_keymap()) {
        fprintf(stderr, "wayland injector: keymap upload failed\n");
        return false;
    }
    return true;
}

static bool wayland_key(void)
{
    uint32_t t = (uint32_t)(now_ns() / 1000000);
    zwp_virtual_keyboard_v1_key(vk, t, KEY_A, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(vk, t, KEY_A, WL_KEYBOARD_KEY_STATE_RELEASED);
    return wl_display_flush(wl_dpy) >= 0;
}

static void wayland_close(void)
{
    if (vk)
        zwp_virtual_keyboard_v1_destroy(vk);
    vk = NULL;
    if (wl_dpy)
        wl_display_disconnect(wl_dpy);
    wl_dpy = NULL;
}
#endif

/* WSAP_LATENCY_INJECTOR=wayland selects the Wayland virtual keyboard;
 * the default is uinput. */
static bool use_wayland(void)
{
#if defined(WSAP_LATENCY_WAYLAND)
    const char *sel = getenv("WSAP_LATENCY_INJECTOR");
    return sel && !strcmp(sel, "wayland");
#else
    return false;
#endif
}

static bool injector_open(void)
{
#if defined(WSAP_LATENCY_WAYLAND)
    if (use_wayland())
        return wayland_open();
#endif
    return uinput_open();
}

static bool injector_key(void)
{
#if defined(WSAP_LATENCY_WAYLAND)
    if (use_wayland())
        return wayland_key();
#endif
    return uinput_key();
}

static void injector_close(void)
{
#if defined(WSAP_LATENCY_WAYLAND)
    if (use_wayland()) {
        wayland_close();
        return;
    }
#endif
    uinput_close();
}

static const char *injector_name(void)
{
    return use_wayland() ? "zwp_virtual_keyboard_v1 (Wayland -> compositor)"
                         : "uinput (evdev -> compositor)";
}

#elif defined(__APPLE__)
#include <ApplicationServices/ApplicationServices.h>

static bool injector_open(void)
{
    if (AXIsProcessTrusted())
        return true;
    fprintf(stderr, "not trusted for accessibility; grant it under System "
                    "Settings > Privacy & Security > Accessibility\n");
    return false;
}

static bool post(bool down)
{
    CGEventRef ev = CGEventCreateKeyboardEvent(NULL, (CGKeyCode)0 /* A */, down);
    if (!ev)
        return false;
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
    return true;
}

static bool injector_key(void) { return post(true) && post(false); }
static void injector_close(void) {}
static const char *injector_name(void) { return "CGEventPost (HID tap)"; }

#else
#error "no input injector for this platform"
#endif


static volatile sig_atomic_t stopped;
static void on_signal(int sig) { stopped = sig; }

static int echo_helper(void)
{
    struct termios t;
    if (tcgetattr(STDIN_FILENO, &t) < 0) return 1;
    cfmakeraw(&t);
    if (tcsetattr(STDIN_FILENO, TCSANOW, &t) < 0) return 1;
    const char *path = getenv("WSAP_LATENCY_PROBE");
    int fd = path ? open(path, O_RDWR | O_CLOEXEC) : -1;
    if (fd < 0) return 1;
    struct latency_probe_shm *probe = mmap(NULL, sizeof(*probe), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (probe == MAP_FAILED) return 1;
    atomic_store_explicit(&probe->ready, 1, memory_order_release);
    unsigned long long seq = 0;
    char c;
    while (read(STDIN_FILENO, &c, 1) == 1) {
        if (c != 'a') continue;
        char reply[96];
        int n = snprintf(reply, sizeof(reply), "\033]777;latency=%llu\007a", ++seq);
        for (int off = 0; off < n;) {
            ssize_t w = write(STDOUT_FILENO, reply + off, (size_t)(n - off));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) return 1;
            off += (int)w;
        }
    }
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}
static void stats(const char *name, uint64_t *values, int n)
{
    if (!n) return;
    double sum = 0;
    for (int i = 0; i < n; i++) sum += values[i];
    qsort(values, (size_t)n, sizeof(*values), cmp_u64);
    printf("%-29s %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f\n", name,
        values[0] / 1e6, sum / n / 1e6, values[(n - 1) / 2] / 1e6,
        values[((size_t)n * 95 + 99) / 100 - 1] / 1e6,
        values[((size_t)n * 99 + 99) / 100 - 1] / 1e6, values[n - 1] / 1e6);
}
static bool number(const char *arg, int *value, bool zero)
{
    char *end;
    errno = 0;
    long v = strtol(arg, &end, 10);
    if (errno || !*arg || *end || v < (zero ? 0 : 1) || v > 1000000) return false;
    *value = (int)v;
    return true;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--echo")) return echo_helper();
    int iterations = 300, warmup = 20;
    if (argc > 4 || (argc > 1 && !number(argv[1], &iterations, false)) ||
        (argc > 2 && !number(argv[2], &warmup, true))) {
        fprintf(stderr, "usage: %s [samples [warmup [output.csv]]]\n", argv[0]);
        return 1;
    }
    char self[PATH_MAX];
    if (!realpath(argv[0], self) || strpbrk(self, " \t\r\n")) {
        fprintf(stderr, "cannot resolve a safe helper path\n"); return 1;
    }
    const char *bin = getenv("WSAP_TERM_BIN");
    char sibling_bin[PATH_MAX];
    if (!bin) {
        snprintf(sibling_bin, sizeof(sibling_bin), "%s", self);
        char *slash = strrchr(sibling_bin, '/');
        if (slash) *slash = '\0';
        slash = strrchr(sibling_bin, '/');
        if (slash) *slash = '\0';
        size_t used = strlen(sibling_bin);
        if (slash && used + sizeof("/bin/wsap_term") <= sizeof(sibling_bin)) {
            strcat(sibling_bin, "/bin/wsap_term");
            if (!access(sibling_bin, X_OK))
                bin = sibling_bin;
        }
        if (!bin) bin = "build/bin/wsap_term";
    }
    if (access(bin, X_OK)) { perror(bin); return 1; }
    FILE *csv = argc > 3 ? fopen(argv[3], "wx") : NULL;
    if (argc > 3 && !csv) { perror(argv[3]); return 1; }
    if (csv) fputs("sequence,warmup,status,inject_ns,key_ns,queued_ns,parsed_ns,render_ns,commit_ns,presentation_observed_ns,presentation_flags,presentation_refresh_ns\n", csv);
    char dir[] = "/tmp/wsap-latency-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char probe_path[128], conf_dir[128], conf_path[160];
    snprintf(probe_path, sizeof(probe_path), "%s/probe", dir);
    snprintf(conf_dir, sizeof(conf_dir), "%s/wsap_term", dir);
    snprintf(conf_path, sizeof(conf_path), "%s/term.conf", conf_dir);
    int result = 1, n = 0, pn = 0;
    double max_presentation_frames = 0.0;
    pid_t pid = -1;
    struct latency_probe_shm *probe = MAP_FAILED;
    uint64_t *values = calloc((size_t)iterations * 8, sizeof(*values));
    if (!values) goto cleanup;
    FILE *conf = mkdir(conf_dir, 0700) == 0 ? fopen(conf_path, "w") : NULL;
    if (!conf) { perror("config"); goto cleanup; }
    fprintf(conf, "cursor-blink = false\nvsync = false\nfullscreen = true\n"
                   "background = #000000\n"
                   "command = %s --echo\nwidth = 800\nheight = 600\n", self);
    fclose(conf);
    int fd = open(probe_path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) { perror("probe"); goto cleanup; }
    if (ftruncate(fd, sizeof(*probe))) { close(fd); goto cleanup; }
    probe = mmap(NULL, sizeof(*probe), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (probe == MAP_FAILED) goto cleanup;
    memset(probe, 0, sizeof(*probe));
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    pid = fork();
    if (pid < 0) goto cleanup;
    if (!pid) {
        setenv("WSAP_LATENCY_PROBE", probe_path, 1);
        setenv("XDG_CONFIG_HOME", dir, 1);
        execl(bin, bin, (char *)NULL);
        _exit(127);
    }
    /* A virtual keyboard is what gives a keyboard-less compositor's seat
     * its keyboard, so the terminal cannot take focus before it exists. */
    bool injector_ready = use_wayland() && injector_open();
    if (use_wayland() && !injector_ready) goto cleanup;
    uint64_t deadline = now_ns() + 15000000000ull;
    while (!atomic_load_explicit(&probe->focused, memory_order_relaxed)) {
        if (waitpid(pid, NULL, WNOHANG) == pid) { pid = -1; goto cleanup; }
        if (stopped || now_ns() > deadline) { fprintf(stderr, "terminal did not take focus\n"); goto cleanup; }
        sleep_ns(10000000);
    }
    deadline = now_ns() + 5000000000ull;
    while (!atomic_load_explicit(&probe->ready, memory_order_acquire)) {
        if (stopped || now_ns() > deadline) { fprintf(stderr, "echo helper did not become ready\n"); goto cleanup; }
        sleep_ns(1000000);
    }
    deadline = now_ns() + 5000000000ull;
    while (!atomic_load_explicit(&probe->fullscreen, memory_order_acquire)) {
        if (stopped || now_ns() > deadline) {
            fprintf(stderr, "compositor did not confirm fullscreen\n");
            goto cleanup;
        }
        sleep_ns(1000000);
    }
    if (!injector_ready && !injector_open()) goto cleanup;
    sleep_ns(700000000);
    bool presentation = atomic_load_explicit(&probe->presentation_supported, memory_order_acquire);
    printf("terminal: %s\ninjector: %s\nclock: CLOCK_MONOTONIC\n", bin, injector_name());
    printf("presentation: %s\nworkload: raw userspace echo; 30–50 ms deterministic jitter between samples\n",
           presentation ? "Wayland feedback; async mode uses callback receipt time" : "unavailable; submission only");
    puts("fullscreen: confirmed by xdg_toplevel.configure\n"
         "presentation mode: fullscreen; asynchronous presentation requested");
    uint64_t focus_losses = atomic_load_explicit(&probe->focus_losses, memory_order_relaxed);
    unsigned rng = 1;
    int zero_copy_presentations = 0;
    result = 0;
    for (int i = 0; i < iterations + warmup; i++) {
        rng = rng * 1664525u + 1013904223u;
        sleep_ns(30000000ull + rng % 20000001u);
        if (stopped || !atomic_load_explicit(&probe->focused, memory_order_relaxed) ||
            atomic_load_explicit(&probe->focus_losses, memory_order_relaxed) != focus_losses) {
            fprintf(stderr, "interrupted or focus lost\n"); result = 1; break;
        }
        uint64_t id = (uint64_t)i + 1;
        atomic_store_explicit(&probe->request, id, memory_order_release);
        uint64_t start = now_ns();
        if (!injector_key()) { perror("injection"); result = 1; break; }
        deadline = start + 1000000000ull;
        bool committed = false, shown = false;
        while (!stopped && now_ns() < deadline) {
            committed = atomic_load_explicit(&probe->completed, memory_order_acquire) == id;
            shown = presentation && atomic_load_explicit(&probe->presented, memory_order_acquire) == id;
            if (committed && (!presentation || shown)) break;
            sleep_ns(50000);
        }
        const char *status = "ok";
        uint64_t t[7] = { start };
        uint32_t refresh_ns = shown ? probe->presentation_refresh_ns : 0;
        uint32_t presentation_flags = shown ? probe->presentation_flags : 0;
        if (committed) memcpy(t + 1, probe->stage_ns, sizeof(probe->stage_ns));
        if (shown) {
            t[6] = presentation_flags & LATENCY_PRESENTATION_VSYNC
                ? probe->presentation_ns : probe->presentation_received_ns;
        }
        if (!committed || (presentation && !shown)) status = "timeout";
        else if (shown && probe->discarded) status = "discarded";
        else if (shown && !refresh_ns) status = "missing-refresh";
        else if (shown && i >= warmup && t[6] - start > (uint64_t)refresh_ns * 2) {
            double frames = (double)(t[6] - start) / refresh_ns;
            if (frames > max_presentation_frames)
                max_presentation_frames = frames;
            fprintf(stderr, "sample %llu: %.3f ms = %.2f frames (refresh %.3f ms)\n",
                    (unsigned long long)id, (t[6] - start) / 1e6,
                    frames, refresh_ns / 1e6);
            status = "over-two-frames";
        } else {
            for (int j = 1; j < (presentation ? 7 : 6); j++)
                if (!t[j] || t[j] < t[j - 1]) status = "invalid-timestamps";
            if (!strcmp(status, "invalid-timestamps")) {
                fprintf(stderr, "timestamps from injection (ms):");
                for (int j = 0; j < 7; j++)
                    fprintf(stderr, " %d=%.3f", j,
                            t[j] ? ((double)t[j] - (double)start) / 1e6 : -1.0);
                fprintf(stderr, " flags=%u refresh=%.3f ms\n",
                        presentation_flags,
                        refresh_ns / 1e6);
            }
        }
        if (!atomic_load_explicit(&probe->focused, memory_order_relaxed) ||
            atomic_load_explicit(&probe->focus_losses, memory_order_relaxed) != focus_losses) status = "focus-lost";
        if (csv) {
            fprintf(csv, "%llu,%d,%s", (unsigned long long)id, i < warmup, status);
            for (int j = 0; j < 7; j++) fprintf(csv, ",%llu", (unsigned long long)t[j]);
            fprintf(csv, ",%u,%u\n", presentation_flags,
                    refresh_ns);
        }
        if (strcmp(status, "ok")) {
            fprintf(stderr, "sample %llu: %s; stopping to avoid misattributing later input\n", (unsigned long long)id, status);
            result = 1; break;
        }
        if (i < warmup) continue;
        values[n] = t[5] - start;
        for (int j = 1; j <= 5; j++) values[(size_t)j * iterations + n] = t[j] - t[j - 1];
        if (shown) {
            values[(size_t)6 * iterations + pn] = t[6] - start;
            values[(size_t)7 * iterations + pn++] = t[6] - t[5];
            if (i >= warmup) {
                double frames = (double)(t[6] - start) / refresh_ns;
                if (frames > max_presentation_frames)
                    max_presentation_frames = frames;
                if (presentation_flags & LATENCY_PRESENTATION_ZERO_COPY)
                    zero_copy_presentations++;
            }
        }
        n++;
    }
    printf("\nvalid samples: %d/%d (warmup: %d); units: ms\n", n, iterations, warmup);
    printf("%-29s %8s %8s %8s %8s %8s %8s\n", "Measurement", "min", "mean", "p50", "p95", "p99", "max");
    const char *names[] = { "injection -> submission", "injection -> key callback", "key -> input queued", "queued -> echo parsed", "parsed -> render start", "render -> submission", "injection -> presentation", "submission -> presentation" };
    for (int j = 0; j < 8; j++) stats(names[j], values + (size_t)j * iterations, j < 6 ? n : pn);
    if (presentation)
        printf("input -> presentation max: %.2f frames (limit: 2.00)\n"
               "zero-copy presentations: %d/%d\n",
               max_presentation_frames, zero_copy_presentations, n);
    else
        puts("input -> presentation frame limit: unavailable");
    if (!n) result = 1;
cleanup:
    injector_close();
    if (pid > 0) { kill(pid, SIGTERM); waitpid(pid, NULL, 0); }
    if (probe != MAP_FAILED) munmap(probe, sizeof(*probe));
    if (csv && fclose(csv)) result = 1;
    unlink(conf_path); rmdir(conf_dir); unlink(probe_path); rmdir(dir);
    free(values);
    return result;
}
