#include "terminal/internal.h"

#define BLINK_HALF_MS 500

static bool blinking(const struct window *w)
{
    struct terminal *tw = terminal_of(w);
    if (!tw || !tw->focused)
        return false;
    struct wsap_vtcore *t = terminal_term(w);
    if (!window_surface(w) || window_closed(w) || window_suspended(w) || !t)
        return false;
    struct wsap_vtcore_cursor cursor = wsap_vtcore_cursor(t);
    return cursor.visible && cursor.blink;
}

/* The cursor flips every half period, counted from the last keystroke in
 * the active terminal; the draw path computes the same phase. */
uint64_t terminal_next_blink_ms(struct window *w, uint64_t now_ms)
{
    if (!blinking(w))
        return 0;
    uint64_t last_key = session_last_keystroke_ms(&terminal_active_tab(w)->session);
    uint64_t elapsed = last_key > 0 && now_ms >= last_key
        ? now_ms - last_key : now_ms;
    return now_ms + (BLINK_HALF_MS - elapsed % BLINK_HALF_MS);
}
