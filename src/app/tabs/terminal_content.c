#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "app/tabs/internal.h"
#include "app/tabs/terminal_content.h"
#include "font/font.h"
#include "session/session.h"
#include "terminal/drop.h"
#include "terminal/terminal.h"
#include "ui/css.h"
#include "ui/surface.h"
#include "ui/yasl/window.h"

/* The terminal content of a tab. The content pointer of the tab model is
 * the terminal itself, so the app keeps its terminal API. The adapter state
 * lives in the terminal. */

enum { THUMB_MIN_DP = 24 };

struct terminal_content_state {
    struct app_tab_model *model;
    struct window *window;
    struct app_terminal_content_spec spec;
};

static struct terminal_content_state *state_of(void *content)
{
    return terminal_content_state(content);
}

/* The terminal reports reach the tab code through the model. The tab code
 * finds the tab by the content identity. */
static void report_terminal(void *ud, struct terminal *t,
                            enum app_tab_content_report report)
{
    struct terminal_content_state *st = ud;
    app_tabs_content_report(st->model, t, report);
}

static void terminal_output(void *ud, struct terminal *t)
{
    report_terminal(ud, t, APP_TAB_CONTENT_OUTPUT);
}

static void terminal_title_changed(void *ud, struct terminal *t)
{
    report_terminal(ud, t, APP_TAB_CONTENT_TITLE);
}

static void terminal_report_exited(void *ud, struct terminal *t)
{
    report_terminal(ud, t, APP_TAB_CONTENT_EXIT);
}

void *app_terminal_content_create(struct window *window,
                                  struct app_tab_model *model, const char *cwd,
                                  struct vector2i content_size, int font_size,
                                  const struct app_terminal_content_spec *spec)
{
    if (!window || !model || !spec || !spec->config)
        return NULL;
    struct terminal_content_state *st = calloc(1, sizeof(*st));
    if (!st)
        return NULL;
    st->model = model;
    st->window = window;
    st->spec = *spec;
    struct terminal *t = terminal_create(window, &(struct terminal_spec){
        .config = spec->config,
        .theme = spec->theme,
        .theme_picker = spec->theme_picker,
        .host = (struct terminal_host){
            .output = terminal_output,
            .title_changed = terminal_title_changed,
            .exited = terminal_report_exited,
            .ud = st,
        },
        .cwd = cwd,
        .content_size = content_size,
        .font_size = font_size,
    });
    if (!t) {
        free(st);
        return NULL;
    }
    terminal_set_content_state(t, st);
    return t;
}

static void content_destroy(void *content)
{
    struct terminal *t = content;
    if (!t)
        return;
    free(terminal_content_state(t));
    terminal_set_content_state(t, NULL);
    terminal_destroy(t);
}

static void content_set_window(void *content, struct window *window)
{
    struct terminal *t = content;
    state_of(content)->window = window;
    terminal_set_window(t, window);
}

static struct ui_widget *content_widget(void *content)
{
    return terminal_widget(content);
}

static struct ui_widget *content_input(void *content)
{
    return terminal_input_widget(content);
}

static const char *content_title(const void *content)
{
    return terminal_title(content);
}

static uint32_t content_background(const void *content)
{
    return terminal_background(content);
}

static int content_font_size(const void *content)
{
    return terminal_font_size(content);
}

static bool content_exited(const void *content)
{
    return terminal_exited(content);
}

static void content_resize(void *content)
{
    terminal_commit_geometry(content);
}

static void content_restyle(void *content)
{
    terminal_restyle(content);
}

static void content_set_theme(void *content,
                              const struct wsap_vtcore_theme *theme)
{
    terminal_set_theme(content, theme);
}

static void content_focus(void *content, bool focused)
{
    terminal_focus(content, focused);
}

static void content_unmount(void *content)
{
    /* The content that leaves the UI keeps no scrollbar flash. */
    terminal_hide_scrollbar(content);
}

static void content_paste(void *content, const char *text, size_t len)
{
    terminal_paste(content, text, len);
}

static void content_drop_files(void *content, const char *data, size_t len)
{
    char *text = terminal_quote_uri_list(data, len);
    if (text) {
        terminal_paste(content, text, strlen(text));
        free(text);
    }
}

static void content_drop_hover(void *content, bool over)
{
    terminal_set_drop_hover(content, over);
}

static bool content_shortcut(void *content, input_command_id sc)
{
    terminal_handle_shortcut(content, sc);
    return sc == INPUT_COMMAND_FONT_LARGER ||
           sc == INPUT_COMMAND_FONT_SMALLER;
}

static void content_scroll_bottom(void *content)
{
    struct session *session = terminal_session(content);
    if (session && session->term)
        wsap_vtcore_scroll_to(session->term, 0);
}

static bool content_cwd(void *content, char *out, size_t size)
{
    struct session *session = terminal_session(content);
    return session && session_cwd(session, out, size);
}

/* Caps the thumbnail width to the window. It keeps the cell aspect ratio of
 * the terminal. Returns 0 when the thumbnail cannot show. */
static int fit_width(struct window *w, const struct font_instance *font,
                     const struct wsap_vtcore *term, int width, int max_height)
{
    const struct ui_surface *surface = window_ui_surface(w);
    float dpi = (!window_surface(w) && surface)
        ? ui_surface_scale(surface).dpi_scale
        : window_ui_scale(w).dpi_scale;
    int min_px = ui_px(dpi, THUMB_MIN_DP);
    if (min_px < 1)
        min_px = 1;
    int available = window_size(w).x;
    if (available <= 0 && !window_surface(w) && surface)
        available = ui_surface_viewport(surface).x;
    if (width > available)
        width = available;
    if (width < min_px || max_height < min_px)
        return 0;
    struct wsap_vtcore_vector2i cells = wsap_vtcore_size(term);
    const struct font_metrics *metrics = font_instance_metrics(font);
    if (!metrics || cells.x <= 0 || cells.y <= 0 ||
        metrics->cell_w <= 0 || metrics->cell_h <= 0)
        return 0;
    int64_t full_x = (int64_t)cells.x * metrics->cell_w;
    int64_t full_y = (int64_t)cells.y * metrics->cell_h;
    if (full_y > max_height) {
        int64_t cap_w = (int64_t)max_height * full_x / full_y;
        if (cap_w < width)
            width = (int)cap_w;
    }
    int64_t scaled_w = full_x < width ? full_x : width;
    int64_t scaled_h = (full_y * scaled_w) / full_x;
    /* Drop thumbnails smaller than min_px in either axis. Tight budgets
     * then show a title-only card instead of a sliver. */
    if (width < min_px || scaled_w < min_px || scaled_h < min_px)
        return 0;
    return width;
}

static bool content_thumbnail(void *content, struct vector2i max_size,
                              void **state, struct ui_image *image)
{
    struct terminal_content_state *st = state_of(content);
    struct window *w = st->window;
    if (!w || window_closed(w) || !st->spec.preview_capture)
        return false;
    struct session *session = terminal_session(content);
    struct font_instance *font = terminal_font(content);
    if (!session || !session->term || !font)
        return false;
    int fit_w = fit_width(w, font, session->term, max_size.x, max_size.y);
    if (fit_w <= 0)
        return true;
    return st->spec.preview_capture(st->spec.preview_ud, state, font,
                                    session->term, fit_w, image);
}

const struct app_tab_content_ops app_terminal_content_ops = {
    .destroy = content_destroy,
    .set_window = content_set_window,
    .widget = content_widget,
    .input = content_input,
    .title = content_title,
    .background = content_background,
    .font_size = content_font_size,
    .exited = content_exited,
    .resize = content_resize,
    .restyle = content_restyle,
    .set_theme = content_set_theme,
    .focus = content_focus,
    .unmount = content_unmount,
    .paste = content_paste,
    .drop_files = content_drop_files,
    .drop_hover = content_drop_hover,
    .shortcut = content_shortcut,
    .scroll_bottom = content_scroll_bottom,
    .cwd = content_cwd,
    .thumbnail = content_thumbnail,
};
