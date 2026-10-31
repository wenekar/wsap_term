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
