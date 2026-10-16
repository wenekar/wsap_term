#include "internal.h"
#include <errno.h>
#include "common/keyboard.h"
#include "keymap.h"

#define XKB_EVDEV_OFFSET 8
#define KEYMAP_MAX_BYTES (8u * 1024 * 1024)

void keyboard_text_reset(struct seat *s)
{
    if (s->compose) xkb_compose_state_reset(s->compose);
}

void keyboard_repeat_stop(struct seat *s)
{
    if (!s->repeat_held) return;
    s->repeat_held = false;
    if (s->repeat_fd >= 0) {
        struct itimerspec off = {0};
        timerfd_settime(s->repeat_fd, 0, &off, NULL);
    }
}

void keyboard_repeat_sync(struct seat *s)
{
    if (s->repeat_fd < 0) return;
    struct itimerspec timer = {0};
    if (s->repeat_held && s->repeat_rate > 0) {
        uint64_t interval = 1000000000ull / (uint32_t)s->repeat_rate;
        if (!interval) interval = 1;
        uint64_t delay_ns = (uint64_t)s->repeat_delay * 1000000ull;
        if (!delay_ns) delay_ns = 1;
        timer.it_value = (struct timespec){ (time_t)(delay_ns / 1000000000ull), (long)(delay_ns % 1000000000ull) };
        timer.it_interval = (struct timespec){ (time_t)(interval / 1000000000ull), (long)(interval % 1000000000ull) };
    }
    timerfd_settime(s->repeat_fd, 0, &timer, NULL);
}

static const char *const input_mod_names[5] = {
    XKB_MOD_NAME_SHIFT, XKB_MOD_NAME_ALT,
    XKB_MOD_NAME_CTRL, XKB_MOD_NAME_LOGO,
    XKB_MOD_NAME_CAPS
};

static void resolve_input_mod_masks(struct seat *s)
{
    memset(s->input_mod_masks, 0, sizeof(s->input_mod_masks));
    if (!s->xkb_keymap) return;
    struct xkb_state *probe = xkb_state_new(s->xkb_keymap);
    if (!probe) return;
    for (unsigned real = 0; real < 8; real++) {
        xkb_state_update_mask(probe, 1u << real, 0, 0, 0, 0, 0);
        for (unsigned i = 0; i < 5; i++) {
            if (xkb_state_mod_name_is_active(probe, input_mod_names[i],
                    XKB_STATE_MODS_EFFECTIVE) > 0)
                s->input_mod_masks[i] |= 1u << real;
        }
    }
    xkb_state_unref(probe);
    s->input_mods_ready = true;
}

static void keyboard_keymap(void *data, struct wl_keyboard *keyboard,
        uint32_t format, int32_t fd, uint32_t size)
{
    (void)keyboard;
    struct seat *s = data;
    keyboard_repeat_stop(s);
    keyboard_text_reset(s);
    s->input_mods_ready = false;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    struct stat st;
    if (size == 0 || size > KEYMAP_MAX_BYTES ||
        fstat(fd, &st) < 0 || st.st_size < 0 || (uint64_t)st.st_size < size) {
        close(fd);
        return;
    }
    char *map_str = malloc(size);
    if (!map_str) {
        close(fd);
        return;
    }
    size_t off = 0;
    while (off < size) {
        ssize_t n = pread(fd, map_str + off, size - off, (off_t)off);
        if (n > 0) off += (size_t)n;
        else if (n == 0 || errno != EINTR) break;
    }
    close(fd);
    if (off < size) {
        free(map_str);
        return;
    }

    if (s->xkb_keymap)
        xkb_keymap_unref(s->xkb_keymap);
    if (s->xkb_state)
        xkb_state_unref(s->xkb_state);
    s->xkb_state = NULL;

    s->xkb_keymap = xkb_keymap_new_from_buffer(s->conn->xkb_ctx, map_str, size,
            XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    free(map_str);

    if (s->xkb_keymap) {
        s->xkb_state = xkb_state_new(s->xkb_keymap);
        s->mod_shift = xkb_keymap_mod_get_index(s->xkb_keymap, XKB_MOD_NAME_SHIFT);
        s->mod_alt = xkb_keymap_mod_get_index(s->xkb_keymap, XKB_MOD_NAME_ALT);
        s->mod_ctrl = xkb_keymap_mod_get_index(s->xkb_keymap, XKB_MOD_NAME_CTRL);
        s->mod_super = xkb_keymap_mod_get_index(s->xkb_keymap, XKB_MOD_NAME_LOGO);
        resolve_input_mod_masks(s);
    }
}

static void keyboard_reset_keys(struct seat *s, struct wayland_surface *w)
{
    (void)w;
    keyboard_repeat_stop(s);
    keyboard_text_reset(s);
}

bool conn_keyboard_on(const struct wayland_display *c, const struct wayland_surface *w,
                      const struct seat *except)
{
    for (const struct seat *s = c->seats; s; s = s->next)
        if (s != except && s->keyboard_focus == w)
            return true;
    return false;
}

static void push_focus(struct wayland_surface *w, bool focused)
{
    struct yasl_private_event ev = {
        .type = YASL_PRIVATE_EVENT_FOCUS,
        .surface = &w->base,
        .focus = { .focused = focused },
    };
    input_queue_push(&w->input, &ev);
}

static void keyboard_enter(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface, struct wl_array *keys)
{
    (void)keyboard; (void)keys;
    struct seat *s = data;
    struct wayland_surface *w = surface_root(surface ?
        (struct wayland_surface *)wl_surface_get_user_data(surface) : NULL);
    bool same_window = w && w == s->focus_leaving;
    if (same_window)
        s->focus_leaving = NULL;
    if (!w) {
        s->keyboard_focus = NULL;
        return;
    }
    /* The enter serial is a valid input serial, e.g. for set_selection. */
    seat_note_input(s, serial);
    bool focused = conn_keyboard_on(s->conn, w, s);
    s->keyboard_focus = w;
    keyboard_reset_keys(s, w);
    if (!same_window && !focused)
        push_focus(w, true);
}

static void seat_settle_focus(struct seat *s)
{
    struct wayland_surface *w = s->focus_leaving;
    if (!w)
        return;
    s->focus_leaving = NULL;
    if (!conn_keyboard_on(s->conn, w, NULL))
        push_focus(w, false);
}

void keyboard_settle_focus(struct wayland_display *c)
{
    for (struct seat *s = c->seats; s; s = s->next)
        if (s->focus_leaving)
            seat_settle_focus(s);
}

static void keyboard_leave(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, struct wl_surface *surface)
{
    (void)keyboard; (void)serial; (void)surface;
    struct seat *s = data;
    struct wayland_surface *w = s->keyboard_focus;
    s->keyboard_focus = NULL;
    if (!w) return;
    keyboard_reset_keys(s, w);
    seat_settle_focus(s);
    s->focus_leaving = w;
}

static unsigned input_modifiers(struct seat *s)
{
    if (!s->input_mods_ready)
        resolve_input_mod_masks(s);

    unsigned mods = 0;
    xkb_mod_mask_t reported = 0;
    for (unsigned i = 0; i < 5; i++) {
        xkb_mod_mask_t mask = s->input_mod_masks[i];
        if (mask && (mask & ~reported) &&
            xkb_state_mod_name_is_active(s->xkb_state, input_mod_names[i],
                                         XKB_STATE_MODS_EFFECTIVE) > 0) {
            mods |= 1u << i;
            reported |= mask;
        }
    }
    return mods;
}

static uint32_t key_codepoint_at_level(struct seat *s, uint32_t key, xkb_layout_index_t layout, xkb_level_index_t level)
{
    const xkb_keysym_t *syms;
    int n = xkb_keymap_key_get_syms_by_level(s->xkb_keymap, key + XKB_EVDEV_OFFSET, layout, level, &syms);
    return n == 1 ? xkb_keysym_to_utf32(syms[0]) : 0;
}

static struct yasl_private_key_event xkb_key_event(struct seat *s, uint32_t key,
                                           enum yasl_key_action action)
{
    uint32_t code = key + XKB_EVDEV_OFFSET;
    xkb_layout_index_t layout = xkb_state_key_get_layout(s->xkb_state, code);

    struct yasl_private_key_event k = {
        .key = yasl_private_key_from_evdev(key),
        .action = action,
        .mods = input_modifiers(s),
        .codepoint = yasl_private_key_label(key_codepoint_at_level(s, key, layout, 0)),
        .shifted = yasl_private_key_label(key_codepoint_at_level(s, key, layout, 1)),
        .native_code = key,
    };
    return k;
}

static void keyboard_text(struct seat *s, struct wayland_surface *w, uint32_t key)
{
    uint32_t code = key + XKB_EVDEV_OFFSET;
    unsigned mods = input_modifiers(s);
    if ((mods & (YASL_PRIVATE_MOD_CTRL | YASL_PRIVATE_MOD_SUPER)) ||
        ((mods & YASL_PRIVATE_MOD_ALT) &&
         !xkb_state_mod_index_is_consumed(s->xkb_state, code, s->mod_alt))) {
        keyboard_text_reset(s);
        return;
    }
    if (!s->compose_attempted) {
        s->compose_attempted = true;
        const char *locale = getenv("LC_ALL");
        if (!locale || !*locale) locale = getenv("LC_CTYPE");
        if (!locale || !*locale) locale = getenv("LANG");
        if (!locale || !*locale) locale = "C.UTF-8";
        struct xkb_compose_table *table = xkb_compose_table_new_from_locale(
            s->conn->xkb_ctx, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (table) {
            s->compose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
            xkb_compose_table_unref(table);
        }
    }
    bool composed = false;
    if (s->compose) {
        xkb_compose_state_feed(s->compose, xkb_state_key_get_one_sym(s->xkb_state, code));
        switch (xkb_compose_state_get_status(s->compose)) {
        case XKB_COMPOSE_COMPOSING: return;
        case XKB_COMPOSE_CANCELLED: keyboard_text_reset(s); return;
        case XKB_COMPOSE_COMPOSED: composed = true; break;
        default: break;
        }
    }
    int len = composed ? xkb_compose_state_get_utf8(s->compose, NULL, 0) :
        xkb_state_key_get_utf8(s->xkb_state, code, NULL, 0);
    if (len > 0) {
        char local[128];
        char *text = (size_t)len < sizeof(local) ? local : malloc((size_t)len + 1);
        if (text) {
            if (composed) xkb_compose_state_get_utf8(s->compose, text, (size_t)len + 1);
            else xkb_state_key_get_utf8(s->xkb_state, code, text, (size_t)len + 1);
            /* Editing commands belong to key events, not text insertion. */
            if ((unsigned char)text[0] >= 0x20 && text[0] != 0x7f) {
                struct yasl_private_event ev = {.type = YASL_PRIVATE_EVENT_TEXT,
                    .surface = &w->base, .text = {.data = text, .len = (size_t)len}};
                input_queue_push(&w->input, &ev);
            }
            if (text != local) free(text);
        } else {
            struct yasl_private_event ev = {.type = YASL_PRIVATE_EVENT_INPUT_RESET, .surface = &w->base};
            input_queue_push(&w->input, &ev);
        }
    }
    if (composed) keyboard_text_reset(s);
}

static bool keyboard_dispatch_key(struct seat *s, struct wayland_surface *w, uint32_t key,
                                  enum yasl_key_action action)
{
    if (!s->xkb_state || !w->cbs) return false;

    struct yasl_private_key_event k = xkb_key_event(s, key, action);
    struct yasl_private_event ev = {
        .type = YASL_PRIVATE_EVENT_KEY,
        .surface = &w->base,
        .key = k,
    };
    if (!input_queue_push(&w->input, &ev)) {
        keyboard_text_reset(s);
        return false;
    }
    if (action != YASL_KEY_RELEASE) keyboard_text(s, w, key);
    return action != YASL_KEY_RELEASE;
}

static void keyboard_key(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    (void)keyboard; (void)time;
    struct seat *s = data;
    struct wayland_surface *w = s->keyboard_focus;
    if (!w) return;
    seat_note_input(s, serial);
    if (state == WL_KEYBOARD_KEY_STATE_RELEASED) {
        if (s->repeat_held && key == s->repeat_key) keyboard_repeat_stop(s);
        keyboard_dispatch_key(s, w, key, YASL_KEY_RELEASE);
        return;
    }
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED || !s->xkb_keymap) return;
    bool repeatable = xkb_keymap_key_repeats(s->xkb_keymap, key + XKB_EVDEV_OFFSET);
    if (repeatable) keyboard_repeat_stop(s);
    if (keyboard_dispatch_key(s, w, key, YASL_KEY_PRESS) && repeatable) {
        s->repeat_key = key;
        s->repeat_held = true;
        s->repeat_started_ns = yasl_private_loop_now_ns();
        keyboard_repeat_sync(s);
    }
}

static void keyboard_repeat_ready(void *data, int fd, short revents)
{
    struct seat *s = data;
    uint64_t count;
    if (!(revents & POLLIN) || read(fd, &count, sizeof(count)) != sizeof(count)) return;

    struct wayland_surface *w = s->keyboard_focus;
    if (!w) {
        keyboard_repeat_stop(s);
        return;
    }
    if (count > 32) count = 32;
    while (count-- && s->repeat_held && s->repeat_rate > 0) {
        if (!keyboard_dispatch_key(s, w, s->repeat_key, YASL_KEY_REPEAT))
            keyboard_repeat_stop(s);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
        uint32_t serial, uint32_t mods_depressed, uint32_t mods_latched,
        uint32_t mods_locked, uint32_t group)
{
    (void)keyboard; (void)serial;
    struct seat *s = data;
    if (s->xkb_state) {
        xkb_state_update_mask(s->xkb_state, mods_depressed, mods_latched,
                mods_locked, 0, 0, group);
    }
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *keyboard,
        int32_t rate, int32_t delay)
{
    (void)keyboard;
    struct seat *s = data;
    s->repeat_rate = rate > 0 ? rate : 0;
    s->repeat_delay = delay > 0 ? delay : 0;
    keyboard_repeat_sync(s);
}

const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

int keyboard_repeat_init(struct seat *s)
{
    if (s->repeat_fd >= 0) return 0;
    s->repeat_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (s->repeat_fd < 0) return -1;
    s->repeat_watch = yasl_loop_add_fd(s->conn->loop, s->repeat_fd, POLLIN,
                                       keyboard_repeat_ready, s);
    if (s->repeat_watch <= 0) {
        s->repeat_watch = 0;
        close(s->repeat_fd);
        s->repeat_fd = -1;
        return -1;
    }
    keyboard_repeat_sync(s);
    return 0;
}

int seat_modifiers(const struct seat *s)
{
    if (unlikely(!s || !s->xkb_state)) return 0;
    return ((s->mod_shift != XKB_MOD_INVALID &&
             xkb_state_mod_index_is_active(s->xkb_state, s->mod_shift,
                                           XKB_STATE_MODS_EFFECTIVE) > 0) * YASL_PRIVATE_MOD_SHIFT) |
           ((s->mod_alt != XKB_MOD_INVALID &&
             xkb_state_mod_index_is_active(s->xkb_state, s->mod_alt,
                                           XKB_STATE_MODS_EFFECTIVE) > 0) * YASL_PRIVATE_MOD_ALT) |
           ((s->mod_ctrl != XKB_MOD_INVALID &&
             xkb_state_mod_index_is_active(s->xkb_state, s->mod_ctrl,
                                           XKB_STATE_MODS_EFFECTIVE) > 0) * YASL_PRIVATE_MOD_CTRL) |
           ((s->mod_super != XKB_MOD_INVALID &&
             xkb_state_mod_index_is_active(s->xkb_state, s->mod_super,
                                           XKB_STATE_MODS_EFFECTIVE) > 0) * YASL_PRIVATE_MOD_SUPER) |
           ((xkb_state_mod_name_is_active(s->xkb_state, XKB_MOD_NAME_CAPS,
                                          XKB_STATE_MODS_EFFECTIVE) > 0) * YASL_PRIVATE_MOD_CAPS);
}

int wayland_display_modifiers(const struct wayland_display *c)
{
    return c ? seat_modifiers(c->active_seat ? c->active_seat : c->seats) : 0;
}

bool wayland_display_copy_text(struct wayland_display *c, enum yasl_private_selection sel,
                               const char *text)
{
    struct seat *s = c ? (c->active_seat ? c->active_seat : c->seats) : NULL;
    if (!text) {
        errno = EINVAL;
        return false;
    }
    if (!s || !s->keyboard_focus || !s->input_serial) {
        errno = ESTALE;
        return false;
    }
    if (!s->clipboard) {
        errno = ENOTSUP;
        return false;
    }
    errno = 0;
    bool ok = sel == YASL_PRIVATE_PRIMARY ?
        clipboard_copy_primary(s->clipboard, text, s->input_serial) :
        clipboard_copy(s->clipboard, text, s->input_serial);
    return ok;
}

bool wayland_display_request_paste(struct wayland_display *c, enum yasl_private_selection sel,
                                   struct wayland_surface *target, uint64_t request_id)
{
    struct seat *s = c ? (c->active_seat ? c->active_seat : c->seats) : NULL;
    if (!target || target->closed || target->destroyed || !request_id) {
        errno = EINVAL;
        return false;
    }
    if (!s || !s->clipboard) {
        errno = ENOTSUP;
        return false;
    }
    errno = 0;
    bool ok = clipboard_request_paste(s->clipboard, sel, target->surface, request_id);
    if (!ok) errno = ENOMEM;
    return ok;
}
