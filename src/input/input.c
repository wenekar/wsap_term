#include "util/text_limit.h"
#include "input/input.h"
#include "input/keybind.h"

#include <stdlib.h>
#include <string.h>

void input_keyboard_reset(struct input_keyboard_state *s)
{
    if (s) *s = (struct input_keyboard_state){0};
}

void input_keyboard_update(struct input_keyboard_state *s,
                           const struct input_key_event *ev)
{
    if (!s || !ev || ev->key <= INPUT_KEY_NONE ||
        (unsigned)ev->key >= INPUT_KEY_COUNT)
        return;
    if (ev->action == INPUT_RELEASE)
        s->down[ev->key >> 6] &= ~(1ull << (ev->key & 63));
    else if (ev->action == INPUT_PRESS || ev->action == INPUT_REPEAT)
        s->down[ev->key >> 6] |= 1ull << (ev->key & 63);
}

const struct input_shortcut_policy input_platform_shortcuts = {
#ifdef __APPLE__

    .accel = INPUT_MOD_SUPER,
    .accel_alt = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
    .zoom = INPUT_MOD_SUPER | INPUT_MOD_SHIFT,
    .tab_accel = INPUT_MOD_SUPER,
#else
    .accel = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
    .accel_alt = 0,
    .zoom = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
    .tab_accel = INPUT_MOD_CTRL | INPUT_MOD_SHIFT,
#endif
};

static inline unsigned queue_next(unsigned i)
{
    return (i + 1) & (INPUT_QUEUE_SIZE - 1);
}

static bool is_motion(const struct input_event *ev)
{
    return ev->kind == INPUT_MOUSE && ev->mouse.kind == PTR_MOTION;
}

static bool scrolls_merge(const struct input_event *a, const struct input_event *b)
{
    return a->kind == INPUT_MOUSE && a->mouse.kind == PTR_SCROLL &&
           b->kind == INPUT_MOUSE && b->mouse.kind == PTR_SCROLL &&
           a->mouse.mods == b->mouse.mods &&
           a->popup == b->popup &&
           pointer_wheel_is_horizontal(a->mouse.button) ==
               pointer_wheel_is_horizontal(b->mouse.button);
}

enum input_push_result input_queue_add(struct input_queue *q,
                                        const struct input_event *ev)
{
    if (!q || !ev || ev->kind == INPUT_NONE) return INPUT_PUSH_DROPPED;
    if ((ev->kind == INPUT_PASTE || ev->kind == INPUT_DROP_FILES) &&
        ((!ev->paste.data && ev->paste.len) ||
         ev->paste.len > INPUT_TEXT_MAX - q->text_bytes))
        return INPUT_PUSH_DROPPED;

    if (q->head != q->tail) {
        unsigned last = (q->tail - 1) & (INPUT_QUEUE_SIZE - 1);
        struct input_event *prev = &q->ev[last];
        if (is_motion(ev) && is_motion(prev) &&
            prev->mouse.button == ev->mouse.button &&
            prev->mouse.mods == ev->mouse.mods) {
            *prev = *ev;
            return INPUT_PUSH_OK;
        }
        if (scrolls_merge(prev, ev)) {
            prev->mouse.scroll_lines = input_scroll_clamp(
                prev->mouse.scroll_lines + ev->mouse.scroll_lines);
            prev->mouse.scroll_delta += ev->mouse.scroll_delta;
            prev->mouse.pos = ev->mouse.pos;
            prev->mouse.button = ev->mouse.button;
            return INPUT_PUSH_OK;
        }
    }

    unsigned next = queue_next(q->tail);
    bool evicted = false;
    if (next == q->head) {

        if (is_motion(&q->ev[q->head])) {
            q->head = queue_next(q->head);
            evicted = true;
        } else {
            return INPUT_PUSH_DROPPED;
        }
    }
    if (ev->kind == INPUT_PASTE || ev->kind == INPUT_DROP_FILES) q->text_bytes += ev->paste.len;
    q->ev[q->tail] = *ev;
    q->tail = queue_next(q->tail);
    return evicted ? INPUT_PUSH_EVICTED_MOTION : INPUT_PUSH_OK;
}

static size_t push_data(struct input_queue *q, enum input_kind kind,
                        const char *data, size_t len)
{
    if (!q || !data || !len || len > INPUT_TEXT_MAX - q->text_bytes)
        return 0;
    struct input_event ev = { .kind = kind };
    ev.paste.data = malloc(len);
    if (!ev.paste.data) return 0;
    memcpy(ev.paste.data, data, len);
    ev.paste.len = len;
    if (input_queue_add(q, &ev) == INPUT_PUSH_DROPPED) {
        free(ev.paste.data);
        return 0;
    }
    return len;
}

size_t input_queue_push_paste(struct input_queue *q, const char *data, size_t len)
{
    return push_data(q, INPUT_PASTE, data, len);
}

size_t input_queue_push_drop_files(struct input_queue *q, const char *uris, size_t len)
{
    return push_data(q, INPUT_DROP_FILES, uris, len);
}

bool input_queue_take(struct input_queue *q, struct input_event *out)
{
    if (!q || !out || q->head == q->tail) return false;
    *out = q->ev[q->head];
    if (out->kind == INPUT_PASTE || out->kind == INPUT_DROP_FILES) q->text_bytes -= out->paste.len;
    q->head = queue_next(q->head);

    while (q->head != q->tail) {
        const struct input_event *next = &q->ev[q->head];
        if (!scrolls_merge(out, next))
            break;
        out->mouse.scroll_lines = input_scroll_clamp(
            out->mouse.scroll_lines + next->mouse.scroll_lines);
        out->mouse.scroll_delta += next->mouse.scroll_delta;
        out->mouse.pos = next->mouse.pos;
        out->mouse.button = next->mouse.button;
        q->head = queue_next(q->head);
    }
    return true;
}

void input_queue_reset(struct input_queue *q)
{
    if (!q) return;
    struct input_event ev;
    while (input_queue_take(q, &ev))
        input_event_release(&ev);
    q->head = q->tail = 0;
}

void input_event_release(struct input_event *ev)
{
    if (!ev || (ev->kind != INPUT_PASTE && ev->kind != INPUT_DROP_FILES)) return;
    free(ev->paste.data);
    ev->paste.data = NULL;
    ev->paste.len = 0;
}

size_t input_queue_push_text(struct input_queue *q, const char *text,
                           size_t len, enum input_text_source source)
{
    if (!q || !text) return 0;
    size_t off = 0;
    while (off < len) {
        struct input_event ev = {
            .kind = INPUT_TEXT,
            .text = { .source = source },
        };
        size_t n = len - off;
        if (n > sizeof(ev.text.utf8)) {
            n = sizeof(ev.text.utf8);
            while (n && ((unsigned char)text[off + n] & 0xC0) == 0x80)
                n--;
            if (!n) break;
        }
        memcpy(ev.text.utf8, text + off, n);
        ev.text.len = (uint8_t)n;
        if (input_queue_add(q, &ev) == INPUT_PUSH_DROPPED)
            break;
        off += n;
    }
    return off;
}

enum shortcut_key_route {
    SHORTCUT_KEY_PASSTHROUGH = 0,
    SHORTCUT_KEY_CONSUMED,
    SHORTCUT_KEY_FONT_LARGER,
    SHORTCUT_KEY_FONT_SMALLER,
    SHORTCUT_KEY_SCROLL_UP,
    SHORTCUT_KEY_SCROLL_DOWN,
};

static enum shortcut_key_route shortcut_key_route(enum input_shortcut action)
{
    switch (action) {
    case INPUT_SHORTCUT_FONT_LARGER:  return SHORTCUT_KEY_FONT_LARGER;
    case INPUT_SHORTCUT_FONT_SMALLER: return SHORTCUT_KEY_FONT_SMALLER;
    case INPUT_SHORTCUT_SCROLL_UP:   return SHORTCUT_KEY_SCROLL_UP;
    case INPUT_SHORTCUT_SCROLL_DOWN: return SHORTCUT_KEY_SCROLL_DOWN;
    default:                        return SHORTCUT_KEY_CONSUMED;
    }
}

static void shortcut_set_route(struct input_shortcut_state *st, unsigned key_id,
                                enum shortcut_key_route route)
{
    unsigned shift = (key_id % 2) * 4;
    uint8_t *entry = &st->routes[key_id / 2];
    *entry = (uint8_t)((*entry & ~(15u << shift)) | ((unsigned)route << shift));
}

enum input_key_route input_shortcut_route_key(const struct input_keymap *km,
        struct input_queue *q,
        struct input_shortcut_state *st,
        const struct input_key_event *key, unsigned key_id,
        enum input_shortcut *out_sc)
{
    if (out_sc) *out_sc = INPUT_SHORTCUT_NONE;
    if (!km || !q || !st || !key || key_id >= INPUT_SHORTCUT_KEY_COUNT)
        return INPUT_KEY_PASSTHROUGH;
    if (key->action != INPUT_PRESS && key->action != INPUT_REPEAT &&
        key->action != INPUT_RELEASE)
        return INPUT_KEY_PASSTHROUGH;

    unsigned shift = (key_id % 2) * 4;
    enum shortcut_key_route route = (st->routes[key_id / 2] >> shift) & 15u;
    if (key->action == INPUT_RELEASE) {
        shortcut_set_route(st, key_id, SHORTCUT_KEY_PASSTHROUGH);
        return route ? INPUT_KEY_SWALLOWED : INPUT_KEY_PASSTHROUGH;
    }
    if (key->action == INPUT_REPEAT) {
        if (route == SHORTCUT_KEY_PASSTHROUGH)
            return INPUT_KEY_PASSTHROUGH;
        if (route == SHORTCUT_KEY_CONSUMED)
            return INPUT_KEY_SWALLOWED;
    }
    if (key->action == INPUT_PRESS)
        shortcut_set_route(st, key_id, SHORTCUT_KEY_PASSTHROUGH);

    enum input_shortcut sc =
        input_keymap_lookup(km, key);
    if (sc == INPUT_SHORTCUT_NONE)
        return key->action == INPUT_REPEAT
            ? INPUT_KEY_SWALLOWED : INPUT_KEY_PASSTHROUGH;

    enum shortcut_key_route matched = shortcut_key_route(sc);
    if (key->action == INPUT_REPEAT && route != matched)
        return INPUT_KEY_SWALLOWED;
    shortcut_set_route(st, key_id, matched);

    if (input_queue_add(q, &(struct input_event){
            .kind = INPUT_SHORTCUT,
            .shortcut = { .action = sc },
        }) == INPUT_PUSH_DROPPED) {
        shortcut_set_route(st, key_id, SHORTCUT_KEY_CONSUMED);
        return INPUT_KEY_DROPPED;
    }
    if (out_sc) *out_sc = sc;
    return INPUT_KEY_SHORTCUT;
}
