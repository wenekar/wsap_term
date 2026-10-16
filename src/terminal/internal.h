#ifndef TERMINAL_INTERNAL_H
#define TERMINAL_INTERNAL_H

#include <stdalign.h>

#include "yasl/yasl.h"
#include "font/font.h"
#include "input/input.h"
#include "input/keybind.h"
#include "terminal/selection.h"
#include <wsap_vtcore/wsap_vtcore.h>
#include "ui/yasl/client.h"
#include "session/session.h"
#include "terminal/terminal.h"
#include "ui/damage.h"
#include "ui/surface.h"
#include "ui/widget_impl.h"
#include "ui/widgets/menu.h"
#include "ui/widgets/palette.h"
#include "ui/widgets/scrollview.h"
#include "ui/widgets/tab_preview.h"
#include "ui/widgets/tabbar.h"
#include "ui/yasl/window.h"

static inline struct wsap_vtcore_vector2i term_vec_from_geom(struct vector2i v)
{
    return (struct wsap_vtcore_vector2i){ .x = v.x, .y = v.y };
}

static inline struct vector2i geom_vec_from_term(struct wsap_vtcore_vector2i v)
{
    return (struct vector2i){ .x = v.x, .y = v.y };
}

/* A terminal tab: a session and how its window shows it. It moves
 * between windows whole. */
struct tab {
    alignas(64) struct session session;
    /* The window showing the tab; NULL between windows. */
    struct window *window;
    /* The tab's own font size, at its window's scale. */
    struct font_instance *font;
    int font_size;
    struct text_selection selection;
    struct vector2i pending_grid;
    /* The shell exited while no window held the tab, which a window
     * then closes instead of showing. */
    bool exited;
};

/* The text the input method composes, drawn at the caret. */
struct terminal_preedit {
    char text[256];
    /* Byte offsets of the input method's caret in text; begin < 0 hides
     * it. */
    int cursor_begin, cursor_end;
    struct rect2i caret;
    uint32_t fg;
    uint32_t bg;
};

/* The widget over the terminal grid; see view.c. */
struct terminal_view {
    struct ui_widget widget;
    char *accessibility_text;
    size_t accessibility_caret_offset;
    size_t accessibility_selection_start;
    size_t accessibility_selection_end;
    uint64_t accessibility_version;
    const struct wsap_vtcore *accessibility_term;
    bool accessibility_text_valid;
    struct terminal_preedit preedit;
    float scroll_remainder;
    bool hover_link;
    bool drop_hover;
    /* A press opened a link: its release goes nowhere. */
    bool opening_link;
    uint64_t visual_bell_until_ms;
};

enum ctx_action {
    CTX_ACTION_NONE = 0,
    CTX_ACTION_COPY,
    CTX_ACTION_PASTE,
    CTX_ACTION_SELECT_ALL,
    CTX_ACTION_OPEN_LINK,
    CTX_ACTION_RESET_VIEW,
    CTX_ACTION_THEMES,
    CTX_ACTION_SETTINGS,
    CTX_ACTION_THEME_BASE = 100,
};

/* The content of a terminal window: tabs over a terminal view. */
struct terminal {
    struct window *window;
    /* The app's terminal config. The app keeps it for the window's life. */
    const struct terminal_config *config;
    struct wsap_vtcore_theme theme;
    struct terminal_theme_picker theme_picker;
    /* Opens windows for tear-off and New Window; see terminal_spec. */
    struct window *(*open_window)(void *ud, struct tab *tab, const char *cwd,
                                  struct vector2i size);
    void *ud;
    /* The window font and cursor values this terminal last applied. A
     * config change diffs against them. The family is owned. */
    char *applied_font_family;
    int applied_font_size;
    enum wsap_vtcore_cursor_style applied_cursor_style;
    bool applied_cursor_blink;
    struct tab **tabs;
    int tab_count;
    int tab_cap;
    int active_tab;

    /* The tab bar over the view, and the overlays; see root.c. */
    struct ui_widget root;
    struct ui_tabbar tabbar;
    /* The scrollview holds the view. Its bar shows the active tab's
     * scrollback. */
    struct ui_scrollview scrollview;
    struct terminal_view view;
    struct ui_palette palette;
    /* The hover card of an inactive tab, and the picture it shows. */
    struct ui_tab_preview tab_preview;
    struct ui_image preview_image;
    void *preview_state;
    terminal_preview_capture_fn preview_capture;
    terminal_preview_release_fn preview_release;
    void *preview_ud;
    terminal_scene_paint_fn scene_paint;
    void *scene_paint_ud;
    struct ui_tab_label *tab_labels;
    int tab_labels_cap;
    /* The copy of the active tab that the renderer paints. */
    struct wsap_vtcore_snapshot *snapshot;
    bool focused;
    /* The caret rectangle last sent to the input method, in device
     * pixels. Zero until the first send. */
    struct rect2i ime_caret;

    /* Where the context menu opened, and its theme submenu. */
    struct vector2i ctx_click;
    struct ui_list_item *menu_theme_items;
};

/* Identifies terminal windows. */
extern const int terminal_kind;

/* NULL unless w shows a terminal: other content is not a terminal. */
static inline struct terminal *terminal_of(const struct window *w)
{
    return w && window_ui_kind(w) == &terminal_kind ? window_ui_content(w) : NULL;
}

static inline struct tab *terminal_active_tab(const struct window *w)
{
    struct terminal *tw = terminal_of(w);
    return tw && tw->active_tab >= 0 && tw->active_tab < tw->tab_count
        ? tw->tabs[tw->active_tab] : NULL;
}

static inline struct wsap_vtcore *terminal_term(const struct window *w)
{
    struct tab *t = terminal_active_tab(w);
    return t ? t->session.term : NULL;
}

static inline const struct font_metrics *tab_metrics(const struct tab *t)
{
    return font_instance_metrics(t && t->font ? t->font : font_instance_default());
}

/* The cell the active tab draws, or the default font's. */
static inline const struct font_metrics *terminal_metrics(const struct window *w)
{
    return tab_metrics(terminal_active_tab(w));
}


/* font_size is in points; the window picks the scale. */
struct tab *tab_create(struct yasl_loop *loop, const struct session_spec *spec,
                       int font_size);
void tab_destroy(struct tab *t);

int terminal_add_tab(struct window *w, struct tab *t, int slot);
/* Removes tab index from w and returns it, closing w if it was the last. */
struct tab *terminal_take_tab(struct window *w, int index);
void terminal_finish_closing_tab(struct window *w);
void terminal_resize_sessions(struct window *w, struct vector2i px);

void terminal_adjust_font_size(struct window *w, int delta_pt);

/* root.c */
void terminal_root_init(struct terminal *tw);
/* Sends the chrome's actions to w. */
void terminal_root_attach(struct window *w);
void terminal_root_release(struct terminal *tw);

/* view.c */
void terminal_view_init(struct terminal *tw);
void terminal_view_attach(struct window *w);
void terminal_view_set_preedit(struct terminal *tw, const char *text,
                               int cursor_begin, int cursor_end,
                               struct rect2i caret, uint32_t fg, uint32_t bg);
/* Where the grid is, in surface space. */
struct rect2i terminal_view_rect(const struct window *w);
/* Turns a surface position into the active tab's cell. */
struct vector2i terminal_cell_at(const struct window *w, struct vector2i pos);
bool terminal_pointer_selects_text(const struct wsap_vtcore *t);

/* input.c */
struct wsap_vtcore_modes terminal_input_modes(const struct window *w);
/* Encodes a key for the program. True when it sent bytes. */
bool terminal_key(struct window *w, const struct input_key_event *key);
/* Sends typed text to the program. The palette and a menu keep it. */
void terminal_send_input(struct window *w, const char *data, size_t len);
/* Pastes text into the active session, bracketed when the program asks. */
void terminal_paste(struct window *w, const char *text, size_t len);
/* The native window gained or lost focus. */
void terminal_focus(struct window *w, bool focused);
void terminal_handle_shortcut(struct window *w, enum input_shortcut sc);
void terminal_copy(struct window *w);

/* draw.c */
void terminal_show_scrollbar(struct window *w);
struct ui_scroll_metrics terminal_scroll_metrics(const struct wsap_vtcore *term);

/* menu.c */
void terminal_open_ctx_menu(struct window *w, struct vector2i at);
void terminal_ctx_menu_perform(struct window *w, int action);
void terminal_select_all(struct window *w);

/* commands.c */
/* Sends the palette's commands to w. */
void terminal_palette_init(struct window *w);
void terminal_palette_toggle(struct window *w);

/* transfer.c */
/* Takes tab index out of w into a drag between windows. grab is where
 * the pointer holds the tab. False if no drag can start, which leaves
 * the gesture to the tab bar. */
bool terminal_tab_drag_begin(struct window *w, int index, struct vector2i grab);
/* The yasl drag events over w while a tab drag runs. ud is the window's
 * drag target data; the terminal ignores it. */
void terminal_tab_drag(struct window *w, enum yasl_drop_action ev,
                       struct vector2i pos, void *ud);
void terminal_view_drop_hover(struct window *w, bool over, void *ud);
/* w is going: a tab in flight stops pointing at it. */
void terminal_tab_flight_forget(struct window *w);

/* preview.c */
/* Gives the hover card its pictures. */
void terminal_tab_preview_init(struct window *w);
bool terminal_tab_preview_capture(struct window *w, int index, bool dragging);

/* blink.c */
/* When the cursor blink next flips, or 0 when the cursor does not blink. */
uint64_t terminal_next_blink_ms(struct window *w, uint64_t now_ms);

#endif
