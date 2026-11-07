#include "internal.h"
#include "screen.h"
#include "unicode/unicode.h"
#include <stdio.h>

_Thread_local char wsap_vtcore_thread_anchor;

const char *wsap_vtcore_unicode_version(void)
{
    return WSAP_VTCORE_UNICODE_DATA_VERSION;
}

bool wsap_vtcore_unicode_is_emoji_presentation(uint32_t cp)
{
    return wsap_vtcore_unicode_emoji_presentation(cp);
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
        .alternate_scroll = t->alternate_scroll,
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

static unsigned term_luma(uint32_t argb)
{
    return 299u * ((argb >> 16) & 0xff) + 587u * ((argb >> 8) & 0xff) + 114u * (argb & 0xff);
}

/* The host scheme, or else light when the theme background is brighter
 * than the theme foreground. */
static enum wsap_vtcore_color_scheme term_color_scheme(const struct wsap_vtcore *t)
{
    if (t->color_scheme)
        return (enum wsap_vtcore_color_scheme)t->color_scheme;
    return term_luma(t->palette.theme.bg) > term_luma(t->palette.theme.fg)
        ? WSAP_VTCORE_COLOR_SCHEME_LIGHT : WSAP_VTCORE_COLOR_SCHEME_DARK;
}

void wsap_vtcore_report_color_scheme(struct wsap_vtcore *t)
{
    send_response(t, term_color_scheme(t) == WSAP_VTCORE_COLOR_SCHEME_LIGHT
                     ? "\033[?997;2n" : "\033[?997;1n");
}

void wsap_vtcore_report_size(struct wsap_vtcore *t)
{
    char reply[96];
    snprintf(reply, sizeof(reply), "\033[48;%d;%d;%lld;%lldt", t->rows, t->cols,
             (long long)t->rows * t->cell_pixels.y, (long long)t->cols * t->cell_pixels.x);
    send_response(t, reply);
}

void wsap_vtcore_set_cell_pixels(struct wsap_vtcore *t, struct wsap_vtcore_vector2i size)
{
    if (!t)
        return;
    if (size.x <= 0 || size.y <= 0)
        size = (struct wsap_vtcore_vector2i){ 0 };
    wsap_vtcore_write_begin(t);
    bool changed = size.x != t->cell_pixels.x || size.y != t->cell_pixels.y;
    t->cell_pixels = size;
    if (changed && t->resize_reports)
        wsap_vtcore_report_size(t);
    wsap_vtcore_write_end(t);
}

void wsap_vtcore_set_color_scheme(struct wsap_vtcore *t, enum wsap_vtcore_color_scheme scheme)
{
    if (!t || (unsigned)scheme > WSAP_VTCORE_COLOR_SCHEME_LIGHT)
        return;
    wsap_vtcore_write_begin(t);
    enum wsap_vtcore_color_scheme old = term_color_scheme(t);
    t->color_scheme = (uint8_t)scheme;
    if (t->color_scheme_reports && term_color_scheme(t) != old)
        wsap_vtcore_report_color_scheme(t);
    wsap_vtcore_write_end(t);
}

void wsap_vtcore_set_theme(struct wsap_vtcore *t, const struct wsap_vtcore_theme *theme)
{
    if (!t)
        return;
    wsap_vtcore_write_begin(t);
    enum wsap_vtcore_color_scheme old = term_color_scheme(t);
    struct palette next = { .version = t->palette.version };
    wsap_vtcore_palette_init(&next, theme);
    if (memcmp(&next.theme, &t->palette.theme, sizeof(next.theme)))
        t->palette = next;
    if (t->color_scheme_reports && term_color_scheme(t) != old)
        wsap_vtcore_report_color_scheme(t);
    wsap_vtcore_write_end(t);
}

bool wsap_vtcore_set_scrollback_limit(struct wsap_vtcore *t, int lines)
{
    if (!t || lines < 0 || lines > WSAP_VTCORE_SCROLLBACK_MAX)
        return false;
    wsap_vtcore_write_begin(t);
    if (t->sb_capacity == lines) {
        wsap_vtcore_write_end(t);
        return true;
    }
    struct hist *h = &t->hist;
    size_t bytes = (size_t)lines * 800;
    if (bytes < 8u * 1024 * 1024) bytes = 8u * 1024 * 1024;
    uint32_t pages = (uint32_t)(bytes / HIST_PAGE_SIZE);
    if (pages > 65535) pages = 65535;
    if (lines > 0 && pages > h->max_pages) {
        uint8_t **data = calloc(pages, sizeof(*data));
        uint32_t *used = calloc(pages, sizeof(*used));
        uint32_t *parts = calloc(pages, sizeof(*parts));
        uint16_t *free_list = malloc(pages * sizeof(*free_list));
        if (!data || !used || !parts || !free_list) {
            free(data); free(used); free(parts); free(free_list);
            wsap_vtcore_write_end(t);
            return false;
        }
        if (h->npages) {
            memcpy(data, h->pages, h->npages * sizeof(*data));
            memcpy(used, h->page_used, h->npages * sizeof(*used));
            memcpy(parts, h->page_parts, h->npages * sizeof(*parts));
        }
        if (h->nfree) memcpy(free_list, h->free_pages, h->nfree * sizeof(*free_list));
        free(h->pages); free(h->page_used); free(h->page_parts); free(h->free_pages);
        h->pages = data;
        h->page_used = used;
        h->page_parts = parts;
        h->free_pages = free_list;
        h->max_pages = pages;
    }
    h->line_limit = (uint32_t)lines;
    evict_for(h, 0);
    t->sb_capacity = lines;
    if (!lines || (t->view.anchor.active && t->view.anchor.line_id < hist_first_id(h)))
        t->view.anchor = (struct view_anchor){0};
    t->view.index_cols = 0;
    wsap_vtcore_select_clear(t);
    t->full_gen++;
    wsap_vtcore_write_end(t);
    return true;
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
