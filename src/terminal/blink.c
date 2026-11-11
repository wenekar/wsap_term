#include "terminal/internal.h"

#define BLINK_HALF_MS 500

static bool blinking(const struct terminal *t)
{
    if (!t || !t->focused || !t->session.term)
        return false;
    struct window *w = t->window;
    if (!w || !window_surface(w) || window_closed(w) || window_suspended(w))
        return false;
    struct wsap_vtcore_cursor cursor = wsap_vtcore_cursor(t->session.term);
    return cursor.visible && cursor.blink;
}

uint64_t terminal_next_blink_ms(struct terminal *t, uint64_t now_ms)
{
    if (!blinking(t))
        return 0;
    uint64_t last_key = session_last_keystroke_ms(&t->session);
    uint64_t elapsed = last_key > 0 && now_ms >= last_key
        ? now_ms - last_key : now_ms;
    return now_ms + (BLINK_HALF_MS - elapsed % BLINK_HALF_MS);
}

/* Text blink uses the app clock. A late frame keeps the same phase. */
bool terminal_text_blink_on(const struct terminal *t, uint64_t now_ms)
{
    if (!t || !t->view.blink_cells)
        return true;
    return (now_ms / BLINK_HALF_MS) % 2 == 0;
}

uint64_t terminal_next_text_blink_ms(struct terminal *t, uint64_t now_ms)
{
    struct window *w = t ? t->window : NULL;
    if (!t || !t->view.blink_cells || !w || !window_surface(w) ||
        window_closed(w) || window_suspended(w))
        return 0;
    return now_ms + (BLINK_HALF_MS - now_ms % BLINK_HALF_MS);
}
