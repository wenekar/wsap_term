#ifndef TERMINAL_INTERNAL_H
#define TERMINAL_INTERNAL_H

#include <stdalign.h>

#include "font/font.h"
#include "input/input.h"
#include "input/keybind.h"
#include "session/session.h"
#include "terminal/selection.h"
#include "terminal/terminal.h"
#include "ui/widgets/menu.h"
#include "ui/widgets/scrollview.h"
#include "ui/widget_impl.h"
#include "ui/yasl/client.h"
#include "ui/yasl/window.h"
#include <wsap_vtcore/wsap_vtcore.h>

static inline struct wsap_vtcore_vector2i term_vec_from_geom(struct vector2i v)
{
    return (struct wsap_vtcore_vector2i){ .x = v.x, .y = v.y };
}

struct terminal_preedit {
    char text[256];
    int cursor_begin, cursor_end;
    struct rect2i caret;
    uint32_t fg;
    uint32_t bg;
};

struct terminal_view {
    struct ui_widget widget;
    char *accessibility_text;
    size_t accessibility_caret_offset;
    size_t accessibility_selection_start;
    size_t accessibility_selection_end;
    uint64_t accessibility_version;
    const struct wsap_vtcore *accessibility_term;
    /* The time of the last text build, and the timer that sends the text
     * that a burst of output left behind. */
    uint64_t accessibility_built_ms;
    uint64_t accessibility_timer;
    bool accessibility_text_valid;
    struct terminal_preedit preedit;
    float scroll_remainder;
    bool hover_link;
    bool drop_hover;
    bool opening_link;
    bool blink_cells;
    /* The snapshot view version of the scan that set blink_cells. */
    uint64_t blink_version;
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

/* One terminal content: one session and its widgets. It has no tab state. */
struct terminal {
    struct window *window;
    /* The loop of the window at creation. The content keeps it, because a
     * detached content still owns timers. */
    struct yasl_loop *loop;
    const struct terminal_config *config;
    struct terminal_host host;
    struct wsap_vtcore_theme theme;
    enum wsap_vtcore_color_scheme host_scheme;
    struct terminal_theme_picker theme_picker;
    char *applied_font_family;
    int applied_font_size;
    enum wsap_vtcore_cursor_style applied_cursor_style;
    bool applied_cursor_blink;

    struct session session;
    struct font_instance *font;
    int font_size;
    struct text_selection selection;
    /* The last grid that the content widget gave the session. */
    struct vector2i grid;
    bool exited;

    struct ui_scrollview scrollview;
    struct terminal_view view;

    struct wsap_vtcore_snapshot *snapshot;
    /* terminal_prepare() updates the snapshot once for each frame. It hands
     * that update to terminal_frame_source() in the same frame. These fields
     * hold the frame serial and the terminal of the waiting update.
     * snapshot_frame is zero when no update waits. */
    uint64_t frame_serial;
    uint64_t snapshot_frame;
    const struct wsap_vtcore *snapshot_term;
    bool focused;
    struct rect2i ime_caret;

    struct vector2i ctx_click;
    struct ui_list_item *menu_theme_items;
};

static inline struct terminal *terminal_of(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct terminal,
                           scrollview.widget);
}

static inline struct terminal *terminal_of_view(const struct ui_widget *widget)
{
    return ui_container_of((struct ui_widget *)widget, struct terminal,
                           view.widget);
}

static inline const struct font_metrics *terminal_font_metrics(
    const struct terminal *terminal)
{
    return font_instance_metrics(terminal && terminal->font
                                     ? terminal->font
                                     : font_instance_default());
}

void terminal_view_init(struct terminal *terminal);
void terminal_view_attach(struct terminal *terminal);
struct rect2i terminal_view_rect(const struct terminal *terminal);
struct vector2i terminal_cell_at(const struct terminal *terminal,
                                 struct vector2i pos);
bool terminal_pointer_selects_text(const struct wsap_vtcore *t);
void terminal_view_drop_hover(struct terminal *terminal, bool over, void *ud);
void terminal_view_set_preedit(struct terminal *terminal, const char *text,
                               int cursor_begin, int cursor_end,
                               struct rect2i caret, uint32_t fg, uint32_t bg);

/* The draw stage of the frame. */
bool terminal_cursor_visible(const struct terminal *terminal, uint64_t now_ms);
void terminal_update_ime(struct terminal *terminal,
                         const struct wsap_vtcore *term);
void terminal_scan_blink(struct terminal_view *view,
                         const struct wsap_vtcore_snapshot *snap);

struct wsap_vtcore_modes terminal_input_modes(const struct terminal *terminal);
void terminal_send_input(struct terminal *terminal, const char *data,
                         size_t len);
bool terminal_key(struct terminal *terminal,
                  const struct input_key_event *key);
void terminal_text(struct terminal *terminal, const char *text, size_t len);
void terminal_copy(struct terminal *terminal);
void terminal_select_all(struct terminal *terminal);

struct ui_scroll_metrics terminal_scroll_metrics(const struct wsap_vtcore *term);

void terminal_open_ctx_menu(struct terminal *terminal, struct vector2i at);
void terminal_ctx_menu_perform(struct terminal *terminal, int action);

uint64_t terminal_next_blink_ms(struct terminal *terminal, uint64_t now_ms);
bool terminal_text_blink_on(const struct terminal *terminal, uint64_t now_ms);
uint64_t terminal_next_text_blink_ms(struct terminal *terminal,
                                     uint64_t now_ms);

#endif
