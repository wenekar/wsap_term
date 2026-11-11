#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>

#include "terminal/internal.h"
#include "platform/platform.h"
#include "util/time.h"

static const char *content_title(const struct terminal *t)
{
    return t ? session_title(&t->session) : NULL;
}

static void report_output(struct terminal *t)
{
    if (t->host.output)
        t->host.output(t->host.ud, t);
    else if (t->window)
        window_redraw(t->window);
}

static void report_title(struct terminal *t)
{
    if (t->host.title_changed)
        t->host.title_changed(t->host.ud, t);
}

static void session_output(void *ud)
{
    report_output(ud);
}

static void session_title_changed(void *ud)
{
    report_title(ud);
}

static void session_clipboard(void *ud, const char *text)
{
    struct terminal *t = ud;
    if (t->window && window_surface(t->window))
        window_copy(t->window, YASL_CLIPBOARD, text);
}

static void session_exited(void *ud)
{
    struct terminal *t = ud;
    t->exited = true;
    if (t->host.exited)
        t->host.exited(t->host.ud, t);
}

static void session_bell(void *ud)
{
    struct terminal *t = ud;
    if (!t->config->visual_bell)
        return;
    t->view.visual_bell_until_ms = yasl_time_ns() / 1000000 + 100;
    if (t->window)
        window_redraw(t->window);
}

static const struct session_host SESSION_HOST = {
    .output = session_output,
    .title = session_title_changed,
    .clipboard = session_clipboard,
    .exited = session_exited,
    .bell = session_bell,
};

/* The cell grid of a content box. The content widget is the one owner of
 * these dimensions. */
static struct vector2i content_grid(const struct font_metrics *metrics,
                                    struct vector2i px)
{
    struct vector2i grid = {
        .x = px.x > 0 && metrics->cell_w > 0 ? px.x / metrics->cell_w : 0,
        .y = px.y > 0 && metrics->cell_h > 0 ? px.y / metrics->cell_h : 0,
    };
    if (grid.x > WSAP_VTCORE_MAX_COLS)
        grid.x = WSAP_VTCORE_MAX_COLS;
    return vector2i_max(grid, vector2i_splat(1));
}

/* The core needs the cell size for CSI 14 t and CSI 16 t. The font metrics
 * give it. An unchanged size sends no report. */
static void sync_cell_pixels(struct terminal *t)
{
    if (!t->session.term)
        return;
    const struct font_metrics *m = terminal_font_metrics(t);
    wsap_vtcore_set_cell_pixels(t->session.term,
        (struct wsap_vtcore_vector2i){ .x = m->cell_w, .y = m->cell_h });
}

/* The unit of font_instance_acquire(). 120 is the 1x scale. */
#define TERMINAL_SCALE_1X 120

static uint32_t content_scale(const struct terminal *t)
{
    if (t->window)
        return window_scale_120(t->window);
    return t->font ? font_instance_scale_120(t->font) : TERMINAL_SCALE_1X;
}

static bool fit_font(struct terminal *t, int size_pt)
{
    uint32_t scale = content_scale(t);
    if (t->font && font_instance_size_pt(t->font) == size_pt &&
        font_instance_scale_120(t->font) == scale) {
        sync_cell_pixels(t);
        return true;
    }
    if (size_pt < 1)
        return false;
    struct font_instance *fi = font_instance_acquire(size_pt, scale);
    if (!fi)
        return false;
    font_instance_unref(t->font);
    t->font = fi;
    t->font_size = font_instance_size_pt(fi);
    sync_cell_pixels(t);
    return true;
}

/* The host preference. Zero means no preference, and the core then
 * compares the theme colors. */
static enum wsap_vtcore_color_scheme host_color_scheme(void)
{
    unsigned appearance = yasl_get_appearance();
    if (appearance & YASL_APPEARANCE_DARK)
        return WSAP_VTCORE_COLOR_SCHEME_DARK;
    if (appearance & YASL_APPEARANCE_LIGHT)
        return WSAP_VTCORE_COLOR_SCHEME_LIGHT;
    return WSAP_VTCORE_COLOR_SCHEME_UNKNOWN;
}

static struct vector2i default_content_size(const struct window *w)
{
    return window_default_size(window_host(w)->config);
}

_Static_assert(alignof(struct terminal) <= 64,
               "the content allocation must hold the session alignment");

struct terminal *terminal_create(struct window *window,
                                 const struct terminal_spec *spec)
{
    if (!spec || !spec->config || !window)
        return NULL;
    struct terminal *t = aligned_alloc(64, (sizeof(*t) + 63) & ~(size_t)63);
    if (!t)
        return NULL;
    memset(t, 0, sizeof(*t));
    const struct window_config *wcfg =
        window_host(window) ? window_host(window)->config : NULL;
    t->window = window;
    t->loop = window_host(window)->loop;
    t->config = spec->config;
    t->host = spec->host;
    t->theme_picker = spec->theme_picker;
    if (spec->theme)
        t->theme = *spec->theme;
    /* The app has no appearance-change event. Read the host preference
     * once. Each theme change applies this value. */
    t->host_scheme = host_color_scheme();
    t->snapshot = wsap_vtcore_snapshot_new();
    if (!t->snapshot)
        goto fail;
    t->applied_font_size = wcfg ? wcfg->font_size : 0;
    t->applied_cursor_style = spec->config->cursor_style;
    t->applied_cursor_blink = spec->config->cursor_blink;
    t->focused = true;
    if (wcfg && wcfg->font_family && !(t->applied_font_family = strdup(wcfg->font_family)))
        goto fail;
    if (t->applied_font_size < 1)
        t->applied_font_size = spec->font_size > 0 ? spec->font_size : 12;
    if (!fit_font(t, spec->font_size > 0 ? spec->font_size : t->applied_font_size))
        goto fail;

    char default_loc[128];
    const char *locale = spec->config->locale;
    if (!locale || !locale[0] || strcmp(locale, "auto") == 0) {
        platform_locale_default(default_loc, sizeof(default_loc));
        locale = default_loc;
    }
    struct vector2i px = spec->content_size;
    if (px.x <= 0 || px.y <= 0)
        px = default_content_size(window);
    struct vector2i grid = content_grid(terminal_font_metrics(t), px);
    const struct session_spec session_spec = {
        .command = spec->config->command,
        .cwd = spec->cwd,
        .locale = locale,
        .grid = grid,
        .scrollback = spec->config->scrollback,
        .theme = t->theme,
        .color_scheme = t->host_scheme,
        .cursor_style = spec->config->cursor_style,
        .cursor_blink = spec->config->cursor_blink,
    };
    if (!session_init(&t->session, window_host(window)->loop, &session_spec))
        goto fail;
    t->grid = grid;
    session_set_host(&t->session, &SESSION_HOST, t);
    terminal_view_init(t);
    terminal_view_attach(t);
    return t;

fail:
    if (t->session.term)
        session_fini(&t->session);
    wsap_vtcore_snapshot_free(t->snapshot);
    font_instance_unref(t->font);
    free(t->applied_font_family);
    free(t);
    return NULL;
}

void terminal_destroy(struct terminal *t)
{
    if (!t)
        return;
    if (t->view.accessibility_timer && t->loop)
        yasl_loop_del_timer(t->loop, t->view.accessibility_timer);
    ui_widget_destroy(&t->scrollview.widget);
    session_fini(&t->session);
    font_instance_unref(t->font);
    free(t->menu_theme_items);
    free(t->view.accessibility_text);
    wsap_vtcore_snapshot_free(t->snapshot);
    free(t->applied_font_family);
    free(t);
}

void terminal_set_window(struct terminal *t, struct window *window)
{
    if (!t)
        return;
    struct window *old = t->window;
    t->window = window;
    /* The caret position belongs to the old window. */
    t->ime_caret = (struct rect2i){0};
    /* A detached content leaves no text input target behind. */
    if (!window && old && window_surface(old))
        yasl_surface_set_text_input(window_surface(old),
                                    &(struct yasl_text_input){0});
    /* An accessibility timer belongs to the loop of the old window. */
    if (t->view.accessibility_timer && t->loop) {
        yasl_loop_del_timer(t->loop, t->view.accessibility_timer);
        t->view.accessibility_timer = 0;
        t->view.accessibility_text_valid = false;
    }
    if (!window)
        return;
    t->loop = window_host(window)->loop;
    /* Apply the font settings and the font scale of the destination. */
    terminal_restyle(t);
    fit_font(t, t->font_size);
    sync_cell_pixels(t);
    if (window_surface(window))
        window_redraw(window);
}

struct ui_widget *terminal_widget(struct terminal *t)
{
    return t ? &t->scrollview.widget : NULL;
}

struct ui_widget *terminal_input_widget(struct terminal *t)
{
    return t ? &t->view.widget : NULL;
}

struct session *terminal_session(struct terminal *t)
{
    return t ? &t->session : NULL;
}

struct font_instance *terminal_font(const struct terminal *t)
{
    return t ? t->font : NULL;
}

const char *terminal_title(const struct terminal *t)
{
    return content_title(t);
}

uint32_t terminal_background(const struct terminal *t)
{
    if (!t)
        return 0;
    const struct wsap_vtcore_view *view = wsap_vtcore_snapshot_view(t->snapshot);
    return view->size.x ? view->palette[WSAP_VTCORE_PALETTE_BG] : t->theme.bg;
}

int terminal_font_size(const struct terminal *t)
{
    return t ? t->font_size : 0;
}

bool terminal_exited(const struct terminal *t)
{
    return t && t->exited;
}

void terminal_commit_geometry(struct terminal *t)
{
    if (!t || !t->view.widget.parent || !t->session.term)
        return;
    struct vector2i box = t->view.widget.bounds.size;
    if (box.x <= 0 || box.y <= 0)
        return;
    struct vector2i grid = content_grid(terminal_font_metrics(t), box);
    if (vector2i_eq(grid, t->grid))
        return;
    t->grid = grid;
    session_resize(&t->session, grid);
}

void terminal_set_theme(struct terminal *t,
                        const struct wsap_vtcore_theme *theme)
{
    if (!t || !theme)
        return;
    t->theme = *theme;
    if (!t->session.term)
        return;
    /* Set the host preference first. The theme change can then report the
     * final scheme once. */
    wsap_vtcore_set_color_scheme(t->session.term, t->host_scheme);
    wsap_vtcore_set_theme(t->session.term, &t->theme);
    if (t->window)
        window_redraw(t->window);
}

static bool same_text(const char *a, const char *b)
{
    if (!a || !b)
        return a == b;
    return strcmp(a, b) == 0;
}

void terminal_restyle(struct terminal *t)
{
    if (!t || !t->window)
        return;
    const struct window_config *wcfg = window_host(t->window)->config;
    const struct terminal_config *cfg = t->config;
    bool family_changed = !same_text(t->applied_font_family, wcfg->font_family);
    bool size_changed = t->applied_font_size != wcfg->font_size;
    bool font_changed = family_changed || size_changed;
    bool cursor_changed = t->applied_cursor_style != cfg->cursor_style ||
        t->applied_cursor_blink != cfg->cursor_blink;

    int size = t->font_size +
        (size_changed ? wcfg->font_size - t->applied_font_size : 0);
    if (family_changed) {
        font_instance_unref(t->font);
        t->font = NULL;
    }
    fit_font(t, size);
    if (cursor_changed)
        wsap_vtcore_set_cursor_default(t->session.term, cfg->cursor_style,
                                       !cfg->cursor_blink);
    if (family_changed) {
        char *family = wcfg->font_family ? strdup(wcfg->font_family) : NULL;
        if (family || !wcfg->font_family) {
            free(t->applied_font_family);
            t->applied_font_family = family;
        }
    }
    t->applied_font_size = wcfg->font_size;
    t->applied_cursor_style = cfg->cursor_style;
    t->applied_cursor_blink = cfg->cursor_blink;
    if (font_changed)
        window_repaint_all(t->window);
    window_redraw(t->window);
}

void terminal_adjust_font_size(struct terminal *t, int delta_pt)
{
    if (!t || !fit_font(t, t->font_size + delta_pt))
        return;
    if (t->window)
        window_repaint_all(t->window);
    if (t->window)
        window_redraw(t->window);
}

void terminal_prepare(struct terminal *t, uint64_t now_ms)
{
    (void)now_ms;
    if (!t)
        return;
    t->frame_serial++;
    struct wsap_vtcore *term = t->session.term;
    if (!term || !t->window || window_closed(t->window))
        return;
    if (!wsap_vtcore_snapshot_update(t->snapshot, term))
        return;
    t->snapshot_frame = t->frame_serial;
    t->snapshot_term = term;
    bool blink_before = t->view.blink_cells;
    terminal_scan_blink(&t->view, t->snapshot);
    /* The UI tick runs before this capture. A new blink cell asks for one
     * more frame, and the next tick schedules the blink flip. */
    if (!blink_before && t->view.blink_cells)
        window_redraw(t->window);
    terminal_update_ime(t, term);
}

bool terminal_frame_source(struct terminal *t, uint64_t now_ms,
                           struct terminal_frame_source *out)
{
    if (!t || !out || !t->session.term)
        return false;
    struct wsap_vtcore *term = t->session.term;
    struct wsap_vtcore_snapshot *snap = t->snapshot;
    /* terminal_prepare() already updated this snapshot in the current
     * frame. Use that update. Do not take the write lock and diff the row
     * keys a second time. A frame without terminal_prepare() updates here. */
    if (t->snapshot_frame == t->frame_serial && t->snapshot_term == term) {
        t->snapshot_frame = 0;
    } else if (!wsap_vtcore_snapshot_update(snap, term)) {
        return false;
    }
    *out = (struct terminal_frame_source){
        .snapshot = snap,
        .font = t->font,
        .rect = terminal_view_rect(t),
        .cursor_visible = terminal_cursor_visible(t, now_ms),
        .text_blink_on = terminal_text_blink_on(t, now_ms),
    };
    return true;
}

void terminal_focus(struct terminal *t, bool focused)
{
    if (!t)
        return;
    t->focused = focused;
    if (t->session.term)
        wsap_vtcore_focus(t->session.term, focused);
    if (t->window)
        window_redraw(t->window);
}

void terminal_hide_scrollbar(struct terminal *t)
{
    if (t)
        ui_scrollview_hide(&t->scrollview);
}

bool terminal_scrollbar_visible(const struct terminal *t)
{
    return t && ui_scrollview_is_shown(&t->scrollview);
}

void terminal_set_drop_hover(struct terminal *t, bool over)
{
    if (t)
        terminal_view_drop_hover(t, over, NULL);
}

void terminal_paste(struct terminal *t, const char *text, size_t len)
{
    if (!t || !session_paste(&t->session, text, len))
        return;
    wsap_vtcore_select_clear(t->session.term);
    wsap_vtcore_scroll_to(t->session.term, 0);
    if (t->window)
        window_redraw(t->window);
}
