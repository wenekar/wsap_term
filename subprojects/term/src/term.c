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
    wsap_vtcore_write_begin(t);
}

void wsap_vtcore_unlock(struct wsap_vtcore *t)
{
    wsap_vtcore_write_end(t);
}

struct wsap_vtcore_vector2i wsap_vtcore_size(const struct wsap_vtcore *t)
{
    if (!t)
        return (struct wsap_vtcore_vector2i){ 0 };
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    struct wsap_vtcore_vector2i size = { .x = t->cols, .y = t->rows };
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return size;
}

struct wsap_vtcore_modes wsap_vtcore_modes(const struct wsap_vtcore *t)
{
    if (!t)
        return (struct wsap_vtcore_modes){ 0 };
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
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
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return m;
}

struct wsap_vtcore_cursor wsap_vtcore_cursor(const struct wsap_vtcore *t)
{
    if (!t)
        return (struct wsap_vtcore_cursor){ .visible = true };
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    struct wsap_vtcore_cursor c = {
        .pos = { .x = t->cursor_x, .y = t->cursor_y },
        .style = t->cursor_style,
        .visible = t->cursor_visible,
        .blink = t->cursor_blink,
    };
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return c;
}

void wsap_vtcore_set_cursor_default(struct wsap_vtcore *t, enum wsap_vtcore_cursor_style style, bool steady)
{
    if (!t)
        return;
    wsap_vtcore_write_begin(t);
    if (style > WSAP_VTCORE_CURSOR_BAR)
        style = WSAP_VTCORE_CURSOR_BLOCK;
    if (t->cursor_style != style || t->cursor_blink == steady) {
        struct srow *cur = wsap_vtcore_row(t, t->cursor_y);
        if (cur) cur->version++;
    }
    t->default_cursor_style = t->cursor_style = style;
    t->default_cursor_blink = t->cursor_blink = !steady;
    wsap_vtcore_write_end(t);
}

void wsap_vtcore_set_theme(struct wsap_vtcore *t, const struct wsap_vtcore_theme *theme)
{
    if (!t)
        return;
    wsap_vtcore_write_begin(t);
    struct palette next = { .version = t->palette.version };
    wsap_vtcore_palette_init(&next, theme);
    if (memcmp(&next.theme, &t->palette.theme, sizeof(next.theme)))
        t->palette = next;
    wsap_vtcore_write_end(t);
}

void wsap_vtcore_focus(struct wsap_vtcore *t, bool focused)
{
    if (!t)
        return;
    wsap_vtcore_write_begin(t);
    if (t->focus_events)
        send_response(t, focused ? "\033[I" : "\033[O");
    wsap_vtcore_write_end(t);
}

char *wsap_vtcore_cwd(const struct wsap_vtcore *t)
{
    if (!t) return NULL;
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    char *text = t->cwd ? wsap_vtcore_strdup(t->cwd) : NULL;
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return text;
}

char *wsap_vtcore_pointer_shape(const struct wsap_vtcore *t)
{
    if (!t) return NULL;
    wsap_vtcore_write_begin((struct wsap_vtcore *)t);
    char *text = t->pointer_shape ? wsap_vtcore_strdup(t->pointer_shape) : NULL;
    wsap_vtcore_write_end((struct wsap_vtcore *)t);
    return text;
}

void wsap_vtcore_text_free(char *text)
{
    free(text);
}
