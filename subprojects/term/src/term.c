#include "internal.h"
#include "screen.h"

_Thread_local char wsap_vtcore_thread_anchor;

const char *wsap_vtcore_unicode_version(void)
{
    return WSAP_VTCORE_UNICODE_DATA_VERSION;
}

unsigned wsap_vtcore_version(void)
{
    return WSAP_VTCORE_VERSION_CODE;
}

void wsap_vtcore_lock(struct wsap_vtcore *t)
{
    if (!t)
        return;
    uint64_t self = (uint64_t)(uintptr_t)&wsap_vtcore_thread_anchor;
    if (atomic_load_explicit(&t->writer_tid, memory_order_relaxed) == self) {
        t->write_depth++;
        return;
    }
    atomic_fetch_add_explicit(&t->waiters, 1, memory_order_acq_rel);
    wsap_vtcore_mutex_lock(&t->write_lock);
    atomic_store_explicit(&t->writer_tid, self, memory_order_relaxed);
    t->write_depth = 1;
}

void wsap_vtcore_unlock(struct wsap_vtcore *t)
{
    if (!t)
        return;
    uint64_t self = (uint64_t)(uintptr_t)&wsap_vtcore_thread_anchor;
    if (atomic_load_explicit(&t->writer_tid, memory_order_relaxed) != self)
        return;
    if (--t->write_depth > 0)
        return;
    atomic_store_explicit(&t->writer_tid, 0, memory_order_relaxed);
    unsigned int prev = atomic_load_explicit(&t->waiters, memory_order_relaxed);
    bool wake_yield = false;
    while (prev > 0) {
        if (atomic_compare_exchange_weak_explicit(&t->waiters, &prev, prev - 1,
                                                  memory_order_seq_cst,
                                                  memory_order_acquire)) {
            wake_yield = (prev == 1);
            break;
        }
    }
    wsap_vtcore_mutex_unlock(&t->write_lock);
    if (wake_yield && atomic_load_explicit(&t->yielding, memory_order_seq_cst) > 0) {
        wsap_vtcore_mutex_lock(&t->yield_lock);
        wsap_vtcore_cond_broadcast(&t->yield_cond);
        wsap_vtcore_mutex_unlock(&t->yield_lock);
    }
}

bool wsap_vtcore_lock_contended(const struct wsap_vtcore *t)
{
    if (!t)
        return false;
    return atomic_load_explicit(&t->waiters, memory_order_acquire) != 0;
}

void wsap_vtcore_yield_to_waiters(struct wsap_vtcore *t)
{
    if (!t || atomic_load_explicit(&t->waiters, memory_order_acquire) == 0)
        return;
    wsap_vtcore_mutex_lock(&t->yield_lock);
    atomic_fetch_add_explicit(&t->yielding, 1, memory_order_seq_cst);
    while (atomic_load_explicit(&t->waiters, memory_order_acquire) > 0)
        wsap_vtcore_cond_wait(&t->yield_cond, &t->yield_lock);
    atomic_fetch_sub_explicit(&t->yielding, 1, memory_order_relaxed);
    wsap_vtcore_mutex_unlock(&t->yield_lock);
}

struct wsap_vtcore_vector2i wsap_vtcore_size(const struct wsap_vtcore *t)
{
    if (!t)
        return (struct wsap_vtcore_vector2i){ 0 };
    wsap_vtcore_lock((struct wsap_vtcore *)t);
    struct wsap_vtcore_vector2i size = { .x = t->cols, .y = t->rows };
    wsap_vtcore_unlock((struct wsap_vtcore *)t);
    return size;
}

struct wsap_vtcore_modes wsap_vtcore_modes(const struct wsap_vtcore *t)
{
    if (!t)
        return (struct wsap_vtcore_modes){ 0 };
    wsap_vtcore_lock((struct wsap_vtcore *)t);
    struct wsap_vtcore_modes m = {
        .keyboard = t->keyboard[t->alternate].flags,
        .mouse = (enum wsap_vtcore_mouse)t->mouse_mode,
        .mouse_encoding = (enum wsap_vtcore_mouse_encoding)t->mouse_encoding,
        .grid = { .x = t->cols, .y = t->rows },
        .app_cursor = t->application_cursor,
        .app_keypad = t->application_keypad,
        .backarrow_bs = t->backarrow_sends_bs,
        .bracketed_paste = t->bracketed_paste,
        .focus_events = t->focus_events,
        .alternate = t->alternate,
        .reverse_video = t->reverse_video,
        .synchronized = t->synchronized_output,
    };
    wsap_vtcore_unlock((struct wsap_vtcore *)t);
    return m;
}

struct wsap_vtcore_cursor wsap_vtcore_cursor(const struct wsap_vtcore *t)
{
    if (!t)
        return (struct wsap_vtcore_cursor){ .visible = true };
    wsap_vtcore_lock((struct wsap_vtcore *)t);
    struct wsap_vtcore_cursor c = {
        .pos = { .x = t->cursor_x, .y = t->cursor_y },
        .style = t->cursor_style,
        .visible = t->cursor_visible,
        .blink = t->cursor_blink,
    };
    wsap_vtcore_unlock((struct wsap_vtcore *)t);
    return c;
}

void wsap_vtcore_set_cursor_default(struct wsap_vtcore *t, enum wsap_vtcore_cursor_style style, bool steady)
{
    if (!t)
        return;
    wsap_vtcore_lock(t);
    if (style > WSAP_VTCORE_CURSOR_BAR)
        style = WSAP_VTCORE_CURSOR_BLOCK;
    if (t->cursor_style != style || t->cursor_blink == steady) {
        struct srow *cur = wsap_vtcore_row(t, t->cursor_y);
        if (cur) cur->version++;
    }
    t->default_cursor_style = t->cursor_style = style;
    t->default_cursor_blink = t->cursor_blink = !steady;
    wsap_vtcore_unlock(t);
}

void wsap_vtcore_set_theme(struct wsap_vtcore *t, const struct wsap_vtcore_theme *theme)
{
    if (!t)
        return;
    wsap_vtcore_lock(t);
    struct palette next = { .version = t->palette.version };
    wsap_vtcore_palette_init(&next, theme);
    if (memcmp(&next.theme, &t->palette.theme, sizeof(next.theme)))
        t->palette = next;
    wsap_vtcore_unlock(t);
}

void wsap_vtcore_focus(struct wsap_vtcore *t, bool focused)
{
    if (!t)
        return;
    wsap_vtcore_lock(t);
    if (t->focus_events)
        send_response(t, focused ? "\033[I" : "\033[O");
    wsap_vtcore_unlock(t);
}

char *wsap_vtcore_cwd(const struct wsap_vtcore *t)
{
    if (!t) return NULL;
    wsap_vtcore_lock((struct wsap_vtcore *)t);
    char *text = t->cwd ? wsap_vtcore_strdup(t->cwd) : NULL;
    wsap_vtcore_unlock((struct wsap_vtcore *)t);
    return text;
}

char *wsap_vtcore_pointer_shape(const struct wsap_vtcore *t)
{
    if (!t) return NULL;
    wsap_vtcore_lock((struct wsap_vtcore *)t);
    char *text = t->pointer_shape ? wsap_vtcore_strdup(t->pointer_shape) : NULL;
    wsap_vtcore_unlock((struct wsap_vtcore *)t);
    return text;
}

void wsap_vtcore_text_free(char *text)
{
    free(text);
}
